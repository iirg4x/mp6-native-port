#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#define ZoneScoped ((void)0)
#define CHECK(x, ...) assert(x)
#define ASSERT(x, ...) assert(x)
#define AURORA_ALIGN(x, a) (((x)+(a)-1)&~((a)-1))

struct Handle {
    std::shared_ptr<int> value;
    Handle() = default;
    Handle(int id): value(std::make_shared<int>(id)) {}
    operator bool() const { return bool(value); }
    int id() const { return value ? *value : 0; }
    bool operator==(const Handle& rhs) const { return value == rhs.value; }
    Handle CreateView() const { return *this; }
};
namespace wgpu {
using BindGroup = Handle;
using TextureView = Handle;
enum class StoreOp { Store, Discard };
enum TextureUsage { RenderAttachment=1, TextureBinding=2 };
struct Extent3D { uint32_t width=0,height=0,depthOrArrayLayers=1; };
struct TextureDescriptor { const char* label; int usage; Extent3D size; int format; };
struct BindGroupEntry {
    uint32_t binding=0; Handle buffer; uint64_t offset=0,size=0; Handle sampler,textureView;
};
struct BindGroupDescriptor {
    Handle layout; size_t entryCount; const BindGroupEntry* entries;
};
}
using WGPUBindGroupDescriptor=wgpu::BindGroupDescriptor;
struct Device {
    int textures=0,bindings=0;
    Handle CreateTexture(const wgpu::TextureDescriptor*) { return Handle(++textures); }
    Handle CreateBindGroup(const wgpu::BindGroupDescriptor*) { return Handle(++bindings); }
} g_device;
struct Viewport { float x=0,y=0,width=0,height=0,minDepth=0,maxDepth=1; };
struct ClipRect { int32_t x=0,y=0,width=0,height=0; };
enum class CommandType { SetViewport,SetScissor };
struct Command {
    struct Data { Viewport setViewport; ClipRect setScissor; } data;
    std::shared_ptr<int> ownershipProbe;
};
using CommandList=std::vector<Command>;
struct RenderPass {
    std::string label;
    Handle colorView,depthStencilView,copySourceTexture,copySourceView,copySourceDepthView;
    wgpu::Extent3D targetSize;
    uint32_t msaaSamples=1;
    std::array<float,4> clearColorValue{};
    float clearDepthValue=1;
    bool clearColor=false,clearDepth=false,hasDepth=false,hasStencil=false;
    bool depthReadOnly=false;
    bool colorSnapshotTarget=false;
    Handle resolveTarget,resolveView;
    wgpu::StoreOp stencilStoreOp=wgpu::StoreOp::Store,colorStoreOp=wgpu::StoreOp::Store;
    CommandList commands;
};
struct TextureUpload { int serial=0; };
struct TextureCopy { int serial=0; };
struct EncoderTask { int serial=0; };
struct StagingHighWater {
    uint32_t verts=0,uniforms=0,indices=0,storage=0,textureUpload=0;
    size_t textureUploadCount=0;
};
enum class FrameOpType { RenderPass,TextureCopy,EncoderTask };
struct FrameOp {
    FrameOpType type; uint32_t index;
    RenderPass* renderPass=nullptr; TextureCopy* textureCopy=nullptr; EncoderTask* encoderTask=nullptr;
    StagingHighWater highWater;
    std::vector<const TextureUpload*> textureUploads;
};
struct FramePacket {
    std::deque<RenderPass> renderPasses;
    std::deque<TextureCopy> textureCopies;
    std::deque<EncoderTask> encoderTasks;
    uint32_t nextOpIndex=0;
    size_t capturedTextureUploads=0;
    std::vector<CommandList> recycledCommands;
    std::deque<TextureUpload> textureUploads;
    std::vector<uint8_t> verts,uniforms,indices,storage,textureUpload;
    int encoder=0;
    uint64_t frameId=0;
};
std::array<FramePacket,3> g_framePackets;
size_t g_recordingFrameSlot=0;
auto& current_frame_packet() { return g_framePackets[g_recordingFrameSlot]; }
auto& current_render_passes() { return current_frame_packet().renderPasses; }
namespace render_worker {
std::vector<std::function<void()>> work;
std::vector<uint32_t> indices;
template<class F> void enqueue_encode_pass(uint64_t, uint32_t index,F&& fn) {
    indices.push_back(index); work.emplace_back(std::forward<F>(fn));
}
}
std::vector<int> uploaded;
void encode_op(int&,FramePacket&,const FrameOp& op) {
    for(auto* upload:op.textureUploads) uploaded.push_back(upload->serial);
}
#include "frame_ops.inc"

