#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
namespace wgpu {
enum class LoadOp { Undefined, Clear, Load };
enum class StoreOp { Undefined, Store, Discard };
struct RenderPassDepthStencilAttachment {
    int view=0;
    LoadOp depthLoadOp=LoadOp::Undefined;
    StoreOp depthStoreOp=StoreOp::Undefined;
    float depthClearValue=0;
    bool depthReadOnly=false;
    LoadOp stencilLoadOp=LoadOp::Undefined;
    StoreOp stencilStoreOp=StoreOp::Undefined;
    uint32_t stencilClearValue=0;
    bool stencilReadOnly=false;
};
}
struct Size { uint32_t width=2340,height=1080; };
struct Pass {
    bool hasDepth=true; int depthStencilView=17; Size targetSize; uint32_t msaaSamples=1;
    bool depthReadOnly=false,clearDepth=true,hasStencil=false;
    wgpu::LoadOp depthLoadOp=wgpu::LoadOp::Undefined,stencilLoadOp=wgpu::LoadOp::Clear;
    wgpu::StoreOp stencilStoreOp=wgpu::StoreOp::Discard;
    float clearDepthValue=.75f; uint32_t stencilClearValue=3;
};
struct ResolveDesc { bool color=true,depth=false; };
struct ResolvedTargets {};
static void* g_recordingFrame=nullptr;
static uint32_t g_currentRenderPass=UINT32_MAX;
static bool g_inOffscreen=false,resolveOK=true;
static std::vector<Pass> passes;
static std::string events;
namespace gx::fifo { void drain() { events+="drain;"; } }
auto& current_render_passes() { return passes; }
bool resolve_pass(const ResolveDesc& desc,ResolvedTargets&) {
    assert(!desc.color && !desc.depth);events+="seal;";
    if(!resolveOK)return false;
    passes.clear();passes.shrink_to_fit();passes.push_back({false,99,{1,1},4});
    return true;
}
void begin_offscreen_target(uint32_t width,uint32_t height,bool snapshot,int depth) {
    assert(width==2340 && height==1080 && snapshot && depth==17);
    events+="open;";g_inOffscreen=true;
}
wgpu::StoreOp depth_store_op(const Pass&,bool) { return wgpu::StoreOp::Store; }
#include "subject.inc"
void reset() {
    g_recordingFrame=&passes;g_currentRenderPass=0;g_inOffscreen=false;
    resolveOK=true;events.clear();passes={Pass{}};
}
int main() {
    uint32_t w=0,h=0;
    reset();assert(create_pass_with_readonly_efb_depth(w,h));
    assert(w==2340 && h==1080 && g_inOffscreen && events=="drain;seal;open;");
    for(int invalid=0;invalid<9;++invalid) {
        reset();w=h=42;
        if(invalid==0)g_recordingFrame=nullptr;
        if(invalid==1)g_currentRenderPass=UINT32_MAX;
        if(invalid==2)g_inOffscreen=true;
        if(invalid==3)passes[0].hasDepth=false;
        if(invalid==4)passes[0].depthStencilView=0;
        if(invalid==5)passes[0].targetSize.width=0;
        if(invalid==6)passes[0].targetSize.height=0;
        if(invalid==7)passes[0].msaaSamples=0;
        if(invalid==8)passes[0].msaaSamples=4;
        assert(!create_pass_with_readonly_efb_depth(w,h));
        assert(w==0 && h==0 && events=="drain;");
    }
    reset();resolveOK=false;
    assert(!create_pass_with_readonly_efb_depth(w,h) && !w && !h && !g_inOffscreen);
    assert(events=="drain;seal;");
    for(bool readOnly:{false,true})for(bool hasDepth:{false,true})
    for(bool hasStencil:{false,true})for(bool clear:{false,true}) {
        Pass pass;pass.depthReadOnly=readOnly;pass.hasDepth=hasDepth;
        pass.hasStencil=hasStencil;pass.clearDepth=clear;
        const auto d=descriptor(pass);
        assert(d.view==17 && d.depthReadOnly==readOnly && d.stencilReadOnly==readOnly);
        assert(d.depthLoadOp==((readOnly||!hasDepth)?wgpu::LoadOp::Undefined:
            clear?wgpu::LoadOp::Clear:wgpu::LoadOp::Load));
        assert(d.depthStoreOp==((readOnly||!hasDepth)?wgpu::StoreOp::Undefined:wgpu::StoreOp::Store));
        assert(d.stencilLoadOp==((readOnly||!hasStencil)?wgpu::LoadOp::Undefined:wgpu::LoadOp::Clear));
        assert(d.stencilStoreOp==((readOnly||!hasStencil)?wgpu::StoreOp::Undefined:wgpu::StoreOp::Discard));
    }
}
