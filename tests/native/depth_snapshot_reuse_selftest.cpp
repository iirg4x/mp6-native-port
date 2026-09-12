#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <deque>
#include <mutex>
#include <cstring>
#define ZoneScoped ((void)0)
#define CHECK(condition, ...) assert(condition)
namespace wgpu { enum class StoreOp { Store, Discard }; }

struct Handle {
    int id=0;
    Handle()=default;
    Handle(std::nullptr_t) {}
    Handle(int value):id(value) {}
    operator bool() const { return id!=0; }
    bool operator==(Handle other) const { return id==other.id; }
};
struct Size { uint32_t width=2960,height=1848,depthOrArrayLayers=1; };
using EncoderTaskId=uint32_t;
constexpr EncoderTaskId InvalidEncoderTask=0;
constexpr size_t InlineDrawPayloadSize=64;
struct EncoderTask {
    EncoderTaskId type=0;
    Handle depth;
    Size targetSize{0,0,0};
    uint32_t sampleCount=1,payloadSize=0;
    std::array<std::byte,InlineDrawPayloadSize> payload;
};
struct FramePacket { std::deque<EncoderTask> encoderTasks; };
struct RenderPass {
    std::string label;
    Handle colorView,resolveView,depthStencilView,copySourceTexture,copySourceView,copySourceDepthView;
    Size targetSize;
    uint32_t msaaSamples=4;
    Handle resolveTarget,snapshotColorDst,snapshotDepthDst,unchangedDepthSnapshot;
    bool colorSnapshotTarget=false,colorSnapshotExported=false;
    int resolveFormat=0;
    bool captureDepthSnapshot=false;
    wgpu::StoreOp depthStoreOp=wgpu::StoreOp::Store;
    bool deferColorResolve=false,clearColor=true,clearDepth=true,hasDepth=true,hasStencil=false;
    bool hasDraws=false,discardable=false;
    bool depthReadOnly=false;
    std::vector<int> commands;
    bool has_content() const { return hasDraws || clearColor || clearDepth; }
    bool has_consumer() const { return resolveTarget || snapshotColorDst || snapshotDepthDst || colorSnapshotExported; }
};
struct ResolveDesc { bool color=false,depth=false; };
struct ResolvedTargets { Handle color,depth; int colorFormat=0; uint32_t width=0,height=0; };
struct Entry { struct Texture { Handle texture,view; int format=1; } color,depth; };
Entry::Texture g_offscreenColor,g_offscreenDepth;
std::optional<RenderPass> g_suspendedEfbPass;
FramePacket frame;
int copies=0,depthCopies=0,colorCopies=0,nextHandle=1,closedOffscreen=0;
FramePacket* g_recordingFrame=&frame;
uint32_t g_currentRenderPass=0;
size_t g_recordingFrameSlot=0;
bool g_inOffscreen=false,supported=true;
std::vector<RenderPass> passes;
auto& current_render_passes() { return passes; }
auto& current_frame_packet() { return frame; }
namespace gx::fifo {
int drains=0;
void drain() { ++drains; }
uint64_t efb_depth_write_serial() { return 17; }
}
namespace gx { bool is_depth_format(int format) { return format==42; } }
namespace tex_copy_conv { bool snapshot_depth_supported() { return supported; } }
namespace webgpu { struct { uint32_t vendorID=0; } g_adapterInfo; }
struct Logger { template<class... T> void warn(const char*,T...) {} } Log;
std::string pass_label(const char* label) { return label; }
int g_cachedViewport=0,g_cachedScissor=0;
int g_suspendedEfbViewport=0,g_suspendedEfbScissor=0;
enum class CommandType { SetViewport,SetScissor };
struct Command { struct Data { int setViewport=0,setScissor=0; }; };
void push_command(CommandType,const Command::Data&) {}
std::vector<int> taskOrder;
void enqueue_pass(FramePacket&,size_t,uint32_t) { taskOrder.push_back(1);if(g_inOffscreen) ++closedOffscreen; }
std::mutex g_runtimeDrawTypeMutex;
struct RuntimeTask { bool readsDepth; } readTask{true},ordinaryTask{false};
RuntimeTask* find_runtime_encoder_task_type(EncoderTaskId id) {
    return id==1 ? &readTask : id==2 ? &ordinaryTask : nullptr;
}
enum class FrameOpType { EncoderTask };
struct FrameOp { uint32_t index; };
FrameOp capture_frame_op(FramePacket&,FrameOpType,uint32_t index) { return {index}; }
void enqueue_op(FramePacket& packet,size_t,FrameOp op) {
    assert(op.index<packet.encoderTasks.size());
    taskOrder.push_back(2);
}
void set_efb_targets(RenderPass&) {}
void begin_offscreen(uint32_t width,uint32_t height) {
    assert(!g_inOffscreen);
    g_inOffscreen=true;
    passes.emplace_back();
    g_currentRenderPass=uint32_t(passes.size()-1);
    passes.back().targetSize={width,height,1};
    passes.back().msaaSamples=1;
}
Entry& acquire_pass_snapshot(uint32_t,uint32_t,bool color,bool depth) {
    static Entry entry;
    ++copies;
    if(color) { ++colorCopies; entry.color.texture=entry.color.view=Handle(nextHandle++); }
    if(depth) { ++depthCopies; entry.depth.view=Handle(nextHandle++); }
    return entry;
}
// Target allocation is exercised separately with the actual implementation.
void begin_offscreen_target(uint32_t width,uint32_t height,bool snapshotTarget) {
    begin_offscreen(width,height);
    assert(snapshotTarget);
    g_offscreenColor=acquire_pass_snapshot(width,height,true,false).color;
    auto& pass=passes.back();
    pass.colorView=pass.copySourceView=g_offscreenColor.view;
    pass.copySourceTexture=g_offscreenColor.texture;
    pass.colorSnapshotTarget=true;
}
static void resume_efb_pass_loading(const RenderPass&);
#include "depth_snapshot_subject.inc"
#include "encoder_task_subject.inc"

