#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#define ZoneScoped ((void)0)
#define ASSERT(x, ...) assert(x)
#define UNLIKELY
namespace magic_enum { template<class T> int enum_name(T) { return 0; } }
struct Logger { template<class... T> void warn(const char*, T...) {} } Log;
namespace wgpu {
enum class StoreOp { Store, Discard };
struct Color { double r,g,b,a; };
}
struct Handle {
    int value=0;
    Handle()=default;
    Handle(std::nullptr_t) {}
    Handle(int x):value(x) {}
    operator bool() const { return value!=0; }
};
using TextureHandle=Handle;
using GXTexFmt=int;
template<class T> struct Vec4 {
    T v[4]{};
    T x() const { return v[0]; } T y() const { return v[1]; }
    T z() const { return v[2]; } T w() const { return v[3]; }
};
struct Size { uint32_t width=640,height=480,depthOrArrayLayers=1; };
struct ClipRect { int x=0,y=0,width=640,height=480; };
enum class CommandType { SetViewport,SetScissor,Draw,CustomDraw };
struct Command {
    CommandType type;
    struct Data { int setViewport=0,setScissor=0; } data;
};
using CommandList=std::vector<Command>;
struct RenderPass {
    std::string label;
    Handle colorView,resolveView,depthStencilView,copySourceTexture,copySourceView,copySourceDepthView;
    Size targetSize;
    uint32_t msaaSamples=1;
    Vec4<float> clearColorValue;
    float clearDepthValue=1;
    Handle unchangedDepthSnapshot;
    bool clearColor=true,clearDepth=true,hasDepth=true,hasStencil=false;
    bool sealed=false,hasDraws=false,captureDepthSnapshot=false,finalEfb=false;
    bool msResolveCurrent=false,skipRenderPass=false;
    Handle resolveTarget,snapshotColorDst,snapshotDepthDst;
    GXTexFmt resolveFormat=0;
    ClipRect resolveRect;
    int resolveUniformRange=0;
    wgpu::StoreOp colorStoreOp=wgpu::StoreOp::Store,depthStoreOp=wgpu::StoreOp::Store;
    CommandList commands;
    bool has_content() const { return hasDraws||clearColor||clearDepth; }
};
struct Uniforms { void append_zeroes(size_t) {} };
struct FramePacket {
    std::deque<RenderPass> renderPasses;
    std::vector<CommandList> recycledCommands;
    Uniforms uniforms;
} frame;
FramePacket* g_recordingFrame=&frame;
uint32_t g_currentRenderPass=0;
size_t g_recordingFrameSlot=0;
bool g_inOffscreen=false;
int g_cachedViewport=73,g_cachedScissor=91;
auto& current_frame_packet() { return frame; }
auto& current_render_passes() { return frame.renderPasses; }
std::string pass_label(const char* s) { return s; }
bool ao_compatibility_mode() { return false; }
namespace gx {
constexpr size_t MaxUniformSize=32;
bool is_depth_format(int format) { return format==42; }
}
int push_uniform(const std::array<float,4>&) { return 1; }
namespace clear {
struct PipelineConfig { uint32_t msaaSamples;bool clearColor,clearAlpha,clearDepth; };
struct DrawData { int pipeline;wgpu::Color color; };
}
int pipeline_ref(clear::PipelineConfig) { return 1; }
static void push_command(CommandType,const Command::Data&);
void push_draw_command(clear::DrawData) { push_command(CommandType::Draw,{}); }
void enqueue_pass(FramePacket& packet,size_t,uint32_t i) { packet.renderPasses[i].sealed=true; }
#include "subject.inc"

void continuation_tests() {
    for(int kind=0;kind<3;++kind) for(unsigned samples:{1,4}) for(unsigned mask=0;mask<8;++mask) {
        if(kind==1 && mask==0)continue;
        frame={};
        frame.recycledCommands.emplace_back();
        frame.recycledCommands.back().reserve(16);
        auto* allocation=frame.recycledCommands.back().data();
        for(int n=0;n<300;++n) {
            frame.renderPasses.emplace_back();
            g_currentRenderPass=0;
            frame.renderPasses[0].msaaSamples=samples;
            frame.renderPasses[0].copySourceDepthView=Handle(123);
            if(kind==0)resolve_pass_into(Handle(1),{},mask&1,mask&2,mask&4,{},1,0);
            if(kind==1)replay_copy_clear(mask&1,mask&2,mask&4,{},1);
            if(kind==2)resume_efb_pass_loading(frame.renderPasses[0]);
            assert(frame.renderPasses.size()==2 && g_currentRenderPass==1);
            auto& pass=frame.renderPasses.back();
            assert(pass.commands.data()==allocation && pass.commands.capacity()==16);
            assert(pass.commands.size()==(kind!=2 && bool(mask&1)!=bool(mask&2) ? 3u : 2u));
            assert(pass.commands[pass.commands.size()-2].data.setViewport==73);
            assert(pass.commands.back().data.setScissor==91);
            assert(pass.copySourceDepthView.value==123 && pass.msaaSamples==samples);
            recycle_frame_packet(frame);
            assert(frame.renderPasses.empty() && frame.recycledCommands.size()==1);
            assert(frame.recycledCommands[0].empty());
        }
    }
}
void final_depth_tests() {
    for(bool final:{false,true})for(bool peek:{false,true})
    for(bool snapshot:{false,true})for(int copy=0;copy<3;++copy) {
        RenderPass pass;
        pass.finalEfb=final;
        if(snapshot)pass.snapshotDepthDst=Handle(1);
        if(copy) { pass.resolveTarget=Handle(1);pass.resolveFormat=copy==2?42:0; }
        const bool discard=final&&!peek&&!snapshot&&copy!=2;
        assert(depth_store_op(pass,peek)==(discard?wgpu::StoreOp::Discard:wgpu::StoreOp::Store));
    }
    frame={};frame.renderPasses.emplace_back();g_currentRenderPass=0;
    finish();
    assert(frame.renderPasses[0].finalEfb && frame.renderPasses[0].captureDepthSnapshot);
    assert(frame.renderPasses[0].sealed && g_currentRenderPass==UINT32_MAX);
    finish(); // no active pass
    g_recordingFrame=nullptr;finish();g_recordingFrame=&frame;
}
int main() {
    continuation_tests();final_depth_tests();
    puts("PASS: all three real continuations reuse storage; final-depth consumers retained");
}
