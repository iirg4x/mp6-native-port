/* Optional port-owned GTAO, using Aurora's public GPU extension API only.
 * All WebGPU work executes on Aurora's render worker. The game thread owns
 * camera values and retained-stream boundaries, never GPU command encoders. */
#include <aurora/gfx.hpp>
#include <aurora/aurora.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <utility>
#include "mp6_ambient_occlusion.h"
#include "mp6_enhancements.h"
#include "mp6_ao_resolution.h"
#include "mp6_ao_depth_epoch.h"
#include "ambient_occlusion_shader.hpp"
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
    // Ping-pong the two denoise passes; the raw signal is dead after blur X.
    std::array<wgpu::Texture, 2> textures;
    std::array<wgpu::TextureView, 2> ao;
    wgpu::BindGroup composite;
    uint32_t width = 0, height = 0;
    bool preparedDepth = false; /* render-worker only; private AO input */
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
uint64_t g_frame = 0;
gfx::DrawTypeId g_drawType = gfx::InvalidDrawType;
gfx::EncoderTaskId g_taskType = gfx::InvalidEncoderTask;
gfx::DrawTypeId g_foliageSeedType = gfx::InvalidDrawType;
wgpu::BindGroupLayout g_aoLayout, g_compositeLayout, g_depthLayout, g_maskLayout;
wgpu::Texture g_emptyFoliageTexture;
wgpu::TextureView g_emptyFoliage;
wgpu::Buffer g_seedUniform;
wgpu::ShaderModule g_shader;
wgpu::ShaderModule g_preparedShader;
wgpu::RenderPipeline g_aoPipeline, g_blurXPipeline, g_blurYPipeline, g_depthPipeline;
std::array<wgpu::RenderPipeline, 3> g_preparedPipelines;
struct AttachmentDepthPipeline { wgpu::BindGroupLayout layout; wgpu::RenderPipeline pipeline; };
std::array<std::array<AttachmentDepthPipeline, 2>, 2> g_attachmentDepthPipelines;
struct CompositePipeline {
    wgpu::TextureFormat color, depth;
    uint32_t samples;
    wgpu::RenderPipeline pipeline;
    bool preparedDepth = false;
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
uint64_t g_encoded = 0, g_composited = 0;
uint64_t g_foliageSeeded=0;

void warn(const char *reason) {
    if (!g_warned) std::fprintf(stderr, "[MP6-AO] unavailable: %s\n", reason);
    g_warned = true;
}

wgpu::BindGroupLayout make_layout(const wgpu::Device &device, int kind,
                                bool depthAttachment = false, bool multisampled = false) {
    const uint32_t count = kind >= 2 ? 4u : (kind ? 3u : 2u);
    std::array<wgpu::BindGroupLayoutEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[0].buffer.minBindingSize = sizeof(Params);
    for (uint32_t i = 1; i < count; ++i) {
        entries[i].binding = kind == 2 && i >= 2 ? i + 1 : i;
        if (kind==3 && i==3) entries[i].binding=5;
        entries[i].visibility = wgpu::ShaderStage::Fragment;
        entries[i].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
        entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        if (i == 1 && depthAttachment) {
            entries[i].texture.sampleType = wgpu::TextureSampleType::Depth;
            entries[i].texture.multisampled = multisampled;
        }
    }
    const wgpu::BindGroupLayoutDescriptor desc{
        .label = "MP6 GTAO bindings",
        .entryCount = count, .entries = entries.data(),
    };
    return device.CreateBindGroupLayout(&desc);
}

wgpu::RenderPipeline make_pipeline(const wgpu::Device &device, const char *entry, bool composite,
                                  wgpu::TextureFormat color, wgpu::TextureFormat depth, uint32_t samples,
                                  bool preparedDepth = false, uint32_t seedDepthSamples = 0) {
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
    const auto &bindLayout = seedDepthSamples ? g_seedDepthLayouts[seedVariant] :
                            !std::strcmp(entry, "fs_decal_depth") ? g_depthLayout :
                            (composite ? g_maskLayout :
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
        .module = seedDepthSamples ? g_seedDepthShaders[seedVariant] :
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
        .vertex = {.module = g_shader, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = composite || seed ? &depthState : nullptr,
        .multisample = {.count = samples},
        .fragment = &fragment,
    };
    return device.CreateRenderPipeline(&desc);
}

void init_worker(const wgpu::Device &device) {
    if (g_shader) return;
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

void init_depth_path(const wgpu::Device &device, bool preparedDepth) {
    if (preparedDepth) {
        if (g_preparedShader) return;
        const auto shaderCode = ambient_occlusion_shader(gfx::ao_compatibility_mode(), true);
        const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = shaderCode.c_str()}};
        const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = "MP6 GTAO prepared R32 depth"};
        g_preparedShader = device.CreateShaderModule(&desc);
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

Job &payload_job(const void *payload) {
    Job *job;
    std::memcpy(&job, payload, sizeof(job));
    return *job;
}

const AttachmentDepthPipeline &attachment_depth_pipeline(const wgpu::Device &device, bool msaa, bool decals) {
    auto &cached = g_attachmentDepthPipelines[msaa][decals];
    if (cached.pipeline) return cached;
    const auto code = ambient_occlusion_shader(gfx::ao_compatibility_mode(), true, true, msaa, decals);
    const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = code.c_str()}};
    const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = "MP6 ordered depth preparation"};
    const auto shader = device.CreateShaderModule(&desc);
    cached.layout = make_layout(device, decals ? 2 : 0, true, msaa);
    const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &cached.layout};
    const auto layout = device.CreatePipelineLayout(&layoutDesc);
    const wgpu::ColorTargetState target{.format = wgpu::TextureFormat::R32Float};
    const wgpu::FragmentState fragment{.module = shader, .entryPoint = "fs_decal_depth",
                                       .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor pipelineDesc{
        .label = "MP6 ordered depth preparation", .layout = layout,
        .vertex = {.module = shader, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList}, .fragment = &fragment,
    };
    cached.pipeline = device.CreateRenderPipeline(&pipelineDesc);
    return cached;
}

