#pragma once

#include <aurora/gfx.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include "adapter_shader.hpp"
#include "half_adapter_shader.hpp"
#include "generated_shaders.hpp"
#include "generated_r16_shaders.hpp"
#include "parameters.hpp"
#include "initialization_guard.hpp"
#include "../ao_serial_scratch_pool.hpp"
#include "../ao_uniform_upload_cache.hpp"

namespace mp6::xegtao {

struct AdapterParams {
    float depth[4], viewport[4], scissor[4];
};
static_assert(sizeof(AdapterParams) == 48);

enum class DepthInput { ViewZ, CanonicalNdc };

// A serially leased bundle of the four physical textures used by the native
// XeGTAO passes. The aliases below intentionally preserve the disjoint
// producer/consumer plan: edges aliases raw depth and AO1 aliases normals.
struct Scratch {
    uint32_t width = 0, height = 0;
    DepthStorage depthStorage = DepthStorage::Float32;
    wgpu::Texture rawDepth, depthMips, normals, edges;
    std::array<wgpu::Texture, 2> ao;
    wgpu::TextureView rawView, depthView, normalView, edgeView;
    std::array<wgpu::TextureView, 5> mipViews;
    std::array<wgpu::TextureView, 2> aoViews;
    wgpu::BindGroup prefilter, normal, main;
    std::array<wgpu::BindGroup, 2> denoise;
};

// Per-job state. The parent does not reuse a Job within the same frame, so
// queue writes cannot overwrite another camera's uniforms. Scratch textures
// are leased separately and are never used for per-job uniform state.
struct Resources {
    uint32_t finalAoIndex = 0;
    wgpu::Buffer constants, adapter;
    mp6::ao::UniformUploadCache<XeGTAO::GTAOConstants> constantsUpload;
    mp6::ao::UniformUploadCache<AdapterParams> adapterUpload;
    wgpu::BindGroup common;
    std::shared_ptr<Scratch> scratch;
};

class Pipeline {
    bool checked = false, supported = false;
    std::shared_ptr<std::atomic<unsigned>> pendingVerdict;
    bool batchedCompute = false;
    Policy policy{};
    wgpu::TextureFormat workingDepthFormat = wgpu::TextureFormat::R32Float;
    wgpu::BindGroupLayout emptyLayout, commonLayout, prefilterLayout, normalLayout,
                          mainLayout, denoiseLayout, canonicalLayout;
    wgpu::BindGroup emptyGroup;
    wgpu::Sampler pointClamp;
    wgpu::Texture hilbertLut;
    wgpu::TextureView hilbertView;
    bool hilbertUploaded = false;
    mp6::ao::SerialScratchPool<Scratch> scratchPool;
    std::array<wgpu::ComputePipeline, 5> stages;
    wgpu::RenderPipeline canonical;

    static wgpu::BindGroupLayoutEntry uniform(uint32_t binding, uint64_t size,
                                             wgpu::ShaderStage visibility) {
        wgpu::BindGroupLayoutEntry entry{};
        entry.binding = binding;
        entry.visibility = visibility;
        entry.buffer.type = wgpu::BufferBindingType::Uniform;
        entry.buffer.minBindingSize = size;
        return entry;
    }

    static wgpu::BindGroupLayoutEntry texture(uint32_t binding, wgpu::TextureSampleType type,
                                             wgpu::ShaderStage visibility = wgpu::ShaderStage::Compute) {
        wgpu::BindGroupLayoutEntry entry{};
        entry.binding = binding;
        entry.visibility = visibility;
        entry.texture.sampleType = type;
        entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        return entry;
    }

    static wgpu::BindGroupLayoutEntry storage(uint32_t binding, wgpu::TextureFormat format) {
        wgpu::BindGroupLayoutEntry entry{};
        entry.binding = binding;
        entry.visibility = wgpu::ShaderStage::Compute;
        entry.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
        entry.storageTexture.format = format;
        entry.storageTexture.viewDimension = wgpu::TextureViewDimension::e2D;
        return entry;
    }

