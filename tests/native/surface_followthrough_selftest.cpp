#include <cassert>
#include <cstdio>
#include <utility>
#define ZoneScopedN(x) ((void)0)
namespace wgpu {
enum class SurfaceGetCurrentTextureStatus { SuccessOptimal,SuccessSuboptimal,Timeout,Outdated,Lost,Error };
enum class Status { Error,Success };
struct ConvertibleStatus {
    Status value;
    ConvertibleStatus(Status s):value(s) {}
    operator bool() const { return value==Status::Success; }
};
struct TextureView { bool valid=false;operator bool() const { return valid; } };
struct Texture {
    bool valid=false;
    operator bool() const { return valid; }
    TextureView CreateView() { return {valid}; }
};
struct SurfaceTexture { SurfaceGetCurrentTextureStatus status;Texture texture; };
}
using Status=wgpu::SurfaceGetCurrentTextureStatus;
Status suppliedStatus=Status::SuccessOptimal;
bool suppliedTexture=true,presentSuccess=true;
int acquired=0,presented=0,refreshes=0,released=0,completed=0;
namespace window {
bool presentable=true;
struct SurfaceLock {};
enum class CustomEvent { RefreshSurface };
bool is_presentable() { return presentable; }
void push_custom_event(CustomEvent) { assert(presented==0||presentSuccess);++refreshes; }
}
struct Surface {
    bool valid=false;
    operator bool() const { return valid; }
    void GetCurrentTexture(wgpu::SurfaceTexture* texture) {
        ++acquired;*texture={suppliedStatus,{suppliedTexture}};
    }
    wgpu::ConvertibleStatus Present() {
        ++presented;
        return presentSuccess?wgpu::Status::Success:wgpu::Status::Error;
    }
} g_surface;
namespace webgpu { void release_surface() { ++released;g_surface={}; } }
namespace gfx { void after_present() { ++completed; } }
namespace magic_enum { template<class T> int enum_name(T) { return 0; } }
struct Logger {
    template<class... T> void warn(const char*,T...) {}
    template<class... T> void info(const char*,T...) {}
    template<class... T> void error(const char*,T...) {}
} Log;
#include "subject.inc"
int main() {
    for(Status status:{Status::SuccessOptimal,Status::SuccessSuboptimal,Status::Timeout,
                       Status::Outdated,Status::Lost,Status::Error}) {
        for(bool success:{false,true}) {
            suppliedStatus=status;presentSuccess=success;g_surface.valid=true;
            acquired=presented=refreshes=released=completed=0;
            present_frame();
            const bool usable=status==Status::SuccessOptimal||status==Status::SuccessSuboptimal;
            assert(acquired==1 && presented==int(usable));
            assert(completed==int(usable&&success));
            assert(refreshes==int(status==Status::Outdated||(status==Status::SuccessSuboptimal&&success)));
            assert(released==int(status==Status::Lost||(usable&&!success)));
        }
    }
    suppliedStatus=Status::SuccessSuboptimal;presentSuccess=true;
    g_surface.valid=true;suppliedTexture=false;presented=refreshes=0;
    present_frame();assert(presented==0&&refreshes==1);
    window::presentable=false;g_surface.valid=true;acquired=presented=0;
    present_frame();assert(acquired==0&&presented==0);
    puts("PASS: optimal/suboptimal present; recovery statuses and failures preserve behavior");
}
