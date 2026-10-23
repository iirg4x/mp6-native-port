/* Optional port-owned GTAO, using Aurora's public GPU extension API only.
 * All WebGPU work executes on Aurora's render worker. The game thread owns
 * camera values and retained-stream boundaries, never GPU command encoders. */
#include <aurora/gfx.hpp>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <aurora/aurora.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>
#include <utility>
#include "mp6_ambient_occlusion.h"
#include "mp6_display.h"
#include "mp6_enhancements.h"
#include "mp6_ao_resolution.h"
#include "mp6_ao_depth_epoch.h"
#include "ambient_occlusion_shader.hpp"
#include "ao_uniform_upload_cache.hpp"
#include "xegtao/native.hpp"
#include "xegtao/composite_shader.hpp"
#include "xegtao/half_composite_shader.hpp"
#include "mp6_host_section.h"

namespace {
using namespace aurora;
struct Params {
    float projection[4], viewport[4], scissor[4], depth[4], sizes[4];
};
static_assert(sizeof(Params) == 80);

struct Job {
    std::atomic<bool> available{true};
    uint64_t frame = UINT64_MAX; /* game-thread only */
    Params params{};
    wgpu::TextureView depth;
    wgpu::TextureView decalBefore, decalAfter, foliage;
    uint32_t decalWidth = 0, decalHeight = 0, foliageWidth = 0, foliageHeight = 0;
    wgpu::Texture cleanDepth;
    wgpu::TextureView cleanDepthView;
    uint32_t fullWidth = 0, fullHeight = 0;
    wgpu::Buffer uniform;
    mp6::ao::UniformUploadCache<Params> uniformUpload;
    // Ping-pong the two denoise passes; the raw signal is dead after blur X.
    std::array<wgpu::Texture, 2> textures;
    std::array<wgpu::TextureView, 2> ao;
    wgpu::BindGroup composite;
    uint32_t width = 0, height = 0;
    bool preparedDepth = false; /* render-worker only; private AO input */
    bool upstreamSignal = false; /* render-worker and composite callback */
    bool canonicalDepth = false; /* Full XeGTAO experiment; never legacy/Half */
    // A/B measurement variants, fixed when the job is queued.
    bool skipPasses = false, skipComposite = false;
    int camera = -1; /* game thread at queue time; read by the worker */
    int ordinal = -1; /* n-th AO point of its frame; keys the retained composite */
    mp6::xegtao::Resources upstream;
};
// Bounded by the renderer's two in-flight frames and Hu3D's 16 cameras.
// Slots are reused only after the render worker has consumed their payload,
// and NEVER within the same frame. Aurora streams passes before end_frame:
// CPU callback completion does not mean the frame's command buffer was
// submitted. A second WriteBuffer in that frame would replace uniforms used
// by an earlier camera in the same command buffer.
// GPU commands retain the referenced bind groups/textures after encoding.
std::array<Job, 64> g_jobs;
struct DecalSnapshot {
    wgpu::TextureView before, after;
    uint32_t width = 0, height = 0;
};
std::array<DecalSnapshot, 16> g_decals; /* game thread; reset on every admitted frame */
std::array<Mp6AoDepthEpoch, 16> g_depthEpochs;
struct FoliageSnapshot { wgpu::TextureView view; uint32_t width=0,height=0; };
std::array<FoliageSnapshot,16> g_foliage;
struct FoliageSeedJob {
    std::atomic<bool> available{true};
    uint64_t frame=UINT64_MAX;
    wgpu::TextureView depth;
    uint32_t sourceSamples=0; /* zero: R32 snapshot; otherwise borrowed attachment */
};
std::array<FoliageSeedJob,64> g_foliageSeeds;
int g_foliageCamera=-1;
uint32_t g_foliageWidth=0,g_foliageHeight=0;
bool g_foliageMsaaSeed=false;
uint64_t g_frame = 0;
// Web GPU memory budget (index.html: live GPU bytes + wasm heap against the
// iPhone's kill line). 0 normal; 1 tight: idle slots and converted textures
// are given back after a few frames instead of seconds; 2 critical: AO is
// skipped and every idle slot's targets are freed, so scenes lose their soft
// shadows instead of iOS killing the page. Native builds stay at 0.
int g_budgetLevel = 0;
// Mobile AO size governor (game thread writes, render worker reads): each
// step lowers the AO size cap while frames miss 60 FPS with AO drawing.
constexpr unsigned kGovernorLimits[] = {0, 960, 720, 540};
std::atomic<unsigned> g_governorStep{0};
std::atomic<unsigned> g_governorFps{0}, g_governorTarget{0}; /* telemetry */
gfx::DrawTypeId g_drawType = gfx::InvalidDrawType;
gfx::EncoderTaskId g_taskType = gfx::InvalidEncoderTask;
gfx::DrawTypeId g_foliageSeedType = gfx::InvalidDrawType;
wgpu::BindGroupLayout g_aoLayout, g_compositeLayout, g_depthLayout, g_maskLayout,
                       g_upstreamCompositeLayout;
wgpu::Texture g_emptyFoliageTexture;
wgpu::TextureView g_emptyFoliage;
wgpu::Buffer g_seedUniform;
wgpu::ShaderModule g_shader;
wgpu::ShaderModule g_preparedShader;
std::array<wgpu::ShaderModule, 2> g_upstreamCompositeShaders;
wgpu::RenderPipeline g_aoPipeline, g_blurXPipeline, g_blurYPipeline, g_depthPipeline;
std::array<wgpu::RenderPipeline, 3> g_preparedPipelines;
struct AttachmentDepthPipeline { wgpu::BindGroupLayout layout; wgpu::RenderPipeline pipeline; };
std::array<std::array<std::array<AttachmentDepthPipeline, 2>, 2>, 2> g_attachmentDepthPipelines;
struct CompositePipeline {
    wgpu::TextureFormat color, depth;
    uint32_t samples;
    wgpu::RenderPipeline pipeline;
    bool preparedDepth = false;
    bool upstreamSignal = false;
    bool canonicalDepth = false;
};
std::vector<CompositePipeline> g_compositePipelines;
struct SeedPipeline {
    wgpu::TextureFormat color, depth;
    uint32_t samples, sourceSamples;
    wgpu::RenderPipeline pipeline;
};
std::vector<SeedPipeline> g_seedPipelines;
std::array<wgpu::BindGroupLayout,2> g_seedDepthLayouts;
std::array<wgpu::ShaderModule,2> g_seedDepthShaders;
bool g_warned = false;
// Diagnostic opt-in until native quality and physical Android acceptance.
// Read once on worker initialization, never while encoding individual frames.
bool g_xegtaoRequested = false;
bool g_canonicalPreparationRequested = false;
bool g_canonicalPreparationLogged = false;
mp6::xegtao::Policy g_xegtaoPolicy;
mp6::xegtao::Pipeline g_xegtao;
uint64_t g_encoded = 0, g_composited = 0;
// Render worker only: the most recent encoded AO of each AO point (the n-th
// apply of a frame), re-composited at the same point of interpolated frames
// (mp6_ao_replay_reuses_tick). Keyed by order, not camera: one camera can
// shade several depth segments in a frame (a clear between them), and a
// per-camera entry then painted the last segment's AO, a ghost of another
// view, over the earlier ones. The bind group keeps its textures alive.
struct RetainedComposite {
    wgpu::BindGroup bindings;
    bool preparedDepth = false, upstreamSignal = false, canonicalDepth = false;
};
std::array<RetainedComposite, 32> g_retained;
int g_applyOrdinal = 0, g_retainedOrdinal = 0; /* game thread, reset per frame */
gfx::DrawTypeId g_retainedDrawType = gfx::InvalidDrawType;
uint64_t g_retainedComposited = 0;
uint32_t g_lastSamples = 0, g_lastTarget[2] = {}, g_lastAo[2] = {}; /* A/B diagnostics */
uint64_t g_upstreamEncoded = 0;
uint64_t g_foliageSeeded=0;

void warn(const char *reason) {
    if (!g_warned) std::fprintf(stderr, "[MP6-AO] unavailable: %s\n", reason);
    g_warned = true;
}

wgpu::BindGroupLayout make_layout(const wgpu::Device &device, int kind,
                                bool depthAttachment = false, bool multisampled = false,
                                bool waterDepthMask = false, bool canonicalAdapter = false) {
    const uint32_t baseCount = kind == 5 ? 5u : (kind >= 2 ? 4u : (kind ? 3u : 2u));
    const uint32_t textureCount = baseCount + (waterDepthMask ? 1u : 0u);
    const uint32_t count = textureCount + (canonicalAdapter ? 1u : 0u);
    std::array<wgpu::BindGroupLayoutEntry, 7> entries{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[0].buffer.minBindingSize = sizeof(Params);
    for (uint32_t i = 1; i < textureCount; ++i) {
        entries[i].binding = kind == 2 && i >= 2 ? i + 1 : i;
        if ((kind == 3 || kind == 4) && i == 3) entries[i].binding = 5;
        if (kind == 5 && i == 4) entries[i].binding = 5;
        // Only ordered depth preparation adds this input. AO, seed and both
        // full/half-resolution composite layouts retain their old contracts.
        if (waterDepthMask && i == baseCount) entries[i].binding = 5;
        entries[i].visibility = wgpu::ShaderStage::Fragment;
        entries[i].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
        entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        if ((kind == 4 || kind == 5) && i == 2) entries[i].texture.sampleType = wgpu::TextureSampleType::Uint;
        if (i == 1 && depthAttachment) {
            entries[i].texture.sampleType = wgpu::TextureSampleType::Depth;
            entries[i].texture.multisampled = multisampled;
        }
    }
    if (canonicalAdapter) {
        auto &entry = entries[textureCount];
        entry.binding = 7;
        entry.visibility = wgpu::ShaderStage::Fragment;
        entry.buffer.type = wgpu::BufferBindingType::Uniform;
        entry.buffer.minBindingSize = sizeof(mp6::xegtao::AdapterParams);
    }
    const wgpu::BindGroupLayoutDescriptor desc{
        .label = "MP6 GTAO bindings",
        .entryCount = count, .entries = entries.data(),
    };
    return device.CreateBindGroupLayout(&desc);
}

// AO's pipelines live in Aurora's unified pipeline cache (docs/SHADER_CACHE.md):
// a scene's AO pipelines are recorded with it and built on the pipeline
// compile threads while it loads, instead of on its first AO frame. The create
// function is pure -- its own shader modules and layouts, group-equivalent to
// the ones AO's bind groups use -- so it can run on any thread. XeGTAO's
// validated composite keeps its own path below.
enum AoCachedEntry : uint32_t { AoEntryAo, AoEntryBlurX, AoEntryBlurY, AoEntryComposite, AoEntryFoliageSeed,
                                AoEntryDecalDepth, AoEntryCount };
constexpr std::array<const char *, AoEntryCount> kAoCachedEntries{
    "fs_ao", "fs_blur_x", "fs_blur_y", "fs_composite", "fs_foliage_seed", "fs_decal_depth"};
enum : uint32_t { AoKeyComposite = 1, AoKeyPreparedDepth = 2, AoKeyCompatibility = 4, AoKeyReducedDepth = 8,
                  AoKeyDebugView = 16 };

// The prepared-depth shader variant must match how encode sizes the prepared
// depth: on mobile it is stored at the AO size (mp6_ao_reduced_depth), and a
// full-size shader reading it samples the wrong texels everywhere.
bool ao_debug_view() {
    const char *env = std::getenv("MP6_AO_DEBUG_VIEW");
    return env && env[0] == '1';
}
struct AoPipelineKey {
    uint32_t entry;
    uint32_t color, depth; // wgpu::TextureFormat
    uint32_t samples;
    uint32_t seedDepthSamples;
    uint32_t flags;
};
static_assert(std::has_unique_object_representations_v<AoPipelineKey>);
gfx::PipelineTypeId g_pipelineType = gfx::InvalidPipelineType;

wgpu::RenderPipeline create_cached_pipeline(const void *data, size_t size) {
    AoPipelineKey key;
    if (size != sizeof(key)) return {};
    std::memcpy(&key, data, sizeof(key));
    if (key.entry >= AoEntryCount) return {};
    const auto device = gfx::device();
    const char *entry = kAoCachedEntries[key.entry];
    const bool composite = key.flags & AoKeyComposite, preparedDepth = key.flags & AoKeyPreparedDepth;
    const bool compatibility = key.flags & AoKeyCompatibility;
    const bool seed = key.entry == AoEntryFoliageSeed;
    const auto module = [&](const std::string &code, const char *label) {
        const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = code.c_str()}};
        const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = label};
        return device.CreateShaderModule(&desc);
    };
    const auto base = module(ambient_occlusion_shader(compatibility), "MP6 ambient occlusion");
    const auto fragmentModule =
        key.seedDepthSamples ? module(ambient_occlusion_shader(compatibility, false, true, key.seedDepthSamples > 1,
                                                               false), "MP6 borrowed foliage depth") :
        preparedDepth ? module(ambient_occlusion_shader(compatibility, true, false, false, true, false,
                                                        key.flags & AoKeyReducedDepth, key.flags & AoKeyDebugView),
                               "MP6 GTAO prepared R32 depth") : base;
    const auto bindLayout = key.seedDepthSamples ? make_layout(device, 0, true, key.seedDepthSamples > 1) :
                            key.entry == AoEntryDecalDepth ? make_layout(device, 2) :
                            composite ? make_layout(device, 3) :
                            make_layout(device, seed || key.entry == AoEntryAo ? 0 : 1);
    const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &bindLayout};
    const auto layout = device.CreatePipelineLayout(&layoutDesc);
    // Same blend/target/depth state as make_pipeline's direct path below.
    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;
    blend.alpha.srcFactor = wgpu::BlendFactor::Zero;
    blend.alpha.dstFactor = wgpu::BlendFactor::One;
    const wgpu::ColorTargetState target{
        .format = static_cast<wgpu::TextureFormat>(key.color), .blend = composite ? &blend : nullptr,
        .writeMask = composite ? wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue
                              : wgpu::ColorWriteMask::All,
    };
    const wgpu::FragmentState fragment{.module = fragmentModule, .entryPoint = entry, .targetCount = 1,
                                       .targets = &target};
    wgpu::DepthStencilState depthState{};
    depthState.format = static_cast<wgpu::TextureFormat>(key.depth);
    depthState.depthWriteEnabled = seed;
    depthState.depthCompare = wgpu::CompareFunction::Always;
    depthState.stencilReadMask = 0;
    depthState.stencilWriteMask = 0;
    const wgpu::RenderPipelineDescriptor desc{
        .label = entry,
        .layout = layout,
        .vertex = {.module = base, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = composite || seed ? &depthState : nullptr,
        .multisample = {.count = key.samples},
        .fragment = &fragment,
    };
    return device.CreateRenderPipeline(&desc);
}