void new_frame() {
    passes.clear(); passes.emplace_back(); g_currentRenderPass=0;
    g_inOffscreen=false; supported=true; g_recordingFrame=&frame;
    g_suspendedEfbPass.reset();
    frame.encoderTasks.clear();taskOrder.clear();
}
void ordered_depth_tests() {
    for(unsigned samples:{1,4})for(bool empty:{false,true}) {
        new_frame();
        auto& pass=passes[0];
        pass.copySourceDepthView=Handle(77);
        pass.depthStencilView=Handle(77);
        pass.msaaSamples=samples;pass.targetSize={511,257,1};
        pass.clearColor=pass.clearDepth=!empty;
        pass.depthStoreOp=wgpu::StoreOp::Discard;
        uint64_t payload=0x0123456789abcdefull;
        assert(push_encoder_task(1,&payload,sizeof(payload)));
        assert(taskOrder==std::vector<int>({1,2}));
        const auto& task=frame.encoderTasks[0];
        assert(task.depth==Handle(77) && task.sampleCount==samples);
        assert(task.targetSize.width==511 && task.targetSize.height==257);
        assert(task.payloadSize==sizeof(payload) && !std::memcmp(task.payload.data(),&payload,sizeof(payload)));
        assert(!passes[0].discardable && passes[0].depthStoreOp==wgpu::StoreOp::Store);
        assert(passes.size()==2 && !passes[1].clearDepth && !passes[1].clearColor);
        assert(passes[1].copySourceDepthView==Handle(77) && passes[1].msaaSamples==samples);
        assert(push_encoder_task(2,nullptr,0));
        assert(!frame.encoderTasks[1].depth && frame.encoderTasks[1].targetSize.width==0);
    }
    new_frame();
    assert(!push_encoder_task(1,nullptr,0)); // No attachment to borrow.
    assert(taskOrder.empty() && frame.encoderTasks.empty());
    passes[0].copySourceDepthView=Handle(77);
    for(int invalid=0;invalid<6;++invalid) {
        auto id=1u;const void* data=nullptr;size_t bytes=0;
        if(invalid==0)id=InvalidEncoderTask;
        if(invalid==1)id=100;
        if(invalid==2)bytes=1;
        if(invalid==3)bytes=InlineDrawPayloadSize+1;
        if(invalid==4)g_inOffscreen=true;
        if(invalid==5)g_recordingFrame=nullptr;
        assert(!push_encoder_task(id,data,bytes));
        assert(taskOrder.empty() && frame.encoderTasks.empty());
        g_inOffscreen=false;g_recordingFrame=&frame;
    }
    puts("PASS: ordered depth task, empty-pass stores, samples, dimensions and invalid admission");
}
void direct_offscreen_tests() {
    new_frame();
    const int oldCopies=copies;
    assert(!create_pass(0,100) && !create_pass(100,0) && copies==oldCopies);
    g_recordingFrame=nullptr;
    assert(!create_pass(100,100));
    g_recordingFrame=&frame;
    Handle lastColor;
    for(int i=0;i<20;++i) {
        assert(create_pass(2340+i,1080));
        const uint32_t index=g_currentRenderPass;
        auto color=passes[index].copySourceView;
        assert(color && !(color==lastColor));
        assert(passes[index].colorView==color && passes[index].colorSnapshotTarget);
        assert(!create_pass(640,480)); // no nested allocation / target replacement
        const int afterCreate=copies;
        ResolvedTargets result;
        assert(resolve_pass({true,false},result));
        assert(result.color==color && result.width==uint32_t(2340+i) && result.height==1080);
        assert(!result.depth && copies==afterCreate && !passes[index].snapshotColorDst);
        assert(passes[index].colorSnapshotExported && !passes[index].discardable);
        assert(passes[index].depthStoreOp==wgpu::StoreOp::Discard);
        assert(!g_inOffscreen);
        lastColor=color;
    }
    // Color+depth exports retain the exact R32 depth snapshot and depth store.
    assert(create_pass(511,257));
    auto index=g_currentRenderPass;
    auto color=passes[index].copySourceView;
    const int beforeDepth=depthCopies,beforeColor=colorCopies;
    ResolvedTargets result;
    assert(resolve_pass({true,true},result));
    assert(result.color==color && result.depth && depthCopies==beforeDepth+1 && colorCopies==beforeColor);
    assert(passes[index].depthStoreOp==wgpu::StoreOp::Store);
    // No requested outputs still discards the offscreen pass.
    // Read-only EFB depth is not the disposable private offscreen buffer.
    assert(create_pass(511,257));index=g_currentRenderPass;
    passes[index].depthReadOnly=true;
    assert(resolve_pass({true,false},result));
    assert(passes[index].depthStoreOp==wgpu::StoreOp::Store);
    assert(!passes.back().depthReadOnly);
    assert(create_pass(511,257));index=g_currentRenderPass;
    assert(resolve_pass({false,false},result));
    assert(!result.color && !result.depth && passes[index].discardable);
    // Legacy targets are reused; they still copy on export, never alias.
    begin_offscreen(511,257);index=g_currentRenderPass;
    const int beforeLegacy=colorCopies;
    assert(resolve_pass({true,false},result) && colorCopies==beforeLegacy+1);
    assert(passes[index].snapshotColorDst && !passes[index].colorSnapshotExported);
    // A peek or GX depth consumer must retain stored depth regardless of export.
    for(int consumer=0;consumer<3;++consumer) {
        assert(create_pass(511,257));index=g_currentRenderPass;
        if(consumer==0) passes[index].captureDepthSnapshot=true;
        if(consumer==1) { passes[index].resolveTarget=Handle(99);passes[index].resolveFormat=42; }
        if(consumer==2) { passes[index].resolveTarget=Handle(99);passes[index].resolveFormat=0; }
        assert(resolve_pass({true,false},result));
        assert(passes[index].depthStoreOp==(consumer==2?wgpu::StoreOp::Discard:wgpu::StoreOp::Store));
    }
    // Restoring a suspended EFB keeps its own depth, not the disposable target.
    assert(create_pass(511,257));
    g_suspendedEfbPass=RenderPass{};
    g_suspendedEfbPass->depthStoreOp=wgpu::StoreOp::Store;
    assert(resolve_pass({true,false},result));
    assert(passes.back().depthStoreOp==wgpu::StoreOp::Store && !g_suspendedEfbPass);
    puts("PASS: direct color snapshots, distinct frame images, depth consumers and legacy fallback");
}
int main(int argc, char** argv) {
    assert(argc == 2);
    assert(efb_depth_write_serial()==17 && gx::fifo::drains==1);
    assert(copies==0); /* serial queries drain FIFO, not framebuffer snapshots */
    webgpu::g_adapterInfo.vendorID = std::strtoul(argv[1], nullptr, 0);
#ifdef __ANDROID__
    assert(ao_compatibility_mode() == (webgpu::g_adapterInfo.vendorID != 0x5143));
#else
    assert(!ao_compatibility_mode());
#endif
    if (ao_compatibility_mode()) {
    // Recovery policy: match the pre-0.4.10 schedule even for empty segments.
    new_frame();
    ResolvedTargets first, second, both;
    assert(resolve_pass({false,true},first) && first.depth && depthCopies==1);
    assert(!passes[0].deferColorResolve && !passes.back().unchangedDepthSnapshot);
    assert(resolve_pass({false,true},second) && second.depth && depthCopies==2);
    assert(!(first.depth==second.depth) && !passes[1].discardable);
    assert(resolve_pass({true,true},both) && both.color && both.depth);
    assert(depthCopies==3 && colorCopies==1 && !passes[2].deferColorResolve);
    passes.back().hasDraws=true;
    assert(resolve_pass({false,true},second) && depthCopies==4);
    g_inOffscreen=true;
    assert(resolve_pass({false,true},second) && depthCopies==5 && closedOffscreen==1);
    new_frame();
    assert(resolve_pass({false,true},second) && depthCopies==6);
    supported=false;
    assert(resolve_pass({false,true},second) && !second.depth);
    puts("PASS: Android pre-0.4.10 depth copy/resolve schedule retained");
    direct_offscreen_tests();
    ordered_depth_tests();
    return 0;
    }
    {
    new_frame();
    ResolvedTargets first,reused,changed,both;
    assert(resolve_pass({false,true},first));
    assert(first.depth && depthCopies==1 && first.width==2960 && first.height==1848);
    assert(passes[0].deferColorResolve && !passes.back().deferColorResolve);
    assert(resolve_pass({false,true},reused));
    assert(reused.depth==first.depth && depthCopies==1 && passes[1].discardable);
    assert(resolve_pass({true,true},both));
    assert(both.depth==first.depth && both.color && depthCopies==1 && colorCopies==1);
    assert(!passes[2].deferColorResolve);
    passes.back().hasDraws=true;
    assert(resolve_pass({false,true},changed));
    assert(!(changed.depth==first.depth) && depthCopies==2);
    for(int clear=0;clear<2;++clear) {
        if(clear) passes.back().clearColor=true; else passes.back().clearDepth=true;
        int before=depthCopies;
        assert(resolve_pass({false,true},changed) && depthCopies==before+1);
    }
    // Actual offscreen suspension moves the unchanged loading pass out/back.
    RenderPass suspended=std::move(passes.back());
    passes.pop_back(); passes.push_back(std::move(suspended));
    int before=depthCopies;
    assert(resolve_pass({false,true},reused) && depthCopies==before);
    // Offscreen targets must never borrow an EFB snapshot.
    g_inOffscreen=true;
    assert(resolve_pass({false,true},changed) && depthCopies==before+1 && closedOffscreen==1);
    new_frame();
    before=depthCopies;
    assert(resolve_pass({false,true},changed) && depthCopies==before+1);
    supported=false;
    assert(resolve_pass({false,true},reused) && !reused.depth);
    g_recordingFrame=nullptr;
    assert(!resolve_pass({false,true},reused) && !reused.depth);
    puts("PASS: exact depth reuse, draws/clears/frame invalidation, color consumers and unsupported snapshots");
    direct_offscreen_tests();
    ordered_depth_tests();
    }
}