void encode(const gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &encoder,
            const void *payload, size_t, void *) {
    auto &job = payload_job(payload);
    init_worker(ctx.device);
    // The attachment is borrowed ONLY for this ordered task. Produce the same
    // R32 input before the EFB continuation can write it; no retained alias.
    unsigned aoWidth, aoHeight;
    mp6_ao_working_size(ctx.targetWidth, ctx.targetHeight, &aoWidth, &aoHeight);
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
        job.uniform = ctx.device.CreateBuffer(&desc);
    }
    if (!job.ao[0] || job.width != width || job.height != height) {
        const wgpu::TextureDescriptor desc{
            .label = "MP6 AO visibility and normal",
            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
            .size = {width, height, 1}, .format = wgpu::TextureFormat::RGBA16Float,
        };
        for (size_t i = 0; i < job.ao.size(); ++i) {
            job.textures[i] = ctx.device.CreateTexture(&desc);
            job.ao[i] = job.textures[i].CreateView();
        }
        job.width = width;
        job.height = height;
    }
    ctx.queue.WriteBuffer(job.uniform, 0, &job.params, sizeof(job.params));
    job.preparedDepth = true;
    init_depth_path(ctx.device, job.preparedDepth);
    if (job.preparedDepth) {
        const auto fullWidth = uint32_t(job.params.sizes[0]);
        const auto fullHeight = uint32_t(job.params.sizes[1]);
        if (!job.cleanDepth || job.fullWidth != fullWidth || job.fullHeight != fullHeight) {
            const wgpu::TextureDescriptor desc{
                .label = "MP6 GTAO decal-free depth",
                .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
                .size = {fullWidth, fullHeight, 1}, .format = wgpu::TextureFormat::R32Float,
            };
            job.cleanDepth = ctx.device.CreateTexture(&desc);
            job.cleanDepthView = job.cleanDepth.CreateView();
            job.fullWidth = fullWidth;
            job.fullHeight = fullHeight;
        }
        job.depth = job.cleanDepthView;
        const bool decals = job.decalBefore && job.decalAfter &&
                            job.decalWidth == fullWidth && job.decalHeight == fullHeight;
        const auto &pipeline = attachment_depth_pipeline(ctx.device, ctx.sampleCount > 1, decals);
        const std::array entries{
            wgpu::BindGroupEntry{.binding = 0, .buffer = job.uniform, .size = sizeof(Params)},
            wgpu::BindGroupEntry{.binding = 1, .textureView = ctx.depth},
            wgpu::BindGroupEntry{.binding = 3, .textureView = job.decalBefore},
            wgpu::BindGroupEntry{.binding = 4, .textureView = job.decalAfter},
        };
        const wgpu::BindGroupDescriptor bindingsDesc{
            .label = "MP6 GTAO exact decal coverage", .layout = pipeline.layout,
            .entryCount = decals ? 4u : 2u, .entries = entries.data(),
        };
        auto bindings = gfx::cached_bind_group(bindingsDesc);
        const wgpu::RenderPassColorAttachment color{
            .view = job.depth, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
        };
        const wgpu::RenderPassDescriptor desc{
            .label = "MP6 GTAO decal depth", .colorAttachmentCount = 1, .colorAttachments = &color,
#ifdef AURORA_GPU_STAT_ROWS
            .timestampWrites = gfx::gpu_pass_timestamps("AO depth preparation"),
#endif
        };
        auto pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(pipeline.pipeline);
        pass.SetBindGroup(0, bindings);
        pass.Draw(3);
        pass.End();
    }
    const auto pipelines = job.preparedDepth ? g_preparedPipelines :
                           std::array{g_aoPipeline, g_blurXPipeline, g_blurYPipeline};
    for (size_t i = 0; i < pipelines.size(); ++i) {
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
    job.composite = make_bind_group(ctx.device, job, job.ao[0], true);
    ++g_encoded;
}

void composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
               const void *payload, size_t, void *) {
    auto &job = payload_job(payload);
    auto found = std::find_if(g_compositePipelines.begin(), g_compositePipelines.end(), [&](const auto &p) {
        return p.color == ctx.colorFormat && p.depth == ctx.depthFormat && p.samples == ctx.sampleCount &&
               p.preparedDepth == job.preparedDepth;
    });
    if (found == g_compositePipelines.end()) {
        g_compositePipelines.push_back({ctx.colorFormat, ctx.depthFormat, ctx.sampleCount,
            make_pipeline(ctx.device, "fs_composite", true, ctx.colorFormat, ctx.depthFormat, ctx.sampleCount, job.preparedDepth),
            job.preparedDepth});
        found = g_compositePipelines.end() - 1;
    }
    pass.SetViewport(0, 0, float(ctx.targetWidth), float(ctx.targetHeight), 0, 1);
    pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);
    pass.SetPipeline(found->pipeline);
    pass.SetBindGroup(0, job.composite);
    pass.Draw(3);
    // Aurora restores the GX viewport, scissor and pipeline state afterwards.
    ++g_composited;
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

extern "C" void mp6_ao_begin_frame(void) {
    ++g_frame;
    for (auto &epoch:g_depthEpochs) epoch={};
    for (auto &decal : g_decals) decal = {};
    for (auto &foliage:g_foliage) foliage={};
    g_foliageCamera=-1;
}

extern "C" void mp6_ao_camera_begin(int camera) {
    if (camera<0 || camera>=int(g_depthEpochs.size())) return;
    auto &epoch=g_depthEpochs[camera];
    epoch={};
    if (mp6_enh_ambient_occlusion())
        mp6_ao_depth_epoch_reset(&epoch,gfx::efb_depth_write_serial());
}

extern "C" void mp6_ao_depth_cleared(int camera) {
    // The clear quad itself writes depth, but leaves no scene geometry for AO.
    mp6_ao_camera_begin(camera);
}

extern "C" int mp6_ao_camera_has_depth(int camera) {
    if (!mp6_enh_ambient_occlusion() || camera<0 || camera>=int(g_depthEpochs.size())) return 0;
    return mp6_ao_depth_epoch_has_writes(&g_depthEpochs[camera],gfx::efb_depth_write_serial());
}

extern "C" int mp6_ao_foliage_begin(int camera) {
    if (camera<0 || camera>=int(g_foliage.size()) || g_foliageCamera!=-1) return 0;
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
    return 1;
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
    p.depth[3] = level == 2 ? 0.35f : 0.20f;
    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {
        job->available.store(true, std::memory_order_release);
        warn("scene encoder task rejected");
        return;
    }
    // push_encoder_task drains GX before sealing; record that exact boundary.
    mp6_fi_note_ao(view);
    // If a draw is rejected, retain the slot until shutdown: its encoder task
    // may still be pending. Never hand a pending task another frame's values.
    if (!gfx::push_custom_draw(g_drawType, &job, sizeof(job))) warn("scene composite rejected");
}

extern "C" void mp6_ao_shutdown(void) {
    if (g_drawType == gfx::InvalidDrawType && g_foliageSeedType == gfx::InvalidDrawType) return;
    gfx::synchronize();
    if (std::getenv("MP6_AO_DIAG"))
        std::fprintf(stderr, "[MP6-AO] encoded=%llu composited=%llu foliage=%llu\n",
                     (unsigned long long)g_encoded, (unsigned long long)g_composited,
                     (unsigned long long)g_foliageSeeded);
    gfx::unregister_draw_type(g_drawType);
    gfx::unregister_encoder_task_type(g_taskType);
    gfx::unregister_draw_type(g_foliageSeedType);
    g_foliageSeedType=gfx::InvalidDrawType;
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
        job.width = job.height = 0;
        job.preparedDepth = false;
        job.available.store(true, std::memory_order_release);
    }
    g_compositePipelines.clear();
    g_seedPipelines.clear();
    for (auto &layout:g_seedDepthLayouts) layout=nullptr;
    for (auto &shader:g_seedDepthShaders) shader=nullptr;
    for (auto &seed:g_foliageSeeds) { seed.depth=nullptr; seed.available.store(true); }
    for (auto &foliage:g_foliage) foliage={};
    g_emptyFoliage=nullptr; g_emptyFoliageTexture=nullptr; g_maskLayout=nullptr; g_seedUniform=nullptr;
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