wgpu::RenderPipeline make_pipeline(const wgpu::Device &device, const char *entry, bool composite,
                                  wgpu::TextureFormat color, wgpu::TextureFormat depth, uint32_t samples,
                                  bool preparedDepth = false, uint32_t seedDepthSamples = 0,
                                  bool upstreamComposite = false, bool canonicalDepth = false) {
    const bool seed=!std::strcmp(entry,"fs_foliage_seed");
    const size_t seedVariant = seedDepthSamples > 1 ? 1 : 0;
    if (seedDepthSamples && !g_seedDepthShaders[seedVariant]) {
        const auto code = ambient_occlusion_shader(gfx::ao_compatibility_mode(), false, true,
                                                   seedDepthSamples > 1, false);
        const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code=code.c_str()}};
        const wgpu::ShaderModuleDescriptor desc{.nextInChain=&source,.label="MP6 borrowed foliage depth"};
        g_seedDepthShaders[seedVariant] = device.CreateShaderModule(&desc);
        g_seedDepthLayouts[seedVariant] = make_layout(device,0,true,seedDepthSamples>1);
    }
    if (!upstreamComposite && g_pipelineType != gfx::InvalidPipelineType) {
        const auto named = std::find_if(kAoCachedEntries.begin(), kAoCachedEntries.end(),
                                        [&](const char *name) { return !std::strcmp(name, entry); });
        if (named != kAoCachedEntries.end()) {
            const AoPipelineKey key{
                .entry = static_cast<uint32_t>(named - kAoCachedEntries.begin()),
                .color = static_cast<uint32_t>(color), .depth = static_cast<uint32_t>(depth),
                .samples = samples, .seedDepthSamples = seedDepthSamples,
                .flags = (composite ? AoKeyComposite : 0u) | (preparedDepth ? AoKeyPreparedDepth : 0u) |
                         (gfx::ao_compatibility_mode() ? AoKeyCompatibility : 0u) |
                         (preparedDepth && mp6_ao_reduced_depth() ? AoKeyReducedDepth : 0u) |
                         (preparedDepth && ao_debug_view() ? AoKeyDebugView : 0u),
            };
            if (auto pipeline = gfx::custom_pipeline(g_pipelineType, &key, sizeof(key))) return pipeline;
        }
    }
    const auto &bindLayout = seedDepthSamples ? g_seedDepthLayouts[seedVariant] :
                            !std::strcmp(entry, "fs_decal_depth") ? g_depthLayout :
                            (composite ? (upstreamComposite ? g_upstreamCompositeLayout : g_maskLayout) :
                             (seed || !std::strcmp(entry,"fs_ao") ? g_aoLayout : g_compositeLayout));
    const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &bindLayout};
    auto layout = device.CreatePipelineLayout(&layoutDesc);
    // Shade the background but restore the unshaded premultiplied foliage.
    // RGB = scene * visibility + foreground * (1-visibility).
    // Preserve scene alpha, depth and stencil.
    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;
    blend.alpha.srcFactor = wgpu::BlendFactor::Zero;
    blend.alpha.dstFactor = wgpu::BlendFactor::One;
    const wgpu::ColorTargetState target{
        .format = color, .blend = composite ? &blend : nullptr,
        .writeMask = composite ? wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue
                              : wgpu::ColorWriteMask::All,
    };
    const wgpu::FragmentState fragment{
        .module = upstreamComposite ? g_upstreamCompositeShaders[canonicalDepth] :
                  seedDepthSamples ? g_seedDepthShaders[seedVariant] :
                  preparedDepth ? g_preparedShader : g_shader, .entryPoint = entry,
        .targetCount = 1, .targets = &target,
    };
    wgpu::DepthStencilState depthState{};
    depthState.format = depth;
    depthState.depthWriteEnabled = seed;
    depthState.depthCompare = wgpu::CompareFunction::Always;
    depthState.stencilReadMask = 0;
    depthState.stencilWriteMask = 0;
    const wgpu::RenderPipelineDescriptor desc{
        .label = entry,
        .layout = layout,
        .vertex = {.module = upstreamComposite ? g_upstreamCompositeShaders[canonicalDepth] : g_shader, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = composite || seed ? &depthState : nullptr,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    return device.CreateRenderPipeline(&desc);
}

bool init_upstream_composite(const wgpu::Device &device, bool canonicalDepth = false) {
    // The policy is immutable until shutdown. Keep the full reference shader
    // and its four-entry layout separate from experimental reconstruction.
    const bool half = g_xegtaoPolicy.resolution == mp6::xegtao::Resolution::Half;
    assert(!canonicalDepth || !half);
    auto &shader = g_upstreamCompositeShaders[canonicalDepth];
    if (shader) return true;
    const bool newLayout = !g_upstreamCompositeLayout;
    std::optional<mp6::xegtao::InitializationGuard> validation;
    if (canonicalDepth) {
        validation.emplace(device);
        if (!validation->ready()) return false;
    }
    if (!g_upstreamCompositeLayout) g_upstreamCompositeLayout = make_layout(device, half ? 5 : 4);
    {
        auto code = half ? std::string(mp6::xegtao::HalfCompositeShader) :
                           mp6::xegtao::composite_shader(canonicalDepth);
        // MP6_AO_DEBUG_VIEW=1: output the AO term alone, as the legacy path does.
        if (const char *debugView = std::getenv("MP6_AO_DEBUG_VIEW"); debugView && debugView[0] == '1') {
            for (const auto &[from, to] : {std::pair<std::string, std::string>{"return vec4f(0,0,0,1);", "return vec4f(1,1,1,0);"},
                                           {"return foliage_visibility(pixel.xy, visibility);", "return vec4f(vec3f(visibility),0.0);"}})
                for (size_t at = code.find(from); at != std::string::npos; at = code.find(from, at + to.size()))
                    code.replace(at, from.size(), to);
        }
        const wgpu::ShaderSourceWGSL upstreamCompositeSource{
            wgpu::ShaderSourceWGSL::Init{.code = code.c_str()}};
        const wgpu::ShaderModuleDescriptor upstreamCompositeDesc{
            .nextInChain = &upstreamCompositeSource,
            .label = half ? "MP6 XeGTAO bilateral composite" : "MP6 XeGTAO direct composite"};
        shader = device.CreateShaderModule(&upstreamCompositeDesc);
    }
    if (validation && !validation->finish()) {
        shader = nullptr;
        if (newLayout) g_upstreamCompositeLayout = nullptr;
        return false;
    }
    return shader && g_upstreamCompositeLayout;
}

void init_worker(const wgpu::Device &device) {
    if (g_shader) return;
    const char *upstream = std::getenv("MP6_AO_XEGTAO");
    g_xegtaoRequested = mp6::xegtao::requested(upstream);
    const char *canonical = std::getenv("MP6_AO_XEGTAO_PREPARED_CANONICAL");
    g_canonicalPreparationRequested = canonical && std::strcmp(canonical, "1") == 0;
    const bool explicitSelection = upstream &&
        (std::strcmp(upstream, "0") == 0 || std::strcmp(upstream, "1") == 0);
    if (upstream && !explicitSelection)
        std::fprintf(stderr, "[MP6-AO] invalid MP6_AO_XEGTAO (expected 0|1); using platform default\n");
    std::fprintf(stderr, "[MP6-AO] requested implementation: %s (%s)\n",
                 g_xegtaoRequested ? "XeGTAO" : "legacy",
                 explicitSelection ? "environment override" : "platform default");
    if (g_xegtaoRequested) {
        const auto parsed = mp6::xegtao::parse_policy(std::getenv("MP6_AO_XEGTAO_QUALITY"),
                                                    std::getenv("MP6_AO_XEGTAO_DENOISE"),
                                                    std::getenv("MP6_AO_XEGTAO_RESOLUTION"),
                                                    std::getenv("MP6_AO_XEGTAO_DEPTH_STORAGE"));
        g_xegtaoPolicy = parsed.policy;
        if (parsed.invalidQuality)
            std::fprintf(stderr, "[MP6-AO] invalid MP6_AO_XEGTAO_QUALITY (expected low|medium|high|ultra); using High\n");
        if (parsed.invalidDenoise)
            std::fprintf(stderr, "[MP6-AO] invalid MP6_AO_XEGTAO_DENOISE (expected 1|2|3); using 2\n");
        if (parsed.invalidResolution)
            std::fprintf(stderr, "[MP6-AO] invalid MP6_AO_XEGTAO_RESOLUTION (expected full|half); using full\n");
        if (parsed.invalidDepthStorage)
            std::fprintf(stderr, "[MP6-AO] invalid MP6_AO_XEGTAO_DEPTH_STORAGE (expected r32float|r16float); using r32float\n");
    }
    const bool compatibility = gfx::ao_compatibility_mode();
    const auto shaderCode = ambient_occlusion_shader(compatibility);
    std::fprintf(stderr, "[MP6-AO] shader/scheduling path: %s\n", compatibility ? "compatibility" : "optimized");
    const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = shaderCode.c_str()}};
    const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = "MP6 ambient occlusion"};
    g_shader = device.CreateShaderModule(&desc);
    g_aoLayout = make_layout(device, false);
    g_compositeLayout = make_layout(device, true);
    g_depthLayout = make_layout(device, 2);
    g_maskLayout = make_layout(device, 3);
    const wgpu::TextureDescriptor emptyDesc{
        .label="MP6 AO no foliage", .usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst,
        .size={1,1,1}, .format=wgpu::TextureFormat::RGBA8Unorm,
    };
    g_emptyFoliageTexture=device.CreateTexture(&emptyDesc);
    g_emptyFoliage=g_emptyFoliageTexture.CreateView();
    const uint32_t emptyPixel=0;
    const wgpu::TexelCopyTextureInfo destination{.texture=g_emptyFoliageTexture};
    const wgpu::TexelCopyBufferLayout emptyLayout{.bytesPerRow=4,.rowsPerImage=1};
    const wgpu::Extent3D emptySize{1,1,1};
    gfx::queue().WriteTexture(&destination,&emptyPixel,4,&emptyLayout,&emptySize);
    const wgpu::BufferDescriptor seedUniformDesc{.usage=wgpu::BufferUsage::Uniform,.size=sizeof(Params)};
    g_seedUniform=device.CreateBuffer(&seedUniformDesc);
}