    static wgpu::BindGroupLayout layout(const wgpu::Device &device, const char *label,
                                        std::initializer_list<wgpu::BindGroupLayoutEntry> entries) {
        const wgpu::BindGroupLayoutDescriptor desc{
            .label = label, .entryCount = entries.size(), .entries = entries.begin(),
        };
        return device.CreateBindGroupLayout(&desc);
    }

    static wgpu::BindGroup bindings(const wgpu::Device &device, const wgpu::BindGroupLayout &layout,
                                    std::initializer_list<wgpu::BindGroupEntry> entries) {
        const wgpu::BindGroupDescriptor desc{
            .label = "MP6 XeGTAO resources", .layout = layout,
            .entryCount = entries.size(), .entries = entries.begin(),
        };
        return device.CreateBindGroup(&desc);
    }

    static wgpu::ShaderModule module(const wgpu::Device &device, const char *code, const char *label) {
        const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = code}};
        const wgpu::ShaderModuleDescriptor desc{.nextInChain = &source, .label = label};
        return device.CreateShaderModule(&desc);
    }

    wgpu::ComputePipeline compute_pipeline(const wgpu::Device &device, const char *code,
                                           const char *entry, uint32_t group,
                                           const wgpu::BindGroupLayout &stageLayout) {
#ifdef MP6_XEGTAO_QA_INVALID_SHADER
        // Defined only for the isolated board-QA object, never a normal build.
        const char *inject = std::getenv("MP6_QA_XEGTAO_INVALID_SHADER");
        if (inject && std::strcmp(inject, "1") == 0 && std::strncmp(entry, "CSGTAO", 6) == 0) {
            std::fprintf(stderr, "[MP6-AO-QA] injecting invalid XeGTAO main shader\n");
            code = "this is deliberately invalid WGSL for the initialization failure test";
        }
#endif
        std::array<wgpu::BindGroupLayout, 4> layouts{commonLayout, emptyLayout, emptyLayout, emptyLayout};
        layouts[group] = stageLayout;
        const wgpu::PipelineLayoutDescriptor layoutDesc{
            .bindGroupLayoutCount = group + 1, .bindGroupLayouts = layouts.data(),
        };
        const auto pipelineLayout = device.CreatePipelineLayout(&layoutDesc);
        const wgpu::ComputePipelineDescriptor desc{
            .label = entry, .layout = pipelineLayout,
            .compute = {.module = module(device, code, entry), .entryPoint = entry},
        };
        return device.CreateComputePipeline(&desc);
    }

    static wgpu::RenderPipeline raster_pipeline(const wgpu::Device &device, const wgpu::ShaderModule &shader,
                                                const wgpu::BindGroupLayout &layout, const char *entry,
                                                wgpu::TextureFormat format) {
        const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
        const auto pipelineLayout = device.CreatePipelineLayout(&layoutDesc);
        const wgpu::ColorTargetState target{.format = format};
        const wgpu::FragmentState fragment{.module = shader, .entryPoint = entry, .targetCount = 1, .targets = &target};
        const wgpu::RenderPipelineDescriptor desc{
            .label = entry, .layout = pipelineLayout,
            .vertex = {.module = shader, .entryPoint = "vs_main"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList}, .fragment = &fragment,
        };
        return device.CreateRenderPipeline(&desc);
    }

    static wgpu::Texture image(const wgpu::Device &device, const char *label, uint32_t width,
                               uint32_t height, wgpu::TextureFormat format,
                               wgpu::TextureUsage usage, uint32_t mips = 1) {
        const wgpu::TextureDescriptor desc{
            .label = label, .usage = usage, .size = {width, height, 1}, .format = format, .mipLevelCount = mips,
        };
        return device.CreateTexture(&desc);
    }

    bool ensure_hilbert_lut(const wgpu::Device &device, const wgpu::Queue &queue) {
        if (!hilbertLut) {
            hilbertLut = image(device, "XeGTAO Hilbert LUT", XE_HILBERT_WIDTH, XE_HILBERT_WIDTH,
                               wgpu::TextureFormat::R32Uint,
                               wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
            if (!hilbertLut) return false;
            hilbertView = hilbertLut.CreateView();
        }
        if (!hilbertView || !queue) return false;
        if (hilbertUploaded) return true;

        std::array<uint32_t, XE_HILBERT_AREA> values{};
        for (uint32_t y = 0; y < XE_HILBERT_WIDTH; ++y)
            for (uint32_t x = 0; x < XE_HILBERT_WIDTH; ++x)
                values[y * XE_HILBERT_WIDTH + x] = XeGTAO::HilbertIndex(x, y);
        const wgpu::TexelCopyTextureInfo destination{.texture = hilbertLut};
        const wgpu::TexelCopyBufferLayout layout{
            .bytesPerRow = XE_HILBERT_WIDTH * sizeof(uint32_t), .rowsPerImage = XE_HILBERT_WIDTH,
        };
        const wgpu::Extent3D size{XE_HILBERT_WIDTH, XE_HILBERT_WIDTH, 1};
        queue.WriteTexture(&destination, values.data(), values.size() * sizeof(uint32_t), &layout, &size);
        hilbertUploaded = true;
        return true;
    }

    // Keep per-job uniform storage private. Recreating either buffer also
    // invalidates only that job's byte snapshots; scratch textures are leased
    // independently and must not be reset here.
    void allocate(const wgpu::Device &device, Resources &r) {
        if (r.constants && r.adapter && r.common) return;
        const wgpu::BufferDescriptor constantsDesc{
            .label = "XeGTAO constants", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = sizeof(XeGTAO::GTAOConstants),
        };
        const wgpu::BufferDescriptor adapterDesc{
            .label = "XeGTAO adapter parameters", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = sizeof(AdapterParams),
        };
        if (!r.constants) {
            r.constants = device.CreateBuffer(&constantsDesc);
            r.constantsUpload.reset();
        }
        if (!r.adapter) {
            r.adapter = device.CreateBuffer(&adapterDesc);
            r.adapterUpload.reset();
        }
        if (r.common) return;
        r.common = bindings(device, commonLayout, {
            {.binding = 0, .buffer = r.constants, .size = sizeof(XeGTAO::GTAOConstants)},
            {.binding = 1, .sampler = pointClamp},
        });
    }

    std::shared_ptr<Scratch> create_scratch(const wgpu::Device &device,
                                            uint32_t width, uint32_t height) {
        auto scratch = std::make_shared<Scratch>();
        scratch->width = width;
        scratch->height = height;
        scratch->depthStorage = policy.depthStorage;
        const auto computeUsage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding;
        scratch->rawDepth = image(device, "XeGTAO canonical depth / edges", width, height,
                                  wgpu::TextureFormat::R32Float,
                                  computeUsage | wgpu::TextureUsage::RenderAttachment);
        scratch->rawView = scratch->rawDepth.CreateView();
        scratch->depthMips = image(device, "XeGTAO depth MIPs", width, height,
                                    workingDepthFormat, computeUsage, 5);
        scratch->depthView = scratch->depthMips.CreateView();
        for (uint32_t i = 0; i < scratch->mipViews.size(); ++i) {
            const wgpu::TextureViewDescriptor view{.baseMipLevel = i, .mipLevelCount = 1};
            scratch->mipViews[i] = scratch->depthMips.CreateView(&view);
        }
        scratch->normals = image(device, "XeGTAO packed normals / AO1", width, height,
                                  wgpu::TextureFormat::R32Uint, computeUsage);
        scratch->normalView = scratch->normals.CreateView();
        // Separate passes make these lifetimes disjoint: normals is the last
        // raw-depth reader before main writes edges; main is the last normal
        // reader before denoise writes AO1. Bind only each pass's current role.
        // Both composites consume clean depth and final AO, not normals. The
        // experimental half path also reads retained linear depth MIP zero.
        scratch->edges = scratch->rawDepth;
        scratch->edgeView = scratch->rawView;
        scratch->ao[0] = image(device, "XeGTAO scalar AO0", width, height,
                               wgpu::TextureFormat::R32Uint, computeUsage);
        scratch->aoViews[0] = scratch->ao[0].CreateView();
        scratch->ao[1] = scratch->normals;
        scratch->aoViews[1] = scratch->normalView;
        scratch->prefilter = bindings(device, prefilterLayout, {
            {.binding = 0, .textureView = scratch->rawView},
            {.binding = 1, .textureView = scratch->mipViews[0]}, {.binding = 2, .textureView = scratch->mipViews[1]},
            {.binding = 3, .textureView = scratch->mipViews[2]}, {.binding = 4, .textureView = scratch->mipViews[3]},
            {.binding = 5, .textureView = scratch->mipViews[4]},
        });
        scratch->normal = bindings(device, normalLayout, {
            {.binding = 0, .textureView = scratch->rawView}, {.binding = 1, .textureView = scratch->normalView},
        });
        scratch->main = bindings(device, mainLayout, {
            {.binding = 0, .textureView = scratch->depthView}, {.binding = 1, .textureView = scratch->normalView},
            {.binding = 2, .textureView = hilbertView},
            {.binding = 3, .textureView = scratch->aoViews[0]}, {.binding = 4, .textureView = scratch->edgeView},
        });
        for (size_t i = 0; i < scratch->denoise.size(); ++i) {
            scratch->denoise[i] = bindings(device, denoiseLayout, {
                {.binding = 2, .textureView = scratch->aoViews[i]}, {.binding = 3, .textureView = scratch->edgeView},
                {.binding = 4, .textureView = scratch->aoViews[1 - i]},
            });
        }
        return scratch;
    }

    std::shared_ptr<Scratch> acquire_scratch(const wgpu::Device &device,
                                             uint32_t width, uint32_t height) {
        return scratchPool.acquire(
            [width, height, depthStorage = policy.depthStorage](const Scratch &scratch) {
                return scratch.width == width && scratch.height == height &&
                       scratch.depthStorage == depthStorage;
            },
            [&] { return create_scratch(device, width, height); });
    }

    static void raster(const wgpu::CommandEncoder &encoder, const wgpu::RenderPipeline &pipeline,
                       const wgpu::BindGroup &group, const wgpu::TextureView &destination, const char *label) {
        const wgpu::RenderPassColorAttachment color{
            .view = destination, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
            .clearValue = {1, 0, 0, 0},
        };
        const wgpu::RenderPassDescriptor desc{
            .label = label, .colorAttachmentCount = 1, .colorAttachments = &color,
#ifdef AURORA_GPU_STAT_ROWS
            .timestampWrites = aurora::gfx::gpu_pass_timestamps(label),
#endif
        };
        auto pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, group);
        pass.Draw(3);
        pass.End();
    }

    void dispatch_batched(const wgpu::CommandEncoder &encoder, const Resources &r,
                          const Scratch &scratch, const wgpu::BindGroup &prefilter,
                          const wgpu::BindGroup &normal, const wgpu::BindGroup &main,
                          const std::array<wgpu::BindGroup, 2> &denoise) {
        const wgpu::ComputePassDescriptor desc{
            .label = "XeGTAO compute",
#ifdef AURORA_GPU_STAT_ROWS
            // The whole submission is one measured unit. Stage-level timing
            // writes here would nest/duplicate the same GPU work.
            .timestampWrites = aurora::gfx::gpu_pass_timestamps("XeGTAO compute"),
#endif
        };
        auto pass = encoder.BeginComputePass(&desc);
        const auto dispatch_stage = [&](size_t stage, const wgpu::BindGroup &group,
                                        uint32_t groupIndex, uint32_t tileWidth,
                                        uint32_t tileHeight) {
            pass.SetPipeline(stages[stage]);
            pass.SetBindGroup(0, r.common);
            // Bind groups survive pipeline switches within a pass. Reassert
            // every lower slot for each stage so no prior resource aliases a
            // new stage's storage or sampled view.
            for (uint32_t gap = 1; gap < groupIndex; ++gap) pass.SetBindGroup(gap, emptyGroup);
            pass.SetBindGroup(groupIndex, group);
            pass.DispatchWorkgroups((scratch.width + tileWidth - 1) / tileWidth,
                                    (scratch.height + tileHeight - 1) / tileHeight);
        };
        dispatch_stage(0, prefilter, 1, 16, 16);
        dispatch_stage(1, normal, 3, 8, 8);
        dispatch_stage(2, main, 2, 8, 8);
        for (uint32_t i = 0; i + 1 < policy.denoisePasses; ++i)
            dispatch_stage(3, denoise[i % 2], 3, 16, 8);
        dispatch_stage(4, denoise[1 - r.finalAoIndex], 3, 16, 8);
        pass.End();
    }

    void dispatch(const wgpu::CommandEncoder &encoder, size_t stage, const Resources &r,
                  const Scratch &scratch, const wgpu::BindGroup &group, uint32_t groupIndex,
                  uint32_t tileWidth, uint32_t tileHeight, const char *label) {
        const wgpu::ComputePassDescriptor desc{
            .label = label,
#ifdef AURORA_GPU_STAT_ROWS
            .timestampWrites = aurora::gfx::gpu_pass_timestamps(label),
#endif
        };
        auto pass = encoder.BeginComputePass(&desc);
        pass.SetPipeline(stages[stage]);
        pass.SetBindGroup(0, r.common);
        for (uint32_t gap = 1; gap < groupIndex; ++gap) pass.SetBindGroup(gap, emptyGroup);
        pass.SetBindGroup(groupIndex, group);
        // WGSL textureStore discards invalid texel coordinates. Keep all
        // prefilter lanes alive, including the ones participating in barriers.
        pass.DispatchWorkgroups((scratch.width + tileWidth - 1) / tileWidth,
                                (scratch.height + tileHeight - 1) / tileHeight);
        pass.End();
    }

    bool initialization_failed(const char *reason) {
        // Drop partial resources but cache the failure: no retries every frame.
        *this = Pipeline{};
        checked = true;
        std::fprintf(stderr, "[MP6-AO] XeGTAO unavailable: %s; using legacy\n", reason);
        return false;
    }

    // Browser validation verdicts arrive after initialize() returns; the
    // legacy path keeps drawing until the device accepts every object.
    bool accept_deferred() {
        if (!pendingVerdict) return true;
        const unsigned verdict = pendingVerdict->load(std::memory_order_acquire);
        if (verdict == 0) return false;
        pendingVerdict.reset();
        if (verdict != 1) return initialization_failed("GPU initialization rejected");
        std::fprintf(stderr, "[MP6-AO] XeGTAO accepted by the browser device\n");
        return true;
    }