bool g_inOffscreen=false;
uint32_t g_currentRenderPass=0;
std::optional<RenderPass> g_suspendedEfbPass;
Viewport g_cachedViewport,g_suspendedEfbViewport;
ClipRect g_cachedScissor,g_suspendedEfbScissor;
namespace gx { constexpr bool UseReversedZ=false; }
namespace webgpu {
struct TextureWithSampler { Handle texture,view; wgpu::Extent3D size; int format=0; };
struct { int depthFormat=1; } g_graphicsConfig;
}
webgpu::TextureWithSampler g_offscreenColor,g_offscreenDepth,g_snapshotPassDepth;
struct Targets { webgpu::TextureWithSampler color,depth; } snapshot,legacy;
int legacyAllocations=0,snapshotAllocations=0,enqueued=0;
Targets& acquire_pass_snapshot(uint32_t w,uint32_t h,bool color,bool depth) {
    assert(color && !depth);
    if(!snapshot.color.texture || snapshot.color.size.width!=w || snapshot.color.size.height!=h) {
        snapshot.color={Handle(++snapshotAllocations),Handle(100),{w,h,1},1};
    }
    return snapshot;
}
Targets get_offscreen_textures(uint32_t w,uint32_t h) {
    if(!legacy.color.texture || legacy.color.size.width!=w || legacy.color.size.height!=h) {
        ++legacyAllocations;
        legacy.color={Handle(101),Handle(102),{w,h,1},1};
        legacy.depth={Handle(103),Handle(104),{w,h,1},1};
    }
    return legacy;
}
std::string pass_label(const char* text) { return text; }
void enqueue_pass(FramePacket&,size_t,uint32_t) { ++enqueued; }
void push_command(CommandType,const Command::Data&) {}
#include "offscreen_target.inc"

// Device/resource creation is mocked; production cache and expiry functions run unchanged.
// Hash keys include every semantic field consumed by these call sites.
uint64_t xxh3_hash(const WGPUBindGroupDescriptor& desc) {
    uint64_t key=desc.layout.id();
    auto mix=[&](uint64_t x){ key=(key^x)*1099511628211ull; };
    mix(desc.entryCount);
    for(size_t i=0;i<desc.entryCount;++i) {
        const auto& e=desc.entries[i];
        mix(e.binding);mix(e.buffer.id());mix(e.offset);mix(e.size);mix(e.sampler.id());mix(e.textureView.id());
    }
    return key;
}
std::atomic_uint32_t g_frameIndex{0};
std::mutex g_bindGroupCacheMutex;
struct CachedBindGroup { Handle bindGroup; uint32_t lastUsedFrame; };
std::unordered_map<uint64_t,CachedBindGroup> g_cachedBindGroups;
constexpr uint32_t BindGroupCacheSweepPeriod=16,BindGroupCacheRetainFrames=32;
#include "bindings.inc"
namespace gfx {
using ::cached_bind_group;
namespace render_worker { bool is_worker_thread() { return true; } }
}
struct ResampleUniformBlock { uint32_t samplerMode=0;float frameWidth=0,frameHeight=0;uint32_t postAaMode=0; };
ResampleUniformBlock g_lastResampleUniform;
bool g_hasResampleUniform=false;
int g_Resampler=0;
std::atomic_int g_PostAa{0};
Handle g_ResampleUniformBuffer(71),g_ResampleBindGroupLayout(72);
struct { Handle sampler=Handle(73),view=Handle(74); } presentSource;
auto& present_source() { return presentSource; }
uint32_t viewport_extent(float x) { return uint32_t(x); }
uint32_t sampler_mode(int x) { return x; }
uint32_t post_aa_mode(int x) { return x; }
struct Queue {
    int writes=0;
    void WriteBuffer(Handle,int,const void*,size_t) { ++writes; }
} g_queue;
#include "present_bindings.inc"