void init_depth_path(const wgpu::Device &device, bool preparedDepth, bool legacySignal = true) {
    if (preparedDepth) {
        if (!g_preparedShader) {
            const char *debugView = std::getenv("MP6_AO_DEBUG_VIEW");
            const auto shaderCode = ambient_occlusion_shader(gfx::ao_compatibility_mode(), true, false, false, true,
                                                             false, mp6_ao_reduced_depth(),
                                                             debugView && debugView[0] == '1');
            const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = shaderCode.c_str()}};
            const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = "MP6 GTAO prepared R32 depth"};
            g_preparedShader = device.CreateShaderModule(&desc);
        }
        if (!legacySignal || g_preparedPipelines[0]) return;
        const std::array entries{"fs_ao", "fs_blur_x", "fs_blur_y"};
        for (size_t i=0; i<entries.size(); ++i)
            g_preparedPipelines[i] = make_pipeline(device, entries[i], false, wgpu::TextureFormat::RGBA16Float, wgpu::TextureFormat::Undefined, 1, true);
    } else if (!g_aoPipeline) {
        g_aoPipeline = make_pipeline(device, "fs_ao", false, wgpu::TextureFormat::RGBA16Float, wgpu::TextureFormat::Undefined, 1);
        g_blurXPipeline = make_pipeline(device, "fs_blur_x", false, wgpu::TextureFormat::RGBA16Float, wgpu::TextureFormat::Undefined, 1);
        g_blurYPipeline = make_pipeline(device, "fs_blur_y", false, wgpu::TextureFormat::RGBA16Float, wgpu::TextureFormat::Undefined, 1);
    }
}

wgpu::BindGroup make_bind_group(const wgpu::Device &device, Job &job, wgpu::TextureView input = nullptr, bool composite=false) {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = job.uniform, .size = sizeof(Params)},
        wgpu::BindGroupEntry{.binding = 1, .textureView = job.depth},
        wgpu::BindGroupEntry{.binding = 2, .textureView = input},
        wgpu::BindGroupEntry{.binding = 5, .textureView = job.foliage ? job.foliage : g_emptyFoliage},
    };
    const wgpu::BindGroupDescriptor desc{
        .label = "MP6 AO frame bindings", .layout = composite ? g_maskLayout : (input ? g_compositeLayout : g_aoLayout),
        .entryCount = composite ? 4u : (input ? 3u : 2u), .entries = entries.data(),
    };
    return gfx::cached_bind_group(desc);
}