public:
    bool unavailable() const noexcept { return checked && !supported; }

    const std::shared_ptr<Scratch> &retained_scratch() const noexcept {
        return scratchPool.retained();
    }

    void release_idle_scratch() { scratchPool.reset(); }

    bool initialize(const wgpu::Device &device, const Policy &selectedPolicy = {}) {
        if (checked) return supported && accept_deferred();
        assert(valid_policy(selectedPolicy));
        checked = true;
        const char *batchComputeEnv = std::getenv("MP6_AO_XEGTAO_BATCH_COMPUTE");
        batchedCompute = batchComputeEnv && std::strcmp(batchComputeEnv, "1") == 0;
        if (!device) return initialization_failed("missing device");
        policy = selectedPolicy;
        bool r16StorageSupported = false;
#ifdef WEBGPU_DAWN
        // Query the enabled device, not only the adapter: the format feature
        // must have been requested before device creation. Unsupported devices
        // keep the R32 textures AND their matching upstream FP32-depth bias.
        r16StorageSupported = device.HasFeature(wgpu::FeatureName::TextureFormatsTier1);
#endif
        policy.depthStorage = supported_depth_storage(selectedPolicy.depthStorage, r16StorageSupported);
        const bool r16 = policy.depthStorage == DepthStorage::Float16;
        workingDepthFormat = r16 ? wgpu::TextureFormat::R16Float : wgpu::TextureFormat::R32Float;
        wgpu::Limits limits{};
        if (!device.GetLimits(&limits) || limits.maxStorageTexturesPerShaderStage == WGPU_LIMIT_U32_UNDEFINED ||
            limits.maxStorageTexturesPerShaderStage < 5 || limits.maxBindGroups == WGPU_LIMIT_U32_UNDEFINED ||
            limits.maxBindGroups < 4 || limits.maxSampledTexturesPerShaderStage == WGPU_LIMIT_U32_UNDEFINED ||
            limits.maxSampledTexturesPerShaderStage < 3) {
            std::fprintf(stderr, "[MP6-AO] XeGTAO unavailable: device needs 5 storage textures, 3 sampled textures, and 4 bind groups; using legacy\n");
            return false;
        }
        InitializationGuard validation(device);
        if (!validation.ready()) return initialization_failed("provider cannot validate initialization");
        if (!ensure_hilbert_lut(device, device.GetQueue())) {
            validation.finish();
            return initialization_failed("failed to initialize the Hilbert LUT");
        }
        using ST = wgpu::TextureSampleType;
        using TF = wgpu::TextureFormat;
        constexpr auto CS = wgpu::ShaderStage::Compute;
        constexpr auto FS = wgpu::ShaderStage::Fragment;
        emptyLayout = layout(device, "XeGTAO empty group", {});
        emptyGroup = bindings(device, emptyLayout, {});
        wgpu::BindGroupLayoutEntry samplerEntry{};
        samplerEntry.binding = 1;
        samplerEntry.visibility = CS;
        samplerEntry.sampler.type = wgpu::SamplerBindingType::NonFiltering;
        commonLayout = layout(device, "XeGTAO common", {uniform(0, sizeof(XeGTAO::GTAOConstants), CS), samplerEntry});
        prefilterLayout = layout(device, "XeGTAO prefilter", {
            texture(0, ST::UnfilterableFloat), storage(1, workingDepthFormat), storage(2, workingDepthFormat),
            storage(3, workingDepthFormat), storage(4, workingDepthFormat), storage(5, workingDepthFormat),
        });
        normalLayout = layout(device, "XeGTAO normals", {texture(0, ST::UnfilterableFloat), storage(1, TF::R32Uint)});
        mainLayout = layout(device, "XeGTAO main", {
            texture(0, ST::UnfilterableFloat), texture(1, ST::Uint), texture(2, ST::Uint),
            storage(3, TF::R32Uint), storage(4, TF::R32Float),
        });
        denoiseLayout = layout(device, "XeGTAO denoise", {
            texture(2, ST::Uint), texture(3, ST::UnfilterableFloat), storage(4, TF::R32Uint),
        });
        canonicalLayout = layout(device, "XeGTAO canonical adapter", {
            uniform(0, sizeof(AdapterParams), FS), texture(1, ST::UnfilterableFloat, FS),
        });
        const wgpu::SamplerDescriptor samplerDesc{
            .label = "XeGTAO nearest clamp", .addressModeU = wgpu::AddressMode::ClampToEdge,
            .addressModeV = wgpu::AddressMode::ClampToEdge, .addressModeW = wgpu::AddressMode::ClampToEdge,
            .magFilter = wgpu::FilterMode::Nearest, .minFilter = wgpu::FilterMode::Nearest,
            .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
        };
        pointClamp = device.CreateSampler(&samplerDesc);
        stages[0] = compute_pipeline(device, r16 ? r16_shaders::CSPrefilterDepths16x16 : shaders::CSPrefilterDepths16x16,
                                     "CSPrefilterDepths16x16", 1, prefilterLayout);
        stages[1] = compute_pipeline(device, shaders::CSGenerateNormals, "CSGenerateNormals", 3, normalLayout);
        const char *mainCode = shaders::CSGTAOHigh;
        if (policy.quality == Quality::Low) mainCode = shaders::CSGTAOLow;
        else if (policy.quality == Quality::Medium) mainCode = shaders::CSGTAOMedium;
        else if (policy.quality == Quality::Ultra) mainCode = shaders::CSGTAOUltra;
        if (r16) {
            mainCode = r16_shaders::CSGTAOHigh;
            if (policy.quality == Quality::Low) mainCode = r16_shaders::CSGTAOLow;
            else if (policy.quality == Quality::Medium) mainCode = r16_shaders::CSGTAOMedium;
            else if (policy.quality == Quality::Ultra) mainCode = r16_shaders::CSGTAOUltra;
        }
        stages[2] = compute_pipeline(device, mainCode, main_entry_point(policy.quality), 2, mainLayout);
        if (policy.denoisePasses > 1)
            stages[3] = compute_pipeline(device, shaders::CSDenoisePass, "CSDenoisePass", 3, denoiseLayout);
        stages[4] = compute_pipeline(device, shaders::CSDenoiseLastPass, "CSDenoiseLastPass", 3, denoiseLayout);
        const auto adapter = module(device, policy.resolution == Resolution::Half ? HalfAdapterShader : AdapterShader,
                                    "MP6 XeGTAO adapters");
        canonical = raster_pipeline(device, adapter, canonicalLayout, "fs_canonical_depth", TF::R32Float);
        const bool handlesValid = emptyLayout && emptyGroup && commonLayout && prefilterLayout &&
            normalLayout && mainLayout && denoiseLayout && canonicalLayout && pointClamp &&
            stages[0] && stages[1] && stages[2] && (policy.denoisePasses == 1 || stages[3]) &&
            stages[4] && canonical;
        // Error handles can propagate through creation inside these scopes.
        // Drain them before accepting the handles or publishing ready.
        if (!validation.finish() || !handlesValid)
            return initialization_failed("GPU initialization rejected or validation wait failed");
        supported = true;
        pendingVerdict = validation.deferred();
        std::fprintf(stderr, "[MP6-AO] XeGTAO depth storage: requested=%s actual=%s\n",
                     depth_storage_name(selectedPolicy.depthStorage), depth_storage_name(policy.depthStorage));
        if (selectedPolicy.depthStorage != policy.depthStorage)
            std::fprintf(stderr, "[MP6-AO] R16 storage feature unavailable on enabled device; using R32 textures and shaders\n");
        if (policy.resolution == Resolution::Half)
            std::fprintf(stderr, "[MP6-AO] XeGTAO experimental: half-resolution bilateral uint output, %s, %u spatial denoise pass%s; opt-in only\n",
                         quality_name(policy.quality), policy.denoisePasses, policy.denoisePasses == 1 ? "" : "es");
        else
            std::fprintf(stderr, "[MP6-AO] XeGTAO %s: full-resolution direct uint output, %s, %u spatial denoise pass%s\n",
                         policy.quality == Quality::High && policy.denoisePasses == 2 && !r16 ? "reference" : "experimental",
                         quality_name(policy.quality), policy.denoisePasses, policy.denoisePasses == 1 ? "" : "es");
        if (batchedCompute)
            std::fprintf(stderr, "[MP6-AO] XeGTAO compute submission: batched\n");
        return accept_deferred();
    }

    void reset() { *this = {}; }

    // Canonical attachment preparation runs before encode(), using the same
    // private uniform as the ordinary adapter. A Job is never reused within
    // its frame, so writes cannot replace another queued camera's parameters.
    void prepare_adapter(const wgpu::Device &device, const wgpu::Queue &queue,
                         Resources &r, const AdapterParams &adapter) {
        assert(supported);
        allocate(device, r);
        r.adapterUpload.upload_if_changed(adapter, [&](const void *bytes, size_t size) {
            queue.WriteBuffer(r.adapter, 0, bytes, size);
        });
    }

    // Parent validates dimensions/constants and initialize() before dispatch.
    // cleanDepth stays full resolution; final AO uses the explicitly selected
    // working resolution. The original game depth attachment is never retained
    // here or written to.
    void encode(const wgpu::Device &device, const wgpu::Queue &queue, const wgpu::CommandEncoder &encoder,
                Resources &r, const wgpu::TextureView &cleanDepth,
                const XeGTAO::GTAOConstants &constants, const AdapterParams &adapter,
                DepthInput input = DepthInput::ViewZ) {
        assert(supported && constants.ViewportSize.x > 0 && constants.ViewportSize.y > 0 &&
               std::max(constants.ViewportSize.x, constants.ViewportSize.y) >= 16 &&
               hilbertLut && hilbertView && hilbertUploaded);
        assert(input != DepthInput::CanonicalNdc || policy.resolution == Resolution::Full);
        const uint32_t width = uint32_t(constants.ViewportSize.x);
        const uint32_t height = uint32_t(constants.ViewportSize.y);
        if (input == DepthInput::ViewZ) prepare_adapter(device, queue, r, adapter);
        // Canonical mode was prepared by the parent before its depth pass.
        assert(r.constants && r.adapter && r.common);
        r.scratch = acquire_scratch(device, width, height);
        assert(r.scratch);
        const Scratch &s = *r.scratch;
        r.finalAoIndex = final_ao_index(policy);
        r.constantsUpload.upload_if_changed(constants, [&](const void *bytes, size_t size) {
            queue.WriteBuffer(r.constants, 0, bytes, size);
        });
        wgpu::BindGroup externalPrefilter, externalNormal;
        if (input == DepthInput::CanonicalNdc) {
            // Cache keys contain every view identity, including all output
            // mip/normal views. Never mutate the shared Scratch groups or
            // redirect rawView: main later writes edges through that alias.
            const std::array prefilterEntries{
                wgpu::BindGroupEntry{.binding = 0, .textureView = cleanDepth},
                wgpu::BindGroupEntry{.binding = 1, .textureView = s.mipViews[0]},
                wgpu::BindGroupEntry{.binding = 2, .textureView = s.mipViews[1]},
                wgpu::BindGroupEntry{.binding = 3, .textureView = s.mipViews[2]},
                wgpu::BindGroupEntry{.binding = 4, .textureView = s.mipViews[3]},
                wgpu::BindGroupEntry{.binding = 5, .textureView = s.mipViews[4]},
            };
            const wgpu::BindGroupDescriptor prefilterDesc{
                .label = "XeGTAO prepared canonical MIPs", .layout = prefilterLayout,
                .entryCount = prefilterEntries.size(), .entries = prefilterEntries.data(),
            };
            externalPrefilter = aurora::gfx::cached_bind_group(prefilterDesc);
            const std::array normalEntries{
                wgpu::BindGroupEntry{.binding = 0, .textureView = cleanDepth},
                wgpu::BindGroupEntry{.binding = 1, .textureView = s.normalView},
            };
            const wgpu::BindGroupDescriptor normalDesc{
                .label = "XeGTAO prepared canonical normals", .layout = normalLayout,
                .entryCount = normalEntries.size(), .entries = normalEntries.data(),
            };
            externalNormal = aurora::gfx::cached_bind_group(normalDesc);
        } else {
        // The canonical group references the parent-owned clean depth, which
        // may change without an AO size change. The cache key includes view identity.
        const std::array canonicalEntries{
            wgpu::BindGroupEntry{.binding = 0, .buffer = r.adapter, .size = sizeof(AdapterParams)},
            wgpu::BindGroupEntry{.binding = 1, .textureView = cleanDepth},
        };
        const wgpu::BindGroupDescriptor canonicalDesc{
            .label = "XeGTAO depth adapter", .layout = canonicalLayout,
            .entryCount = canonicalEntries.size(), .entries = canonicalEntries.data(),
        };
        const auto canonicalGroup = aurora::gfx::cached_bind_group(canonicalDesc);
        raster(encoder, canonical, canonicalGroup, s.rawView, "XeGTAO canonical depth");
        }
        if (batchedCompute) {
            dispatch_batched(encoder, r, s,
                             input == DepthInput::CanonicalNdc ? externalPrefilter : s.prefilter,
                             input == DepthInput::CanonicalNdc ? externalNormal : s.normal,
                             s.main, s.denoise);
            return;
        }
        dispatch(encoder, 0, r, s, input == DepthInput::CanonicalNdc ? externalPrefilter : s.prefilter,
                 1, 16, 16, "XeGTAO depth MIPs");
        dispatch(encoder, 1, r, s, input == DepthInput::CanonicalNdc ? externalNormal : s.normal,
                 3, 8, 8, "XeGTAO normals");
        dispatch(encoder, 2, r, s, s.main, 2, 8, 8, "XeGTAO main");
        for (uint32_t i = 0; i + 1 < policy.denoisePasses; ++i)
            dispatch(encoder, 3, r, s, s.denoise[i % 2], 3, 16, 8, "XeGTAO denoise");
        dispatch(encoder, 4, r, s, s.denoise[1 - r.finalAoIndex], 3, 16, 8, "XeGTAO final denoise");
    }
};

} // namespace mp6::xegtao