void frame_tests() {
    auto& frame=g_framePackets[0];
    frame.frameId=42;
    for(int i=0;i<1000;++i) {
        frame.renderPasses.emplace_back();
        frame.textureUploads.push_back({i});
        frame.verts.push_back(uint8_t(i));
        auto op=capture_frame_op(frame,FrameOpType::RenderPass,i);
        assert(op.textureUploads.size()==1 && op.highWater.textureUploadCount==size_t(i+1));
        assert(op.highWater.verts==uint32_t(i+1));
        enqueue_op(frame,0,std::move(op));
    }
    frame.textureCopies.emplace_back();
    frame.encoderTasks.emplace_back();
    auto copy=capture_frame_op(frame,FrameOpType::TextureCopy,0);
    auto task=capture_frame_op(frame,FrameOpType::EncoderTask,0);
    assert(copy.textureCopy==&frame.textureCopies.front() && copy.textureUploads.empty());
    assert(task.encoderTask==&frame.encoderTasks.front() && task.textureUploads.empty());
    for(auto& fn:render_worker::work) fn();
    render_worker::work.clear();
    assert(uploaded.size()==1000);
    for(int i=0;i<1000;++i) assert(uploaded[i]==i && render_worker::indices[i]==uint32_t(i));
    // Vectors may retain capacity, never command ownership/content from the old frame.
    auto probe=std::make_shared<int>(11);
    std::weak_ptr<int> weak=probe;
    for(auto& pass:frame.renderPasses) { pass.commands.reserve(64); pass.commands.push_back({{},probe}); }
    frame.renderPasses.front().commands.reserve(4*1024*1024/sizeof(Command));
    probe.reset();
    recycle_frame_packet(frame);
    assert(weak.expired() && frame.renderPasses.empty() && frame.textureUploads.empty());
    assert(frame.nextOpIndex==0 && frame.capturedTextureUploads==0);
    assert(frame.recycledCommands.size()<=32 && !frame.recycledCommands.empty());
    size_t retained=0;
    for(auto& list:frame.recycledCommands) { assert(list.empty()); retained+=list.capacity()*sizeof(Command); }
    assert(retained<=2*1024*1024);
    for(int i=0;i<1000;++i) {
        frame.renderPasses.emplace_back();
        auto& commands=frame.renderPasses.back().commands;
        commands=std::move(frame.recycledCommands.back());frame.recycledCommands.pop_back();
        const auto capacity=commands.capacity();
        commands.push_back({});
        assert(commands.capacity()==capacity);
        recycle_frame_packet(frame);
    }
}
void target_tests() {
    auto prepare=[] {
        current_render_passes().clear();current_render_passes().emplace_back();
        g_currentRenderPass=0;g_inOffscreen=false;g_suspendedEfbPass.reset();
    };
    for(int i=0;i<1000;++i) {
        prepare();begin_offscreen_target(2048,2048,false);
        assert(!current_render_passes().back().colorSnapshotTarget);
        prepare();begin_offscreen_target(2340,1080,true);
        assert(current_render_passes().back().colorSnapshotTarget);
        assert(current_render_passes().back().copySourceView==snapshot.color.view);
    }
    assert(legacyAllocations==1 && snapshotAllocations==1 && g_device.textures==1);
    prepare();begin_offscreen_target(1920,1080,true);
    assert(g_device.textures==2 && snapshotAllocations==2 && legacyAllocations==1);
    ++webgpu::g_graphicsConfig.depthFormat;
    prepare();begin_offscreen_target(1920,1080,true);assert(g_device.textures==3);
    // Borrowed coverage does not allocate or clear private depth, even after
    // resize/format changes. Each queued pass retains its own attachment view.
    for(int i=0;i<50;++i) {
        prepare();
        Handle depth(700+i);
        begin_offscreen_target(511+i,257,true,depth);
        const auto& pass=current_render_passes().back();
        assert(pass.depthReadOnly && !pass.clearDepth && pass.clearColor);
        assert(pass.depthStencilView==depth && pass.copySourceDepthView==depth);
        assert(pass.msaaSamples==1 && pass.hasDepth && pass.colorSnapshotTarget);
        depth={};
        assert(pass.depthStencilView.id()==700+i && g_device.textures==3);
        assert(!g_offscreenDepth.view);
    }
    prepare();begin_offscreen_target(511,257,true);
    assert(!current_render_passes().back().depthReadOnly && current_render_passes().back().clearDepth);
    assert(g_device.textures==4);
    for(int finalUse=0;finalUse<2;++finalUse) for(int resolve=0;resolve<2;++resolve) {
        prepare();auto& pass=current_render_passes().back();pass.hasStencil=true;
        if(resolve)pass.resolveView=Handle(7);
        end_color_pass(finalUse);
        assert(pass.stencilStoreOp==(finalUse?wgpu::StoreOp::Discard:wgpu::StoreOp::Store));
        assert(pass.colorStoreOp==((finalUse&&resolve)?wgpu::StoreOp::Discard:wgpu::StoreOp::Store));
        assert(g_currentRenderPass==UINT32_MAX);
    }
    const int count=enqueued;end_color_pass(true);assert(enqueued==count);
}
void binding_tests() {
    wgpu::BindGroupEntry entry{.binding=0,.buffer=Handle(8),.size=16};
    wgpu::BindGroupDescriptor desc{Handle(5),1,&entry};
    auto first=cached_bind_group(desc);
    for(int i=0;i<1000;++i) assert(cached_bind_group(desc)==first);
    assert(g_device.bindings==1);
    entry.offset=256;assert(!(cached_bind_group(desc)==first));
    entry.size=32;cached_bind_group(desc);
    entry.textureView=Handle(10);cached_bind_group(desc);
    assert(g_device.bindings==4);
    g_frameIndex=32;expire_cached_bind_groups();assert(g_cachedBindGroups.size()==4);
    g_frameIndex=48;expire_cached_bind_groups();assert(g_cachedBindGroups.empty());
    assert(first.id()==1); // callers retain ownership after expiry/reset
    g_frameIndex=UINT32_MAX-15;cached_bind_group(desc);
    g_frameIndex=16;expire_cached_bind_groups();assert(!g_cachedBindGroups.empty());
    g_frameIndex=32;expire_cached_bind_groups();assert(g_cachedBindGroups.empty());
    Viewport viewport{0,0,2340,1080,0,1};
    const auto initial=resample_present_bind_group(viewport);
    for(int i=0;i<1000;++i) assert(resample_present_bind_group(viewport)==initial);
    assert(g_queue.writes==1);
    ++g_Resampler;resample_present_bind_group(viewport);
    ++g_PostAa;resample_present_bind_group(viewport);
    viewport.width=1920;resample_present_bind_group(viewport);
    viewport.height=720;resample_present_bind_group(viewport);
    assert(g_queue.writes==5);
    presentSource.view=Handle(81);
    assert(!(resample_present_bind_group(viewport)==initial) && g_queue.writes==5);
    g_hasResampleUniform=false;g_ResampleUniformBuffer=Handle(82);
    resample_present_bind_group(viewport);assert(g_queue.writes==6);
}
int main() {
    frame_tests();target_tests();binding_tests();
    puts("PASS: upload deltas/order, bounded command reuse, target separation, final stores, binding ownership/expiry, uniform reuse");
}