wgpu::BindGroup make_upstream_composite_bind_group(const wgpu::Device &device, Job &job) {
    init_upstream_composite(device, job.canonicalDepth);
    const bool half = g_xegtaoPolicy.resolution == mp6::xegtao::Resolution::Half;
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = job.uniform, .size = sizeof(Params)},
        wgpu::BindGroupEntry{.binding = 1, .textureView = job.cleanDepthView},
        wgpu::BindGroupEntry{.binding = 2, .textureView = job.upstream.scratch->aoViews[job.upstream.finalAoIndex]},
        wgpu::BindGroupEntry{.binding = 5, .textureView = job.foliage ? job.foliage : g_emptyFoliage},
        wgpu::BindGroupEntry{.binding = 3, .textureView = job.upstream.scratch->mipViews[0]},
    };
    const wgpu::BindGroupDescriptor desc{
        .label = half ? "MP6 XeGTAO bilateral composite" : "MP6 XeGTAO direct composite", .layout = g_upstreamCompositeLayout,
        .entryCount = half ? 5u : 4u, .entries = entries.data(),
    };
    return gfx::cached_bind_group(desc);
}

Job &payload_job(const void *payload) {
    Job *job;
    std::memcpy(&job, payload, sizeof(job));
    return *job;
}

const AttachmentDepthPipeline &attachment_depth_pipeline(const wgpu::Device &device, bool msaa, bool decals,
                                                        bool canonicalDepth = false) {
    auto &cached = g_attachmentDepthPipelines[msaa][decals][canonicalDepth];
    if (cached.pipeline) return cached;
    std::optional<mp6::xegtao::InitializationGuard> validation;
    if (canonicalDepth) {
        validation.emplace(device);
        if (!validation->ready()) return cached;
    }
    const auto code = ambient_occlusion_shader(gfx::ao_compatibility_mode(), true, true, msaa, decals, canonicalDepth,
                                               !canonicalDepth && mp6_ao_reduced_depth());
    const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = code.c_str()}};
    const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = "MP6 ordered depth preparation"};
    const auto shader = device.CreateShaderModule(&desc);
    cached.layout = make_layout(device, decals ? 2 : 0, true, msaa, true, canonicalDepth);
    const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &cached.layout};
    const auto layout = device.CreatePipelineLayout(&layoutDesc);
    const wgpu::ColorTargetState target{.format = wgpu::TextureFormat::R32Float};
    const wgpu::FragmentState fragment{.module = shader, .entryPoint = canonicalDepth ? "fs_canonical_depth" : "fs_decal_depth",
                                       .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor pipelineDesc{
        .label = "MP6 ordered depth preparation", .layout = layout,
        .vertex = {.module = shader, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList}, .fragment = &fragment,
    };
    cached.pipeline = device.CreateRenderPipeline(&pipelineDesc);
    if (validation && !validation->finish()) cached = {};
    return cached;
}

