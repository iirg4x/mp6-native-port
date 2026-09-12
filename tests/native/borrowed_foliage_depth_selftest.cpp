#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Size { uint32_t width=0, height=0; };
struct Pass { bool hasDepth=false; int copySourceDepthView=0; Size targetSize; uint32_t msaaSamples=0; };
struct BorrowedPassDepth { int view=0; uint32_t width=0,height=0,sampleCount=0; };
struct ResolveDesc { bool color=true,depth=false; };
struct ResolvedTargets {};
static void* g_recordingFrame=nullptr;
static uint32_t g_currentRenderPass=UINT32_MAX;
static bool g_inOffscreen=false, resolveOK=true, createOK=true;
static std::vector<Pass> passes;
static std::string events;
namespace gx::fifo { void drain() { events += "drain;"; } }
auto& current_render_passes() { return passes; }
bool resolve_pass(const ResolveDesc& desc, ResolvedTargets&) {
    assert(!desc.color && !desc.depth);
    events += "seal;";
    if (!resolveOK) return false;
    // Invalidate both the reference and values the implementation sampled.
    passes.clear(); passes.shrink_to_fit();
    passes.push_back({true,99,{1,1},1});
    return true;
}
bool create_pass(uint32_t width,uint32_t height) {
    events += "open;";
    assert(width==2340 && height==1080);
    g_inOffscreen=createOK;
    return createOK;
}
#include "borrowed_depth.inc"
void reset() {
    g_recordingFrame=&passes; g_currentRenderPass=0; g_inOffscreen=false;
    resolveOK=createOK=true; events.clear();
    passes={{true,17,{2340,1080},1}};
}
int main() {
    BorrowedPassDepth out;
    for (unsigned samples : {1u,4u}) {
        reset(); passes[0].msaaSamples=samples;
        assert(create_pass_from_efb_depth(out));
        assert(out.view==17 && out.width==2340 && out.height==1080 && out.sampleCount==samples);
        assert(events=="drain;seal;open;" && g_inOffscreen);
    }
    for (int invalid=0;invalid<7;++invalid) {
        reset(); out={42,42,42,42};
        if (invalid==0) g_recordingFrame=nullptr;
        if (invalid==1) g_currentRenderPass=UINT32_MAX;
        if (invalid==2) g_inOffscreen=true;
        if (invalid==3) passes[0].hasDepth=false;
        if (invalid==4) passes[0].copySourceDepthView=0;
        if (invalid==5) passes[0].targetSize.width=0;
        if (invalid==6) passes[0].msaaSamples=0;
        assert(!create_pass_from_efb_depth(out));
        assert(out.view==0 && out.width==0 && out.sampleCount==0 && events=="drain;");
    }
    reset(); resolveOK=false; out={42,42,42,42};
    assert(!create_pass_from_efb_depth(out) && out.view==0 && events=="drain;seal;");
    reset(); createOK=false; out={42,42,42,42};
    assert(!create_pass_from_efb_depth(out) && out.view==0 && events=="drain;seal;open;" && !g_inOffscreen);
}