void encode(const gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &encoder,
            const void *payload, size_t, void *) {
    auto &job = payload_job(payload);
    init_worker(ctx.device);
    // The attachment is borrowed ONLY for this ordered task. Produce the same
    // R32 input before the EFB continuation can write it; no retained alias.
    unsigned aoWidth, aoHeight;
    if (mp6_enh_ao_ab_variant() == MP6_AO_AB_CAP960)
    {
        // MP6_AO_AB_CAP_PX overrides the measured cap (default 960).
        const char *cap = std::getenv("MP6_AO_AB_CAP_PX");
        const unsigned long requested = cap ? std::strtoul(cap, nullptr, 10) : 0;
        const unsigned limit = requested >= 64 && requested <= 16384 ? unsigned(requested) : 960u;
        mp6_ao_working_size_with_limit(ctx.targetWidth, ctx.targetHeight, limit, &aoWidth, &aoHeight);
    }
    else
        mp6_ao_working_size(ctx.targetWidth, ctx.targetHeight, &aoWidth, &aoHeight);
    if (const unsigned step = g_governorStep.load(std::memory_order_relaxed))
        if (std::max(aoWidth, aoHeight) > kGovernorLimits[step])
            mp6_ao_working_size_with_limit(ctx.targetWidth, ctx.targetHeight, kGovernorLimits[step], &aoWidth, &aoHeight);
    // AO_UPSTREAM_PREPARATION_BEGIN
    bool upstream = false;
    uint32_t upstreamWidth = 0, upstreamHeight = 0;
    std::optional<XeGTAO::GTAOConstants> upstreamConstants;
    if (g_xegtaoRequested && !g_xegtao.unavailable()) {
        const auto upstreamSize = mp6::xegtao::working_size(ctx.targetWidth, ctx.targetHeight, g_xegtaoPolicy.resolution);
        upstreamWidth = upstreamSize.width;
        upstreamHeight = upstreamSize.height;
        // Five upstream depth MIPs require a working longest side of at least 16.
        // Tiny half-resolution views fall back to the existing legacy path rather
        // than silently changing the requested resolution or allocating invalid MIPs.
        if (std::max(upstreamSize.width, upstreamSize.height) >= 16) {
            upstreamConstants.emplace();
            if (mp6::xegtao::fill_parameters(upstreamSize.width, upstreamSize.height,
                    job.params.projection[0], job.params.projection[1],
                    job.params.projection[2], job.params.projection[3],
                    job.params.viewport, *upstreamConstants, 64.0f, g_xegtaoPolicy))
                upstream = g_xegtao.initialize(ctx.device, g_xegtaoPolicy);
        }
    }
    // AO_UPSTREAM_PREPARATION_END
    const bool previousUpstream = job.upstreamSignal;
    job.upstreamSignal = upstream;
    job.canonicalDepth = upstream && g_canonicalPreparationRequested &&
                         g_xegtaoPolicy.resolution == mp6::xegtao::Resolution::Full;
    // Release idle upstream scratch on fallback. Outstanding worker leases and
    // encoded GPU commands retain their references; no texture is destroyed.
    // Per-job uniforms are still retired if this slot changes implementation.
    if (!upstream && previousUpstream) job.upstream = {};
    if (!upstream) g_xegtao.release_idle_scratch();
    // Full resolution remains the reference/default. Half resolution is an
    // explicit experimental quality/performance gate; preset quality does not
    // change scale. The shipping legacy resolution policy is intact.
    if (upstream) {
        aoWidth = upstreamWidth;
        aoHeight = upstreamHeight;
    }
    job.params.sizes[0] = float(ctx.targetWidth);
    job.params.sizes[1] = float(ctx.targetHeight);
    job.params.sizes[2] = float(aoWidth);
    job.params.sizes[3] = float(aoHeight);
    if (job.foliageWidth != ctx.targetWidth || job.foliageHeight != ctx.targetHeight) job.foliage = nullptr;
    const auto width = static_cast<uint32_t>(job.params.sizes[2]);
    const auto height = static_cast<uint32_t>(job.params.sizes[3]);
    if (!job.uniform) {
        const wgpu::BufferDescriptor desc{
            .label = "MP6 AO frame parameters", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = sizeof(Params),
        };
        job.uniformUpload.reset();
        job.uniform = ctx.device.CreateBuffer(&desc);
    }
    if (upstream) {
        // The native signal is already a full-resolution R32_UINT texture.
        // Drop any legacy RGBA16F views/textures when a reused slot switches
        // modes; retaining them would make the slot appear allocated and
        // obscure later fallback-size changes.
        if (!previousUpstream || job.ao[0] || job.ao[1] || job.textures[0] || job.textures[1]) {
            for (auto &view : job.ao) view = nullptr;
            for (auto &texture : job.textures) gfx::retire_texture(std::move(texture));
        }
        job.width = width;
        job.height = height;
    }
    if (!upstream && (!job.ao[0] || !job.ao[1] || job.width != width || job.height != height)) {
        const wgpu::TextureDescriptor desc{
            .label = "MP6 AO visibility and normal",
            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
            .size = {width, height, 1}, .format = wgpu::TextureFormat::RGBA16Float,
        };
        for (size_t i = 0; i < job.ao.size(); ++i) {
            gfx::retire_texture(std::move(job.textures[i]));
            job.textures[i] = ctx.device.CreateTexture(&desc);
            job.ao[i] = job.textures[i].CreateView();
        }
        job.width = width;
        job.height = height;
    }
    job.uniformUpload.upload_if_changed(job.params, [&](const void *bytes, size_t size) {
        ctx.queue.WriteBuffer(job.uniform, 0, bytes, size);
    });
    mp6::xegtao::AdapterParams adapter{};
    if (upstream) {
        adapter.depth[0] = upstreamConstants->DepthUnpackConsts.x;
        adapter.depth[1] = upstreamConstants->DepthUnpackConsts.y;
        adapter.depth[2] = job.params.depth[3];
        std::copy_n(job.params.viewport, 4, adapter.viewport);
        std::copy_n(job.params.scissor, 4, adapter.scissor);
        // The preparation pass references this job-private buffer before
        // Pipeline::encode. Never borrow a global uniform from another camera.
        if (job.canonicalDepth)
            g_xegtao.prepare_adapter(ctx.device, ctx.queue, job.upstream, adapter);
    }
    job.preparedDepth = true;
    init_depth_path(ctx.device, job.preparedDepth, !upstream);
    if (job.preparedDepth) {
        const auto fullWidth = uint32_t(job.params.sizes[0]);
        const auto fullHeight = uint32_t(job.params.sizes[1]);
        // Mobile stores the prepared depth at the shading size (see
        // mp6_ao_reduced_depth); XeGTAO always consumes full resolution.
        const bool reduced = !upstream && mp6_ao_reduced_depth();
        const auto depthWidth = reduced ? width : fullWidth;
        const auto depthHeight = reduced ? height : fullHeight;
        if (!job.cleanDepth || job.fullWidth != depthWidth || job.fullHeight != depthHeight) {
            const wgpu::TextureDescriptor desc{
                .label = "MP6 GTAO decal-free depth",
                .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
                .size = {depthWidth, depthHeight, 1}, .format = wgpu::TextureFormat::R32Float,
            };
            gfx::retire_texture(std::move(job.cleanDepth));
            job.cleanDepth = ctx.device.CreateTexture(&desc);
            job.cleanDepthView = job.cleanDepth.CreateView();
            job.fullWidth = depthWidth;
            job.fullHeight = depthHeight;
        }
        job.depth = job.cleanDepthView;
        const bool decals = job.decalBefore && job.decalAfter &&
                            job.decalWidth == fullWidth && job.decalHeight == fullHeight;
        if (job.canonicalDepth &&
            (!init_upstream_composite(ctx.device, true) ||
             !attachment_depth_pipeline(ctx.device, ctx.sampleCount > 1, decals, true).pipeline)) {
            // Cache rejection until shutdown; the current and later jobs use
            // the original view-Z producer AND consumers, not a mixed ABI.
            g_canonicalPreparationRequested = false;
            job.canonicalDepth = false;
            std::fprintf(stderr, "[MP6-AO] prepared canonical unavailable; using view-Z\n");
        }
        const auto &pipeline = attachment_depth_pipeline(ctx.device, ctx.sampleCount > 1, decals, job.canonicalDepth);
        std::array<wgpu::BindGroupEntry, 6> entries{};
        entries[0] = {.binding = 0, .buffer = job.uniform, .size = sizeof(Params)};
        entries[1] = {.binding = 1, .textureView = ctx.depth};
        entries[2] = {.binding = 5, .textureView = job.foliage ? job.foliage : g_emptyFoliage};
        uint32_t entryCount = 3;
        if (decals) {
            entries[entryCount++] = {.binding = 3, .textureView = job.decalBefore};
            entries[entryCount++] = {.binding = 4, .textureView = job.decalAfter};
        }
        if (job.canonicalDepth)
            entries[entryCount++] = {.binding = 7, .buffer = job.upstream.adapter, .size = sizeof(adapter)};
        const wgpu::BindGroupDescriptor bindingsDesc{
            .label = "MP6 GTAO exact decal coverage", .layout = pipeline.layout,
            .entryCount = entryCount, .entries = entries.data(),
        };
        auto bindings = gfx::cached_bind_group(bindingsDesc);
        const wgpu::RenderPassColorAttachment color{
            .view = job.depth, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
            .clearValue = {job.canonicalDepth ? 1.0 : 0.0, 0, 0, 0},
        };
        const wgpu::RenderPassDescriptor desc{
            .label = "MP6 GTAO decal depth", .colorAttachmentCount = 1, .colorAttachments = &color,
#ifdef AURORA_GPU_STAT_ROWS
            .timestampWrites = gfx::gpu_pass_timestamps("AO depth preparation"),
#endif
        };
        if (!job.skipPasses) {
            auto pass = encoder.BeginRenderPass(&desc);
            pass.SetPipeline(pipeline.pipeline);
            pass.SetBindGroup(0, bindings);
            pass.Draw(3);
            pass.End();
        }
    }
    if (upstream) {
        g_xegtao.encode(ctx.device, ctx.queue, encoder, job.upstream,
            job.depth, *upstreamConstants, adapter, job.canonicalDepth ?
                mp6::xegtao::DepthInput::CanonicalNdc : mp6::xegtao::DepthInput::ViewZ);
        if (job.canonicalDepth && !g_canonicalPreparationLogged) {
            std::fprintf(stderr, "[MP6-AO] XeGTAO preparation: canonical\n");
            g_canonicalPreparationLogged = true;
        }
        ++g_upstreamEncoded;
    } else {
        const auto pipelines = job.preparedDepth ? g_preparedPipelines :
                               std::array{g_aoPipeline, g_blurXPipeline, g_blurYPipeline};
        // The no-blur measurement variant keeps only the main pass (its
        // output is ao[0], which the composite reads either way).
        const size_t passCount = job.skipPasses ? 0 :
                                 mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_BLUR ? 1 : pipelines.size();
        for (size_t i = 0; i < passCount; ++i) {
            auto bindings = make_bind_group(ctx.device, job, i ? job.ao[(i - 1) % 2] : nullptr);
            const wgpu::RenderPassColorAttachment color{
                .view = job.ao[i % 2], .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
                .clearValue = {1, 0, 0, 0},
            };
            const wgpu::RenderPassDescriptor passDesc{
                .label = "MP6 ambient occlusion / denoise", .colorAttachmentCount = 1, .colorAttachments = &color,
#ifdef AURORA_GPU_STAT_ROWS
                .timestampWrites = gfx::gpu_pass_timestamps(i == 0 ? "GTAO" : (i == 1 ? "AO blur X" : "AO blur Y")),
#endif
            };
            auto pass = encoder.BeginRenderPass(&passDesc);
            pass.SetPipeline(pipelines[i]);
            pass.SetBindGroup(0, bindings);
            pass.Draw(3);
            pass.End();
        }
    }
    job.composite = upstream ? make_upstream_composite_bind_group(ctx.device, job) :
                    make_bind_group(ctx.device, job, job.ao[0], true);
    if (job.ordinal >= 0 && job.ordinal < int(g_retained.size()))
        g_retained[job.ordinal] = {job.composite, job.preparedDepth, job.upstreamSignal, job.canonicalDepth};
    g_lastSamples = ctx.sampleCount;
    g_lastTarget[0] = ctx.targetWidth; g_lastTarget[1] = ctx.targetHeight;
    g_lastAo[0] = width; g_lastAo[1] = height;
    if (job.skipComposite) {
        // No composite draw was queued: release the slot here instead.
        job.composite = nullptr;
        job.upstream.scratch.reset();
        job.available.store(true, std::memory_order_release);
    }
    ++g_encoded;
}

void draw_composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
                    const wgpu::BindGroup &bindings, bool preparedDepth, bool upstreamSignal,
                    bool canonicalDepth) {
    if (upstreamSignal) init_upstream_composite(ctx.device, canonicalDepth);
    auto found = std::find_if(g_compositePipelines.begin(), g_compositePipelines.end(), [&](const auto &p) {
        return p.color == ctx.colorFormat && p.depth == ctx.depthFormat && p.samples == ctx.sampleCount &&
               p.preparedDepth == preparedDepth && p.upstreamSignal == upstreamSignal &&
               p.canonicalDepth == canonicalDepth;
    });
    if (found == g_compositePipelines.end()) {
        g_compositePipelines.push_back({ctx.colorFormat, ctx.depthFormat, ctx.sampleCount,
            make_pipeline(ctx.device, "fs_composite", true, ctx.colorFormat, ctx.depthFormat, ctx.sampleCount,
                          preparedDepth, 0, upstreamSignal, canonicalDepth),
            preparedDepth, upstreamSignal, canonicalDepth});
        found = g_compositePipelines.end() - 1;
    }
    pass.SetViewport(0, 0, float(ctx.targetWidth), float(ctx.targetHeight), 0, 1);
    pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);
    pass.SetPipeline(found->pipeline);
    pass.SetBindGroup(0, bindings);
    pass.Draw(3);
}

void retained_composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
                        const void *payload, size_t, void *) {
    int ordinal;
    std::memcpy(&ordinal, payload, sizeof(ordinal));
    if (ordinal < 0 || ordinal >= int(g_retained.size())) return;
    const auto &retained = g_retained[ordinal];
    if (!retained.bindings) return;
    draw_composite(ctx, pass, retained.bindings, retained.preparedDepth, retained.upstreamSignal,
                   retained.canonicalDepth);
    ++g_retainedComposited;
}

void composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
               const void *payload, size_t, void *) {
    auto &job = payload_job(payload);
    draw_composite(ctx, pass, job.composite, job.preparedDepth, job.upstreamSignal, job.canonicalDepth);
    // Aurora restores the GX viewport, scissor and pipeline state afterwards.
    ++g_composited;
    // The next encoder task seals/enqueues this EFB pass before it can acquire
    // scratch. Same-queue command order, not available/GPU completion, makes
    // its texture writes follow this final read. A rejected composite never
    // releases its lease, so a later encode allocates a separate bundle.
    job.composite = nullptr;
    job.upstream.scratch.reset();
    job.available.store(true, std::memory_order_release);
}

void foliage_seed(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
                  const void *payload, size_t, void *) {
    FoliageSeedJob *job;
    std::memcpy(&job,payload,sizeof(job));
    init_worker(ctx.device);
    auto found=std::find_if(g_seedPipelines.begin(),g_seedPipelines.end(),[&](const auto &p) {
        return p.color==ctx.colorFormat && p.depth==ctx.depthFormat && p.samples==ctx.sampleCount &&
               p.sourceSamples==job->sourceSamples;
    });
    if (found==g_seedPipelines.end()) {
        g_seedPipelines.push_back({ctx.colorFormat,ctx.depthFormat,ctx.sampleCount,job->sourceSamples,
            make_pipeline(ctx.device,"fs_foliage_seed",false,ctx.colorFormat,ctx.depthFormat,
                          ctx.sampleCount,false,job->sourceSamples)});
        found=g_seedPipelines.end()-1;
    }
    // The seed entry only reads binding 1, but the explicit AO layout also
    // declares an unused uniform. Supply a small immutable zero buffer.
    const std::array entries{
        wgpu::BindGroupEntry{.binding=0,.buffer=g_seedUniform,.size=sizeof(Params)},
        wgpu::BindGroupEntry{.binding=1,.textureView=job->depth},
    };
    const auto &layout = job->sourceSamples ? g_seedDepthLayouts[job->sourceSamples>1 ? 1 : 0] : g_aoLayout;
    const wgpu::BindGroupDescriptor desc{.layout=layout,.entryCount=entries.size(),.entries=entries.data()};
    auto bindings=gfx::cached_bind_group(desc);
    pass.SetViewport(0,0,float(ctx.targetWidth),float(ctx.targetHeight),0,1);
    pass.SetScissorRect(0,0,ctx.targetWidth,ctx.targetHeight);
    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);
    ++g_foliageSeeded;
    job->available.store(true,std::memory_order_release);
}
} // namespace

/* A recorded AO key (the bundled seed is recorded on a PC) asks for this
 * device's variant: its target formats and sample count for the passes that
 * draw into the scene, and the platform's shader variants (WebGPU
 * compatibility mode, the phones' reduced-resolution depth), which the PC
 * recording never uses. */
static bool replay_cached_pipeline(void *data, size_t size) {
    if (size != sizeof(AoPipelineKey)) return false;
    AoPipelineKey key;
    std::memcpy(&key, data, sizeof(key));
    if (key.entry >= AoEntryCount) return false;
    if (key.entry == AoEntryComposite || key.entry == AoEntryFoliageSeed) {
        key.color = static_cast<uint32_t>(gfx::color_format());
        key.depth = static_cast<uint32_t>(gfx::depth_format());
        key.samples = gfx::sample_count();
        if (key.seedDepthSamples) key.seedDepthSamples = gfx::sample_count();
    }
    const bool preparedDepth = key.flags & AoKeyPreparedDepth;
    key.flags = (key.flags & (AoKeyComposite | AoKeyPreparedDepth)) |
                (gfx::ao_compatibility_mode() ? AoKeyCompatibility : 0u) |
                (preparedDepth && mp6_ao_reduced_depth() ? AoKeyReducedDepth : 0u) |
                (preparedDepth && ao_debug_view() ? AoKeyDebugView : 0u);
    std::memcpy(data, &key, sizeof(key));
    return true;
}

/* AO's pipelines go through the unified pipeline cache as their own type.
 * Registered before the first-launch shader build (main_native.c), so the
 * recorded AO pipelines are part of it, and again here for safety. */
extern "C" void mp6_ao_register_pipeline_type(void) {
    if (g_pipelineType == gfx::InvalidPipelineType)
        g_pipelineType = gfx::register_pipeline_type({.name = "mp6.ambient_occlusion", .version = 1,
                                                       .create = create_cached_pipeline,
                                                       .replay = replay_cached_pipeline});
}

extern "C" void mp6_ao_begin_frame(void) {
    ++g_frame;
#ifdef __EMSCRIPTEN__
    {
        const int level = EM_ASM_INT({ return (Module.mp6GpuLevel | 0); });
        if (level != g_budgetLevel) {
            g_budgetLevel = level;
            gfx::set_texture_idle_seconds(level >= 1 ? 1 : 0);
        }
    }
#endif
    // A slot was released only by its composite draw, so every composite that
    // never ran (a dropped or skipped frame, a draw skipped while its shader
    // compiles, a hidden frame) leaked the slot with its full-size AO pair and
    // depth copy. In any scene the leaks add up until all 64 slots are held:
    // ~620 MB of GPU memory on an iPhone, where iOS killed the page. A slot
    // whose frame is long gone (far past the renderer's frames in flight; its
    // commands, if any, were submitted and keep their textures alive) is
    // reclaimed, and a slot idle for 2 s gives its textures back.
    for (auto &job : g_jobs) {
        if (job.frame == UINT64_MAX) continue;
        if (!job.available.load(std::memory_order_acquire)) {
            if (g_frame - job.frame < 16) continue;
            job.composite = nullptr;
            job.upstream.scratch.reset();
            job.available.store(true, std::memory_order_release);
        }
        if (g_frame - job.frame < (g_budgetLevel >= 2 ? 2u : g_budgetLevel >= 1 ? 10u : 120u)) continue;
        if (!job.textures[0] && !job.textures[1] && !job.cleanDepth) continue;
        for (auto &view : job.ao) view = nullptr;
        for (auto &texture : job.textures) gfx::retire_texture(std::move(texture));
        job.cleanDepthView = nullptr;
        gfx::retire_texture(std::move(job.cleanDepth));
        job.composite = nullptr;
        job.depth = nullptr;
        job.width = job.height = 0;
        job.fullWidth = job.fullHeight = 0;
    }
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    {
        // Frame-rate feedback stands in for GPU timers, which Safari cannot
        // read safely. The target is the display's refresh (120 on the S22+,
        // 60 on the iPhone). Below 92% of it with AO in most frames, AO steps
        // down once per second; a step that does not raise the frame rate by
        // 3% is undone and step-downs pause for 30 s (the frame is limited by
        // something else). Five seconds at 97%+ steps back up. Windows under
        // half the target (loading, backgrounding) are ignored.
        using Clock = std::chrono::steady_clock;
        static Clock::time_point start = Clock::now();
        static unsigned frames = 0, aoFrames = 0, good = 0, hold = 0;
        static double target = 0, beforeStep = 0;
        ++frames;
        if (g_applyOrdinal > 0) ++aoFrames;
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        if (seconds >= 1.0) {
            Mp6DisplayInfo display{};
            if (!target || !(frames & 7)) {
                const int hz = mp6_display_info_get(&display) && display.refreshHz > 0 ? display.refreshHz : 60;
                target = hz < 30 ? 30 : hz > 240 ? 240 : hz;
            }
            const double fps = frames / seconds;
            g_governorFps.store(unsigned(fps + 0.5), std::memory_order_relaxed);
            g_governorTarget.store(unsigned(target), std::memory_order_relaxed);
            const unsigned step = g_governorStep.load(std::memory_order_relaxed);
            if (hold) --hold;
            if (fps < target * 0.5) {
                good = 0;
            } else if (beforeStep > 0) {
                if (fps < beforeStep * 1.03) {
                    g_governorStep.store(step - 1, std::memory_order_relaxed);
                    hold = 30;
                }
                beforeStep = 0;
            } else if (!hold && aoFrames * 2 >= frames && fps < target * 0.92 &&
                       step + 1 < std::size(kGovernorLimits)) {
                g_governorStep.store(step + 1, std::memory_order_relaxed);
                beforeStep = fps;
                good = 0;
            } else if (fps >= target * 0.97 && step > 0) {
                if (++good >= 5) { g_governorStep.store(step - 1, std::memory_order_relaxed); good = 0; }
            } else {
                good = 0;
            }
            start = Clock::now();
            frames = aoFrames = 0;
        }
    }
#endif
    g_applyOrdinal = g_retainedOrdinal = 0;
    for (auto &epoch:g_depthEpochs) epoch={};
    for (auto &decal : g_decals) decal = {};
    for (auto &foliage:g_foliage) foliage={};
    g_foliageCamera=-1;

    // Registered before the first scene loads, so recorded AO pipelines are
    // prewarmed with their scene (create_cached_pipeline).
    mp6_ao_register_pipeline_type();
}

extern "C" void mp6_ao_camera_begin(int camera) {
    if (camera<0 || camera>=int(g_depthEpochs.size())) return;
    // The game may mutate Hu3DCamera after this boundary.  The HSF adapter
    // records the immutable values immediately after each Hu3DCameraSet;
    // invalidate any previous scope before the new camera starts.
    mp6_ao_camera_snapshot_reset(camera);
    auto &epoch=g_depthEpochs[camera];
    epoch={};
    if (mp6_enh_ambient_occlusion())
        mp6_ao_depth_epoch_reset(&epoch,gfx::efb_depth_write_serial());
}

extern "C" void mp6_ao_camera_consume(int camera) {
    if (camera<0 || camera>=int(g_depthEpochs.size())) return;
    mp6_ao_depth_epoch_consume(&g_depthEpochs[camera]);
}

extern "C" void mp6_ao_camera_exclude(int camera) {
    if (camera<0 || camera>=int(g_depthEpochs.size())) return;
    if (!mp6_enh_ambient_occlusion()) return;
    // This is a semantic boundary supplied by selector code: subsequent
    // commands are orthographic/UI/copy work and must never shade the 3D
    // receiver.  It does not drain GX or touch unrelated cameras.
    mp6_ao_depth_epoch_exclude(&g_depthEpochs[camera]);
    mp6_ao_camera_snapshot_reset(camera);
    g_decals[camera]={};
    g_foliage[camera]={};
}

extern "C" void mp6_ao_depth_cleared(int camera) {
    if (camera<0 || camera>=int(g_depthEpochs.size())) return;
    if (!mp6_enh_ambient_occlusion()) return;
    // The clear quad itself writes depth, but leaves no scene geometry for AO.
    mp6_ao_camera_begin(camera);
    // A camera can render a texture, clear, then draw an unrelated scene with
    // a different projection in the same tick (the opening book does this).
    // The old foreground list survived that boundary and replayed its cutouts
    // over the finished book at the new FOV, producing displaced silhouettes.
    // Retire all receiver-dependent data with its depth, not just the epoch.
    mp6_ao_foliage_render(camera,0);
    g_decals[camera]={};
    g_foliage[camera]={};
}

extern "C" int mp6_ao_camera_has_depth(int camera) {
    if (!mp6_enh_ambient_occlusion() || camera<0 || camera>=int(g_depthEpochs.size())) return 0;
    const auto &epoch=g_depthEpochs[camera];
    // Consumed, excluded, and not-yet-admitted scopes cannot use the serial;
    // avoid touching the bridge/FIFO for duplicate ends and UI boundaries.
    if (!epoch.valid || epoch.consumed || epoch.excluded) return 0;
    return mp6_ao_depth_epoch_has_writes(&epoch,gfx::efb_depth_write_serial());
}

extern "C" int mp6_ao_foliage_begin(int camera) {
    if (camera<0 || camera>=int(g_foliage.size()) || g_foliageCamera!=-1) return 0;
    if (mp6_enh_ao_ab_variant()==MP6_AO_AB_NO_FOLIAGE) return 0;
    g_foliageMsaaSeed=false;
    // Coverage draws only test depth. Read the original single-sample EFB
    // attachment directly instead of copying it into a private depth target.
    // MSAA retains its explicit sample-zero seed; Mali retains its tested path.
    unsigned width=0, height=0;
    if (!gfx::ao_compatibility_mode() && gfx::create_pass_with_readonly_efb_depth(width,height)) {
        mp6_fi_note_ao_foliage(camera,0);
        g_foliageCamera=camera;
        g_foliageWidth=width; g_foliageHeight=height;
        return 1;
    }
    FoliageSeedJob *job=nullptr;
    for (auto &candidate:g_foliageSeeds)
        if (candidate.frame!=g_frame && candidate.available.load(std::memory_order_acquire)) { job=&candidate; break; }
    if (!job) return 0;
    gfx::ResolvedTargets targets;
    gfx::BorrowedPassDepth source;
    // The seed executes before the offscreen bracket ends, hence before any
    // resumed EFB writes. Sample zero is identical to the old R32 snapshot.
    // Keep the verified Mali schedule until it can be tested on that device.
    if (!gfx::ao_compatibility_mode() && gfx::create_pass_from_efb_depth(source)) {
        targets.depth=std::move(source.view);
        targets.width=source.width; targets.height=source.height;
    } else {
        if (!gfx::resolve_pass({.color=false,.depth=true},targets) || !targets.depth || !targets.width || !targets.height) return 0;
        if (!gfx::create_pass(targets.width,targets.height)) return 0;
    }
    if (g_foliageSeedType==gfx::InvalidDrawType)
        g_foliageSeedType=gfx::register_draw_type({.label="MP6 foliage receiver coverage",.draw=foliage_seed});
    job->frame=g_frame; job->depth=targets.depth;
    job->sourceSamples=source.sampleCount;
    job->available.store(false,std::memory_order_relaxed);
    if (!gfx::push_custom_draw(g_foliageSeedType,&job,sizeof(job))) {
        gfx::ResolvedTargets discarded; gfx::resolve_pass({.color=false,.depth=false},discarded);
        job->available.store(true,std::memory_order_release); return 0;
    }
    mp6_fi_note_ao_foliage(camera,0);
    g_foliageCamera=camera;
    g_foliageWidth=targets.width; g_foliageHeight=targets.height;
    g_foliageMsaaSeed=source.sampleCount>1;
    return 1;
}

extern "C" int mp6_ao_foliage_uses_msaa_seed(void) {
    return g_foliageCamera>=0 && g_foliageMsaaSeed;
}

extern "C" int mp6_ao_foliage_target_size(unsigned *width, unsigned *height) {
    if (g_foliageCamera<0 || !width || !height) return 0;
    *width=g_foliageWidth; *height=g_foliageHeight;
    return 1;
}

extern "C" void mp6_ao_foliage_end(int camera) {
    if (camera!=g_foliageCamera) return;
    gfx::ResolvedTargets targets;
    if (gfx::resolve_pass({.color=true,.depth=false},targets) && targets.color)
        g_foliage[camera]={targets.color,targets.width,targets.height};
    mp6_fi_note_ao_foliage(camera,1);
    g_foliageCamera=-1;
}

extern "C" void mp6_ao_capture_decals(int camera, int after) {
    if (!mp6_enh_ambient_occlusion() || camera < 0 || camera >= int(g_decals.size())) return;
    if (mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_DECALS) return;
    auto &decal = g_decals[camera];
    if (!after) decal = {};
    if (after && !decal.before) return;
    gfx::ResolvedTargets targets;
    if (!gfx::resolve_pass({.color = false, .depth = true}, targets) || !targets.depth) {
        decal = {};
        return;
    }
    // Record only after resolve_pass drains the GX FIFO. The exact bracket,
    // not a stale game's depth snapshot, is repeated on interpolated frames.
    mp6_fi_note_ao_decals(camera, after);
    if (!after) {
        decal.before = targets.depth;
        decal.width = targets.width;
        decal.height = targets.height;
    } else if (decal.width == targets.width && decal.height == targets.height) {
        decal.after = targets.depth;
    } else decal = {};
}

extern "C" void mp6_ao_apply(const Mp6AoView *view) {
    DecalSnapshot decal;
    if (view && view->camera >= 0 && view->camera < int(g_decals.size())) {
        decal = std::move(g_decals[view->camera]);
        g_decals[view->camera] = {};
    }
    const int level = mp6_enh_ambient_occlusion();
    if (!level || !view) return;
    if (g_budgetLevel >= 2) return; /* GPU memory critical: no AO this frame */
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    // Multi-camera screens (the bus, minigame previews, results) ran a full-
    // size AO pass per camera: 3-6 per frame dropped the iPhone to ~37 FPS.
    // Small windows get none, and at most two cameras per frame get AO.
    if (view->viewport[2] * view->viewport[3] < 0.2f || g_applyOrdinal >= 2) return;
#endif
    if (mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_APPLY) return;
    for (float value : view->projection) if (!std::isfinite(value) || value <= 0) return;
    if (view->projection[3] <= view->projection[2] ||
        view->viewport[2] <= 0 || view->viewport[3] <= 0 ||
        view->depthRange[1] <= view->depthRange[0]) return;
    if (g_drawType == gfx::InvalidDrawType) {
        g_drawType = gfx::register_draw_type({.label = "MP6 ambient occlusion", .draw = composite});
        g_taskType = gfx::register_encoder_task_type({.label = "MP6 ambient occlusion", .callback = encode, .readsDepth = true});
    }
    Job *job = nullptr;
    for (auto &candidate : g_jobs) {
        if (candidate.frame != g_frame && candidate.available.load(std::memory_order_acquire)) {
            job = &candidate;
            break;
        }
    }
    if (!job) { warn("GPU frame slot capacity exceeded"); return; }
    job->available.store(false, std::memory_order_relaxed);
    job->frame = g_frame;
    job->camera = view->camera;
    job->skipPasses = mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_PASSES;
    job->skipComposite = mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_COMPOSITE;
    job->foliage=nullptr;
    if (view->camera>=0 && view->camera<int(g_foliage.size())) {
        auto &mask=g_foliage[view->camera];
        job->foliage=std::move(mask.view);
        job->foliageWidth=mask.width; job->foliageHeight=mask.height;
        mask={};
    }
    job->decalBefore = job->decalAfter = nullptr;
    job->decalWidth=decal.width; job->decalHeight=decal.height;
    if (decal.before && decal.after) {
        job->decalBefore = std::move(decal.before);
        job->decalAfter = std::move(decal.after);
    }
    auto &p = job->params;
    std::copy_n(view->projection, 4, p.projection);
    std::copy_n(view->viewport, 4, p.viewport);
    std::copy_n(view->scissor, 4, p.scissor);
    p.depth[0] = view->depthRange[0];
    p.depth[1] = view->depthRange[1];
    p.depth[2] = gfx::uses_reversed_z() ? 1.0f : 0.0f;
    // Maximum darkening for Strong/Subtle. MP6's open boards produce little
    // obscurance outside contacts; 0.35/0.20 was hard to see, 0.8/0.5 and
    // 0.55/0.35 too dark on the iPhone.
    p.depth[3] = level == 2 ? 0.45f : 0.25f;
    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {
        job->available.store(true, std::memory_order_release);
        warn("scene encoder task rejected");
        return;
    }
    // push_encoder_task drains GX before sealing; record that exact boundary.
    mp6_fi_note_ao(view);
    // The replay's n-th AO marker is this apply: same order, same view.
    job->ordinal = g_applyOrdinal++;
    // If a draw is rejected, retain the slot until shutdown: its encoder task
    // may still be pending. Never hand a pending task another frame's values.
    if (job->skipComposite) return; /* A/B variant: encode releases the slot */
    if (!gfx::push_custom_draw(g_drawType, &job, sizeof(job))) warn("scene composite rejected");
}

/* Mobile Unlocked FPS: an interpolated frame re-composites the AO its tick
 * frame computed (same camera, at most one tick old) instead of repeating the
 * decal snapshots, foliage pass, depth preparation, GTAO and denoise. On the
 * Galaxy S22+ those cost ~7 ms of GPU per frame (knockout A/B with pass
 * timestamps, Towering Treetop). AO is low frequency; it lags moving geometry
 * by at most one 60 Hz tick, as a 60 FPS game would. MP6_AO_EVERY_FRAME=1 (or
 * the every-frame A/B variant) restores per-frame recomputation. */
extern "C" int mp6_ao_replay_reuses_tick(void) {
    static int everyFrame = -1;
    if (everyFrame < 0) {
        const char *env = std::getenv("MP6_AO_EVERY_FRAME");
        everyFrame = env && env[0] == '1';
    }
#ifdef __ANDROID__
    return mp6_ao_reduced_depth() && !everyFrame && mp6_enh_ao_ab_variant() != MP6_AO_AB_EVERY_FRAME;
#else
    /* Web: a desktop browser presents several hundred interpolated frames per
     * second, each showing the tick's AO up to a tick behind moving geometry,
     * which read as a smeared ghost silhouette (board select, PC Chrome, 413
     * FPS). Desktop GPUs recompute AO per frame cheaply, and the iPhone runs
     * at its display rate, so the web build recomputes every frame. */
    (void)everyFrame;
    return 0;
#endif
}

extern "C" void mp6_ao_apply_retained(const Mp6AoView *view) {
    // Count every marker, shaded or not, so the order matches the tick frame.
    const int ordinal = g_retainedOrdinal++;
    if (!view || !mp6_enh_ambient_occlusion() || ordinal >= int(g_retained.size()) ||
        mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_APPLY || mp6_enh_ao_ab_variant() == MP6_AO_AB_NO_COMPOSITE)
        return;
    if (g_retainedDrawType == gfx::InvalidDrawType)
        g_retainedDrawType = gfx::register_draw_type({.label = "MP6 retained AO composite", .draw = retained_composite});
    if (!gfx::push_custom_draw(g_retainedDrawType, &ordinal, sizeof(ordinal))) warn("retained composite rejected");
}

/* A/B diagnostics since the previous call: AO frames, applies queued/encoded/
 * composited, and the last encoded target (samples, framebuffer, AO size). */
extern "C" void mp6_ao_ab_stats(char *out, size_t size) {
    static uint64_t frame0 = 0, encoded0 = 0, composited0 = 0;
    static uint64_t retained0 = 0;
    extern unsigned long long mp6AoCpuDecalNs, mp6AoCpuFoliageNs, mp6AoCpuApplyNs;
    const int used = std::snprintf(out, size, "cpu_ms(decals=%.1f foliage=%.1f apply=%.1f) ",
                                   mp6AoCpuDecalNs / 1e6, mp6AoCpuFoliageNs / 1e6, mp6AoCpuApplyNs / 1e6);
    mp6AoCpuDecalNs = mp6AoCpuFoliageNs = mp6AoCpuApplyNs = 0;
    if (used > 0 && size_t(used) < size) { out += used; size -= size_t(used); }
    std::snprintf(out, size, "ao_frames=%llu encoded=%llu composited=%llu retained=%llu samples=%u target=%ux%u ao_size=%ux%u",
                  (unsigned long long)(g_frame - frame0), (unsigned long long)(g_encoded - encoded0),
                  (unsigned long long)(g_composited - composited0),
                  (unsigned long long)(g_retainedComposited - retained0), g_lastSamples,
                  g_lastTarget[0], g_lastTarget[1], g_lastAo[0], g_lastAo[1]);
    frame0 = g_frame; encoded0 = g_encoded; composited0 = g_composited; retained0 = g_retainedComposited;
}

/* Telemetry window (rides with [MP6-PIPE]): whether AO is configured and
 * actually drawing, so phone reports show AO activity without a debug build. */
extern "C" void mp6_ao_window_stats(char *out, size_t size) {
    static uint64_t encoded0 = 0, composited0 = 0, upstream0 = 0;
    std::snprintf(out, size, "setting=%d budget=%d governor=%u fps=%u/%u encoded=%llu composited=%llu xegtao=%llu ao_size=%ux%u",
                  mp6_enh_ambient_occlusion(), g_budgetLevel, g_governorStep.load(), g_governorFps.load(), g_governorTarget.load(), (unsigned long long)(g_encoded - encoded0),
                  (unsigned long long)(g_composited - composited0),
                  (unsigned long long)(g_upstreamEncoded - upstream0), g_lastAo[0], g_lastAo[1]);
    encoded0 = g_encoded; composited0 = g_composited; upstream0 = g_upstreamEncoded;
}

extern "C" void mp6_ao_shutdown(void) {
    const bool initialized = g_drawType != gfx::InvalidDrawType || g_foliageSeedType != gfx::InvalidDrawType;
    if (initialized) gfx::synchronize();
    if (std::getenv("MP6_AO_DIAG"))
        std::fprintf(stderr, "[MP6-AO] encoded=%llu composited=%llu foliage=%llu xegtao=%llu\n",
                     (unsigned long long)g_encoded, (unsigned long long)g_composited,
                     (unsigned long long)g_foliageSeeded, (unsigned long long)g_upstreamEncoded);
    if (!initialized) return;
    gfx::unregister_draw_type(g_drawType);
    gfx::unregister_encoder_task_type(g_taskType);
    gfx::unregister_draw_type(g_foliageSeedType);
    g_foliageSeedType=gfx::InvalidDrawType;
    if (g_retainedDrawType != gfx::InvalidDrawType) gfx::unregister_draw_type(g_retainedDrawType);
    g_retainedDrawType = gfx::InvalidDrawType;
    g_retained = {};
    g_drawType = gfx::InvalidDrawType;
    g_taskType = gfx::InvalidEncoderTask;
    for (auto &job : g_jobs) {
        job.composite = nullptr;
        job.depth = nullptr;
        job.decalBefore = job.decalAfter = nullptr;
        job.foliage=nullptr;
        job.cleanDepth = nullptr;
        job.cleanDepthView = nullptr;
        job.fullWidth = job.fullHeight = 0;
        for (auto &view : job.ao) view = nullptr;
        for (auto &texture : job.textures) texture = nullptr;
        job.uniform = nullptr;
        job.uniformUpload.reset();
        job.width = job.height = 0;
        job.preparedDepth = false;
        job.upstreamSignal = false;
        job.canonicalDepth = false;
        job.upstream = {};
        job.available.store(true, std::memory_order_release);
    }
    g_compositePipelines.clear();
    g_xegtao.reset();
    g_seedPipelines.clear();
    for (auto &layout:g_seedDepthLayouts) layout=nullptr;
    for (auto &shader:g_seedDepthShaders) shader=nullptr;
    for (auto &seed:g_foliageSeeds) { seed.depth=nullptr; seed.available.store(true); }
    for (auto &foliage:g_foliage) foliage={};
    g_emptyFoliage=nullptr; g_emptyFoliageTexture=nullptr;
    g_maskLayout = g_upstreamCompositeLayout = nullptr;
    g_upstreamCompositeShaders = {};
    g_xegtaoRequested = false;
    g_canonicalPreparationRequested = g_canonicalPreparationLogged = false;
    g_xegtaoPolicy = {};
    g_seedUniform=nullptr;
    g_aoPipeline = nullptr;
    g_blurXPipeline = g_blurYPipeline = nullptr;
    g_depthPipeline = nullptr;
    g_shader = nullptr;
    g_preparedShader = nullptr;
    for (auto &pipeline : g_preparedPipelines) pipeline = nullptr;
    for (auto &samples : g_attachmentDepthPipelines) for (auto &pipeline : samples) pipeline = {};
    g_aoLayout = g_compositeLayout = g_depthLayout = nullptr;
    for (auto &decal : g_decals) decal = {};
}
