#include "gpu.hpp"
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <magic_enum.hpp>
#include <webgpu/webgpu_cpp.h>

#include "../gfx/common.hpp"
#include "../gfx/render_worker.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "gpu_prof.hpp"

#ifdef WEBGPU_DAWN
#include "../dawn/BackendBinding.hpp"
#include "../dawn/TracyPlatform.hpp"
#ifndef __MINGW32__
#ifndef __EMSCRIPTEN__
#include <dawn/native/DawnNative.h>
#endif
#endif
#endif

namespace aurora::gx {
void clear_copy_texture_cache() noexcept;
} // namespace aurora::gx
namespace aurora::gfx {
void clear_offscreen_cache();
} // namespace aurora::gfx

namespace aurora::webgpu {
static Module Log("aurora::gpu");

wgpu::Device g_device;
wgpu::Queue g_queue;
wgpu::Surface g_surface;
wgpu::BackendType g_backendType;
GraphicsConfig g_graphicsConfig;
TextureWithSampler g_frameBuffer;
TextureWithSampler g_frameBufferResolved;
TextureWithSampler g_depthBuffer;

// EFB -> XFB copy pipeline
static wgpu::BindGroupLayout g_CopyBindGroupLayout;
wgpu::RenderPipeline g_CopyPipeline;
wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
wgpu::BindGroup g_CopyBindGroup;
static AuroraSampler g_Resampler = SAMPLER_BILINEAR;
// Anti-Aliasing P2 (aurora-patches/0017): live post-process AA mode fed into
// the present resample fragment shader. Toggled by aurora_set_post_aa without
// recreating any pipeline (mirrors g_Resampler), so it applies immediately.
// ATOMIC because the two ends live on different threads: aurora_set_post_aa is
// called from the game/UI thread (the launcher's Anti-Aliasing row, and once at
// boot), while resample_present_source reads it on the render worker (that
// function ASSERTs is_worker_thread()). Relaxed ordering is sufficient -- the
// value is self-contained and is republished into the resample uniform every
// present, so no other state is ordered against it; all the atomic buys (and
// all that is needed) is that a live toggle can never be read half-written.
static std::atomic<AuroraPostAA> g_PostAa{AURORA_POST_AA_NONE};
static wgpu::BindGroupLayout g_ResampleBindGroupLayout;
static wgpu::RenderPipeline g_ResamplePipeline;
static wgpu::Buffer g_ResampleUniformBuffer;
static TextureWithSampler g_resampledFrameBuffer;

static wgpu::Adapter g_adapter;
wgpu::Instance g_instance;
#ifdef __EMSCRIPTEN__
AsyncPipelineSink* g_asyncPipelineSink = nullptr;
#endif

wgpu::RenderPipeline create_render_pipeline(const wgpu::RenderPipelineDescriptor& descriptor) {
#ifdef __EMSCRIPTEN__
  if (g_asyncPipelineSink != nullptr) {
    AsyncPipelineSink sink = std::move(*g_asyncPipelineSink);
    g_asyncPipelineSink = nullptr;
    g_device.CreateRenderPipelineAsync(
        &descriptor, wgpu::CallbackMode::AllowProcessEvents,
        [sink = std::move(sink), label = std::string(std::string_view(descriptor.label))](
            wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline pipeline, wgpu::StringView message) {
          if (status != wgpu::CreatePipelineAsyncStatus::Success) {
            // A shader the browser rejects would otherwise just never draw
            // (the draw is skipped until its pipeline exists); say so.
            static uint32_t s_failures = 0;
            if (++s_failures <= 12) {
              Log.error("[MP6-PIPELINE] async pipeline build failed ({}): {} -- {}", s_failures,
                        label.substr(0, 120), std::string_view(message).substr(0, 400));
            }
          }
          sink(status == wgpu::CreatePipelineAsyncStatus::Success ? std::move(pipeline) : wgpu::RenderPipeline{});
        });
    return {};
  }
#endif
  return g_device.CreateRenderPipeline(&descriptor);
}
wgpu::AdapterInfo g_adapterInfo;
static wgpu::SurfaceCapabilities g_surfaceCapabilities;
bool g_hasCoreFeatures = false;
bool g_bcTexturesSupported = false;
bool g_astcTexturesSupported = false;
bool g_textureComponentSwizzleSupported = false;
static std::atomic_bool g_initialized = false;
// Anti-Aliasing P3 (aurora-patches/0018): SSAA supersampling. g_ssaaFactor
// (read from AuroraConfig.ssaa at init) scales the content framebuffer while
// the surface stays native; the existing present-resample pass downsamples.
// 1.0 = off (byte-identical). g_maxTextureDimension2D caches the adapter limit
// so the scaled size is clamped and never exceeds what the device allows.
static float g_ssaaFactor = 1.f;
static uint32_t g_maxTextureDimension2D = 0;

namespace {

#ifndef __EMSCRIPTEN__ // Dawn-native logging hook; the browser logs to the console itself
AuroraLogLevel wgpu_log_level(wgpu::LoggingType type) {
  switch (type) {
  case wgpu::LoggingType::Verbose:
    return LOG_DEBUG;
  case wgpu::LoggingType::Info:
    return LOG_INFO;
  case wgpu::LoggingType::Warning:
    return LOG_WARNING;
  case wgpu::LoggingType::Error:
    return LOG_ERROR;
  default:
    return LOG_FATAL;
  }
}

void wgpu_log(wgpu::LoggingType type, wgpu::StringView message) {
  Log.report(wgpu_log_level(type), "WebGPU message: {}", message);
}
#endif

struct ResampleUniformBlock {
  uint32_t samplerMode = 0;
  float frameWidth = 0.f;
  float frameHeight = 0.f;
  // Anti-Aliasing P2 (aurora-patches/0017): post-process AA selector applied
  // in the present resample fragment shader. 0 = none, 1 = FXAA. Keeps the
  // block at 16 bytes (its allocation is already AURORA_ALIGN'd to 16), so
  // the uniform buffer size/binding is unchanged.
  uint32_t postAaMode = 0;
  // Where the content viewport starts in the target it is drawn into: 0 in the
  // intermediate texture, the letterbox offset when drawn into the surface.
  float frameLeft = 0.f;
  float frameTop = 0.f;
};
static ResampleUniformBlock g_lastResampleUniform;
static bool g_hasResampleUniform = false;

constexpr std::string_view resampleShaderSource = R"(
struct Uniforms {
    sampler_mode: u32,
    frame_width: f32,
    frame_height: f32,
    post_aa_mode: u32,
    frame_origin: vec2<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

// BEGIN AREA SAMPLER REGION

fn sample_by_pixel(pixel: vec2<i32>) -> vec4<f32> {
    let source_dims = textureDimensions(t);
    let max_coord = vec2<i32>(source_dims) - vec2<i32>(1, 1);
    let coord = clamp(pixel, vec2<i32>(0, 0), max_coord);
    return textureLoad(t, coord, 0);
}

fn sample_area(frag_position: vec4<f32>) -> vec4<f32> {
    let source_size = vec2<f32>(textureDimensions(t));
    let target_size = max(vec2<f32>(uniforms.frame_width, uniforms.frame_height), vec2<f32>(1.0, 1.0));

    let frag = frag_position.xy - uniforms.frame_origin;
    let source_min = clamp((frag - vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;
    let source_max = clamp((frag + vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;

    let first_pixel = vec2<i32>(floor(source_min));
    let last_pixel = vec2<i32>(ceil(source_max));
    let max_iterations: i32 = 16;

    var avg_color = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var total_weight = 0.0;

    for (var iy: i32 = 0; iy < max_iterations; iy = iy + 1) {
        let source_y = first_pixel.y + iy;
        if (source_y < last_pixel.y) {
            let y0 = f32(source_y);
            let weight_y = max(min(source_max.y, y0 + 1.0) - max(source_min.y, y0), 0.0);

            for (var ix: i32 = 0; ix < max_iterations; ix = ix + 1) {
                let source_x = first_pixel.x + ix;
                if (source_x < last_pixel.x) {
                    let x0 = f32(source_x);
                    let weight_x = max(min(source_max.x, x0 + 1.0) - max(source_min.x, x0), 0.0);
                    let weight = weight_x * weight_y;
                    avg_color += weight * sample_by_pixel(vec2<i32>(source_x, source_y));
                    total_weight += weight;
                }
            }
        }
    }

    return avg_color / max(total_weight, 0.000001);
}

// END AREA SAMPLER REGION

// BEGIN FXAA REGION (Anti-Aliasing P2, aurora-patches/0017)
//
// Straight-line luma FXAA (Timothy Lottes / NVIDIA FXAA 3.11 "console"
// variant): every textureSampleLevel below runs unconditionally -- no
// edge-search loop, no textureGather, no textureSample in a divergent
// branch -- so the same WGSL lowers cleanly on both D3D12/DXC (use_dxc)
// and Vulkan/SPIR-V. textureSampleLevel (explicit LOD 0) is used instead
// of textureSample so no implicit derivatives are required and the call is
// legal regardless of control-flow uniformity; the present source has no
// mips, so LOD 0 is plain bilinear.

fn fxaa_luma(rgb: vec3<f32>) -> f32 {
    return dot(rgb, vec3<f32>(0.299, 0.587, 0.114));
}

fn apply_fxaa(uv: vec2<f32>) -> vec3<f32> {
    let rcp = vec2<f32>(1.0, 1.0) / max(vec2<f32>(textureDimensions(t)), vec2<f32>(1.0, 1.0));
    let rgb_nw = textureSampleLevel(t, s, uv + vec2<f32>(-1.0, -1.0) * rcp, 0.0).rgb;
    let rgb_ne = textureSampleLevel(t, s, uv + vec2<f32>(1.0, -1.0) * rcp, 0.0).rgb;
    let rgb_sw = textureSampleLevel(t, s, uv + vec2<f32>(-1.0, 1.0) * rcp, 0.0).rgb;
    let rgb_se = textureSampleLevel(t, s, uv + vec2<f32>(1.0, 1.0) * rcp, 0.0).rgb;
    let rgb_m = textureSampleLevel(t, s, uv, 0.0).rgb;
    let luma_nw = fxaa_luma(rgb_nw);
    let luma_ne = fxaa_luma(rgb_ne);
    let luma_sw = fxaa_luma(rgb_sw);
    let luma_se = fxaa_luma(rgb_se);
    let luma_m = fxaa_luma(rgb_m);
    let luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    let luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    var dir: vec2<f32>;
    dir.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
    dir.y = ((luma_nw + luma_sw) - (luma_ne + luma_se));
    let dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.03125, 0.0078125);
    let rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2<f32>(-8.0, -8.0), vec2<f32>(8.0, 8.0)) * rcp;

    let rgb_a = 0.5 * (
        textureSampleLevel(t, s, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
        textureSampleLevel(t, s, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    let rgb_b = rgb_a * 0.5 + 0.25 * (
        textureSampleLevel(t, s, uv + dir * -0.5, 0.0).rgb +
        textureSampleLevel(t, s, uv + dir * 0.5, 0.0).rgb);
    let luma_b = fxaa_luma(rgb_b);
    if (luma_b < luma_min || luma_b > luma_max) {
        return rgb_a;
    }
    return rgb_b;
}

// END FXAA REGION

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // The pixel center's place in the viewport, from the fragment position
    // rather than the interpolated in.uv: identical arithmetic whether the
    // viewport starts at the target's corner or at a letterbox offset.
    let uv = (in.position.xy - uniforms.frame_origin) /
             max(vec2<f32>(uniforms.frame_width, uniforms.frame_height), vec2<f32>(1.0, 1.0));
    if (uniforms.post_aa_mode == 1u) {
        return vec4<f32>(apply_fxaa(uv), 1.0);
    }
    var color = textureSample(t, s, uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}
)"sv;

wgpu::PresentMode best_present_mode(bool vsync) {
  const auto supports = [](const wgpu::PresentMode candidate) {
    for (size_t i = 0; i < g_surfaceCapabilities.presentModeCount; ++i) {
      if (g_surfaceCapabilities.presentModes[i] == candidate) {
        return true;
      }
    }
    return false;
  };
  if (vsync) {
    if (supports(wgpu::PresentMode::FifoRelaxed)) {
      return wgpu::PresentMode::FifoRelaxed;
    }
  } else {
    if (supports(wgpu::PresentMode::Immediate)) {
      return wgpu::PresentMode::Immediate;
    }
    // Fall back only when the surface cannot present immediately.
    if (g_backendType != wgpu::BackendType::Metal && supports(wgpu::PresentMode::Mailbox)) {
      return wgpu::PresentMode::Mailbox;
    }
  }
  return wgpu::PresentMode::Fifo;
}

wgpu::TextureFormat to_linear(wgpu::TextureFormat format) {
  if (format == wgpu::TextureFormat::RGBA8UnormSrgb) {
    return wgpu::TextureFormat::RGBA8Unorm;
  }
  if (format == wgpu::TextureFormat::BGRA8UnormSrgb) {
    return wgpu::TextureFormat::BGRA8Unorm;
  }
  return format;
}

wgpu::TextureFormat best_surface_format() {
  if (g_surfaceCapabilities.formatCount == 0) {
    return wgpu::TextureFormat::Undefined;
  }
  for (size_t i = 0; i < g_surfaceCapabilities.formatCount; ++i) {
    const auto format = to_linear(g_surfaceCapabilities.formats[i]);
    if (format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::BGRA8Unorm) {
      return format;
    }
  }
  return g_surfaceCapabilities.formats[0];
}

uint32_t sampler_mode(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
    return 1;
  case SAMPLER_BILINEAR:
  default:
    return 0;
  }
}

// Anti-Aliasing P2 (aurora-patches/0017): maps the public post-AA enum to the
// shader's post_aa_mode uniform value (see resampleShaderSource fs_main).
uint32_t post_aa_mode(AuroraPostAA mode) noexcept {
  switch (mode) {
  case AURORA_POST_AA_FXAA:
    return 1;
  case AURORA_POST_AA_NONE:
  default:
    return 0;
  }
}

uint32_t viewport_extent(float value) noexcept {
  return std::max(1u, static_cast<uint32_t>(std::lround(std::max(value, 1.f))));
}

#ifndef __ANDROID__
// Anti-Aliasing P3 (aurora-patches/0018): scale the content framebuffer by the
// SSAA factor, clamped to the device's max 2D texture dimension so an
// aggressive factor on a high-res window can never request a texture the device
// rejects. Factor <= 1 leaves both extents unchanged (exact native -> the
// SSAA-off path is byte-identical). Desktop-only (compiled out under
// __ANDROID__).
//
// BOTH axes share ONE effective scale. Clamping each dimension independently
// (as this did originally) silently changes the framebuffer's aspect ratio the
// moment either axis hits the cap: 5120x2880 at 2x with an 8192 limit became
// 8192x5760 -- 1.42:1 instead of the window's 16:9, i.e. a stretched image.
// Taking the tightest of the requested factor and what each axis can afford,
// then applying that one number to both, keeps the content framebuffer exactly
// the window's shape at every resolution and every limit.
void apply_ssaa_scale(uint32_t width, uint32_t height, uint32_t& outWidth, uint32_t& outHeight) noexcept {
  outWidth = width;
  outHeight = height;
  if (g_ssaaFactor <= 1.f || width == 0 || height == 0) {
    return;
  }
  double scale = static_cast<double>(g_ssaaFactor);
  if (g_maxTextureDimension2D != 0) {
    const auto limit = static_cast<double>(g_maxTextureDimension2D);
    scale = std::min(scale, limit / static_cast<double>(width));
    scale = std::min(scale, limit / static_cast<double>(height));
  }
  if (scale <= 1.0) {
    return; // the window is already at/over the limit: native, undistorted
  }
  outWidth = static_cast<uint32_t>(std::max<double>(1.0, std::lround(static_cast<double>(width) * scale)));
  outHeight = static_cast<uint32_t>(std::max<double>(1.0, std::lround(static_cast<double>(height) * scale)));
  if (g_maxTextureDimension2D != 0) {
    // Rounding can land one past the cap on the axis that set the scale.
    outWidth = std::min(outWidth, g_maxTextureDimension2D);
    outHeight = std::min(outHeight, g_maxTextureDimension2D);
  }
}
#endif

} // namespace

TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  uint32_t sampleCount = 1;
  if (multisampled) {
    sampleCount = g_graphicsConfig.msaaSamples;
  }
  if (width == 0 || height == 0) {
    Log.fatal("Invalid render texture size! {}x{}, multisampled {}, format {}", width, height,
              static_cast<uint32_t>(format), multisampled);
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Render texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = sampleCount,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Render texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Render sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

const TextureWithSampler& present_source() noexcept {
  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
}

void set_resampler(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
  case SAMPLER_BILINEAR:
    g_Resampler = sampler;
    return;
  default:
    g_Resampler = SAMPLER_BILINEAR;
    return;
  }
}

AuroraSampler get_resampler() noexcept { return g_Resampler; }

// Anti-Aliasing P2 (aurora-patches/0017): clones set_resampler exactly -- just
// stores the mode; the value is read back into the resample uniform each
// present (resample_present_source), so the change is live with no pipeline
// rebuild and no restart.
void set_post_aa(AuroraPostAA mode) noexcept {
  switch (mode) {
  case AURORA_POST_AA_NONE:
  case AURORA_POST_AA_FXAA:
    g_PostAa.store(mode, std::memory_order_relaxed);
    return;
  default:
    g_PostAa.store(AURORA_POST_AA_NONE, std::memory_order_relaxed);
    return;
  }
}

AuroraPostAA get_post_aa() noexcept { return g_PostAa.load(std::memory_order_relaxed); }

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept {
  if (surface_width == 0 || surface_height == 0 || content_width == 0 || content_height == 0) {
    return {};
  }

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = std::min<uint32_t>(
      surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) *
                                                                               static_cast<double>(content_height) /
                                                                               static_cast<double>(content_width)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(
        surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) *
                                                                                static_cast<double>(content_width) /
                                                                                static_cast<double>(content_height)))));
  }

  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

static TextureWithSampler create_depth_texture(uint32_t width, uint32_t height) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Depth texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = g_graphicsConfig.msaaSamples,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Depth texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Depth sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

void create_copy_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = R"""(
@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

@fragment
fn fs_opaque(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(color.rgb, 1.0);
}

@fragment
fn fs_premultiplied_alpha(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(efb_texture, efb_sampler, in.uv);
}
)""";
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "XFB Copy Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_CopyBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_CopyBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);

  const auto make_copy_pipeline = [&](const char* label, const char* fragmentEntryPoint,
                                      const wgpu::BlendState* blend) {
    const std::array colorTargets{wgpu::ColorTargetState{
        .format = g_graphicsConfig.surfaceConfiguration.format,
        .blend = blend,
        .writeMask = wgpu::ColorWriteMask::All,
    }};
    const wgpu::FragmentState fragmentState{
        .module = module,
        .entryPoint = fragmentEntryPoint,
        .targetCount = colorTargets.size(),
        .targets = colorTargets.data(),
    };
    const wgpu::RenderPipelineDescriptor pipelineDescriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex =
            wgpu::VertexState{
                .module = module,
                .entryPoint = "vs_main",
            },
        .primitive =
            wgpu::PrimitiveState{
                .topology = wgpu::PrimitiveTopology::TriangleList,
            },
        .multisample =
            wgpu::MultisampleState{
                .count = 1,
                .mask = UINT32_MAX,
            },
        .fragment = &fragmentState,
    };
    return g_device.CreateRenderPipeline(&pipelineDescriptor);
  };
  g_CopyPipeline = make_copy_pipeline("XFB Copy Pipeline", "fs_opaque", nullptr);

  const wgpu::BlendState premultipliedAlphaBlend{
      .color =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
      .alpha =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
  };
  g_CopyPremultipliedAlphaPipeline =
      make_copy_pipeline("XFB Premultiplied Alpha Copy Pipeline", "fs_premultiplied_alpha", &premultipliedAlphaBlend);
}

void create_resample_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = resampleShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "Present Resample Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .writeMask = wgpu::ColorWriteMask::All,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_ResampleBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_ResampleBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = "Present Resample Pipeline",
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .multisample =
          wgpu::MultisampleState{
              .count = 1,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  g_ResamplePipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);

  const wgpu::BufferDescriptor uniformBufferDescriptor{
      .label = "Present Resample Uniform Buffer",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
      .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
  };
  g_ResampleUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  g_hasResampleUniform = false;
}

wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source) {
  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .textureView = source.view,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_CopyBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  return gfx::cached_bind_group(bindGroupDescriptor);
}

// intoSurface: drawn straight into the surface at the viewport's offset,
// rather than into an intermediate texture of the viewport's size.
static wgpu::BindGroup resample_present_bind_group(const Viewport& viewport, bool intoSurface) {
  const auto& source = present_source();
  const uint32_t width = viewport_extent(viewport.width);
  const uint32_t height = viewport_extent(viewport.height);

  const ResampleUniformBlock uniform{
      .samplerMode = sampler_mode(g_Resampler),
      .frameWidth = static_cast<float>(width),
      .frameHeight = static_cast<float>(height),
      .postAaMode = post_aa_mode(g_PostAa.load(std::memory_order_relaxed)),
      .frameLeft = intoSurface ? viewport.left : 0.f,
      .frameTop = intoSurface ? viewport.top : 0.f,
  };
  ASSERT(gfx::render_worker::is_worker_thread(), "Present resample queue write must run on the render worker");
  if (!g_hasResampleUniform || uniform.samplerMode != g_lastResampleUniform.samplerMode ||
      uniform.frameWidth != g_lastResampleUniform.frameWidth ||
      uniform.frameHeight != g_lastResampleUniform.frameHeight ||
      uniform.postAaMode != g_lastResampleUniform.postAaMode || uniform.frameLeft != g_lastResampleUniform.frameLeft ||
      uniform.frameTop != g_lastResampleUniform.frameTop) {
    g_queue.WriteBuffer(g_ResampleUniformBuffer, 0, &uniform, sizeof(uniform));
    g_lastResampleUniform = uniform;
    g_hasResampleUniform = true;
  }

  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = g_ResampleUniformBuffer,
          .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 2,
          .textureView = source.view,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_ResampleBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  return gfx::cached_bind_group(bindGroupDescriptor);
}

bool can_resample_present_direct(const Viewport& viewport) {
  // Exact same pixel centers and format as the intermediate+copy path. A
  // viewport filling the surface always qualifies. A whole-pixel letterbox
  // offset (the usual case: the content width is a multiple of 16) qualifies
  // when the source is not rescaled: the shader samples in viewport-relative
  // UVs (the area sampler subtracts frame_origin), so the pixels are the same
  // bit for bit. A rescaled source (SSAA) can round differently at an offset,
  // so it keeps the original path, as do fractional viewports.
  const auto& surface = g_graphicsConfig.surfaceConfiguration;
  const auto& source = present_source();
  if (source.format != surface.format) {
    return false;
  }
  if (viewport.left == 0.f && viewport.top == 0.f && viewport.width == float(surface.width) &&
      viewport.height == float(surface.height)) {
    return true;
  }
  const auto integral = [](float v) { return v >= 0.f && v == std::floor(v); };
  return integral(viewport.left) && integral(viewport.top) && integral(viewport.width) &&
         integral(viewport.height) && viewport.left + viewport.width <= float(surface.width) &&
         viewport.top + viewport.height <= float(surface.height) && viewport.width == float(source.size.width) &&
         viewport.height == float(source.size.height);
}

void draw_resampled_present(const wgpu::RenderPassEncoder& pass, const Viewport& viewport) {
  const auto bindings = resample_present_bind_group(viewport, true);
  pass.SetPipeline(g_ResamplePipeline);
  pass.SetBindGroup(0, bindings, 0, nullptr);
  pass.Draw(3);
}

const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport) {
  const auto& source = present_source();
  const uint32_t width = viewport_extent(viewport.width);
  const uint32_t height = viewport_extent(viewport.height);
  if (!g_resampledFrameBuffer.view || g_resampledFrameBuffer.size.width != width ||
      g_resampledFrameBuffer.size.height != height || g_resampledFrameBuffer.format != source.format) {
    g_resampledFrameBuffer = create_render_texture(width, height, false);
  }
  const auto bindGroup = resample_present_bind_group(viewport, false);
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = g_resampledFrameBuffer.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "Present resample render pass",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
      .timestampWrites = gpu_prof::pass_writes("Present resample"),
  };
  const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
  pass.SetPipeline(g_ResamplePipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetViewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f);
  pass.Draw(3);
  pass.End();

  return g_resampledFrameBuffer;
}

static wgpu::BackendType to_wgpu_backend(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_WEBGPU:
    return wgpu::BackendType::WebGPU;
  case BACKEND_D3D11:
    return wgpu::BackendType::D3D11;
  case BACKEND_D3D12:
    return wgpu::BackendType::D3D12;
  case BACKEND_METAL:
    return wgpu::BackendType::Metal;
  case BACKEND_VULKAN:
    return wgpu::BackendType::Vulkan;
  case BACKEND_OPENGL:
    return wgpu::BackendType::OpenGL;
  case BACKEND_OPENGLES:
    return wgpu::BackendType::OpenGLES;
  default:
    return wgpu::BackendType::Null;
  }
}

static void release_surface_locked() noexcept {
  if (g_surface) {
    g_surface.Unconfigure();
  }
  g_surface = {};
}

static bool create_surface() {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    Log.error("Failed to create surface: no window");
    return false;
  }
  window::SurfaceLock surfaceLock;
  const auto chainedDescriptor = utils::SetupWindowAndGetSurfaceDescriptor(window);
  if (!chainedDescriptor) {
    Log.error("Failed to create surface descriptor for current window");
    return false;
  }
  const wgpu::SurfaceDescriptor surfaceDescriptor{
      .nextInChain = chainedDescriptor.get(),
      .label = "Surface",
  };
  release_surface_locked();
  g_surface = g_instance.CreateSurface(&surfaceDescriptor);
  if (!g_surface) {
    Log.error("Failed to create surface");
    return false;
  }
  return true;
}

static bool request_adapter(const wgpu::RequestAdapterOptions& options) {
  bool requestAdapterCallbackCompleted = false;
  wgpu::RequestAdapterStatus requestAdapterStatus = wgpu::RequestAdapterStatus::CallbackCancelled;
  std::string requestAdapterMessage;
  const auto future = g_instance.RequestAdapter(
      &options, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
        requestAdapterCallbackCompleted = true;
        requestAdapterStatus = status;
        requestAdapterMessage = std::string{std::string_view{message}};
        if (status == wgpu::RequestAdapterStatus::Success) {
          g_adapter = std::move(adapter);
        } else {
          Log.warn("Adapter request failed: {}: {}", magic_enum::enum_name(status), message);
        }
      });
  const auto status = g_instance.WaitAny(future, 5000000000);
  if (status != wgpu::WaitStatus::Success) {
    if (requestAdapterCallbackCompleted) {
      Log.error("Failed to create adapter: wait status {}, request status {}, message: {}",
                magic_enum::enum_name(status), magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
    } else {
      Log.error("Failed to create adapter: wait status {}, request callback did not complete",
                magic_enum::enum_name(status));
    }
    return false;
  }
  if (!g_adapter) {
    if (requestAdapterCallbackCompleted) {
      Log.error("Failed to create adapter: request status {}, message: {}",
                magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
    } else {
      Log.error("Failed to create adapter: request callback did not complete");
    }
    return false;
  }
  return true;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
// dawn::native::d3d::RequestAdapterOptionsLUID, laid out through the C API
// (its C++ constructor is not usable from this ABI).
struct D3DAdapterLuid {
  wgpu::ChainedStruct chain;
  LUID adapterLUID;
};

// DXGI's own choice for GPU-heavy work: the adapter Dawn would pick for
// PowerPreference::HighPerformance among hardware adapters.
static bool high_performance_adapter_luid(LUID& out) {
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
    return false;
  }
  DXGI_ADAPTER_DESC1 desc{};
  if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
    return false;
  }
  out = desc.AdapterLuid;
  return true;
}
#endif

bool initialize(AuroraBackend auroraBackend, bool allowCpu) {
  if (!g_instance) {
    Log.info("Creating WebGPU instance");
    const std::array requiredInstanceFeatures{
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor instanceDescriptor{
        .requiredFeatureCount = requiredInstanceFeatures.size(),
        .requiredFeatures = requiredInstanceFeatures.data(),
    };
#if defined(WEBGPU_DAWN) && !defined(__MINGW32__) && !defined(__EMSCRIPTEN__)
    // dawn::native::DawnInstanceDescriptor is Dawn's C++ "native" extension API
    // (dawn/native/DawnNative.h), compiled into the prebuilt webgpu_dawn.dll/lib
    // with the Microsoft C++ ABI. A MinGW-ABI compiler (incl. zig cc's
    // x86_64-windows-gnu target) mangles/lays out these C++ symbols differently
    // and cannot link against them -- confirmed via direct build attempt
    // (lld-link: undefined symbol dawn::native::DawnInstanceDescriptor::
    // DawnInstanceDescriptor()). Skipping this block only loses Dawn's default
    // backend-validation level override and Aurora's custom Dawn-internal
    // logging callback hookup; wgpu::CreateInstance and the rest of the WebGPU
    // C API below are extern "C" and link fine regardless of ABI.
    dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor;
    dawnInstanceDescriptor.backendValidationLevel = dawn::native::BackendValidationLevel::Disabled;
    dawnInstanceDescriptor.SetLoggingCallback(wgpu_log);
#ifdef TRACY_ENABLE
    dawnInstanceDescriptor.platform = tracy_dawn_platform();
#endif
    instanceDescriptor.nextInChain = &dawnInstanceDescriptor;
#endif
    g_instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!g_instance) {
      Log.error("Failed to create WebGPU instance");
      return false;
    }
  }
  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);
  Log.info("Attempting to initialize {}", magic_enum::enum_name(backend));
#if 0
  // D3D12's debug layer is very slow
  g_dawnInstance->EnableBackendValidation(backend != WGPUBackendType::D3D12);
#endif

  if (!create_surface()) {
    return false;
  }
  {
    const wgpu::RequestAdapterOptions options{
        .featureLevel = wgpu::FeatureLevel::Compatibility,
        .powerPreference = wgpu::PowerPreference::HighPerformance,
#ifdef __EMSCRIPTEN__
        .backendType = wgpu::BackendType::Undefined, // the browser picks
#else
        .backendType = backend,
#endif
        .compatibleSurface = g_surface,
    };
    Log.info("Requesting adapter\n  Feature level: {}\n  Power preference: {}\n  Backend: {}\n  Compatible surface: {}",
             magic_enum::enum_name(options.featureLevel), magic_enum::enum_name(options.powerPreference),
             magic_enum::enum_name(options.backendType), static_cast<bool>(options.compatibleSurface));
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Dawn's D3D12 discovery opens a device on every DXGI adapter to read its
    // limits, which costs ~100 ms each (0.7-0.9 s on a PC that lists eight).
    // Naming DXGI's high-performance adapter makes it open only that one.
    D3DAdapterLuid luid{};
    wgpu::RequestAdapterOptions targeted = options;
    if (backend == wgpu::BackendType::D3D12 && high_performance_adapter_luid(luid.adapterLUID)) {
      luid.chain.sType = static_cast<wgpu::SType>(WGPUSType_RequestAdapterOptionsLUID);
      targeted.nextInChain = &luid.chain;
    }
    if (targeted.nextInChain != nullptr && request_adapter(targeted)) {
      Log.info("Requested the high-performance D3D12 adapter by LUID");
    } else
#endif
    if (!request_adapter(options)) {
      return false;
    }
  }
  g_adapter.GetInfo(&g_adapterInfo);
  auto adapterName = g_adapterInfo.device;
  if (adapterName.IsUndefined()) {
    adapterName = wgpu::StringView("Unknown");
  }
  if (!allowCpu && g_adapterInfo.adapterType == wgpu::AdapterType::CPU && backend != wgpu::BackendType::Null) {
    Log.warn("Ignoring CPU adapter: {}", adapterName);
    g_adapterInfo = {};
    g_adapter = {};
    return false;
  }
  auto description = g_adapterInfo.description;
  if (description.IsUndefined()) {
    description = wgpu::StringView("Unknown");
  }
  g_backendType = g_adapterInfo.backendType;
  const auto backendName = magic_enum::enum_name(g_backendType);
  Log.info("Graphics adapter information\n  API: {}\n  Device: {} ({})\n  Driver: {}", backendName, adapterName,
           magic_enum::enum_name(g_adapterInfo.adapterType), description);

  {
    wgpu::Limits supportedLimits{};
    const wgpu::Status supportedLimitsStatus = g_adapter.GetLimits(&supportedLimits);
    if (supportedLimitsStatus != wgpu::Status::Success) {
      Log.error("Failed to query adapter limits: {}", magic_enum::enum_name(supportedLimitsStatus));
      return false;
    }
    // Anti-Aliasing P3 (aurora-patches/0018): remember the device's max 2D
    // texture dimension so SSAA can clamp the scaled content framebuffer.
    g_maxTextureDimension2D = supportedLimits.maxTextureDimension2D;
    // XeGTAO's 16x16 prefilter writes five storage images in one shader
    // stage. Request exactly that limit only when the adapter reports a
    // meaningful value; otherwise leave it undefined so the renderer can
    // select its fallback on devices that cannot expose all five images.
    constexpr uint32_t kXeGTAOPrefilterStorageTextures = 5;
    const uint32_t supportedStorageTextures = supportedLimits.maxStorageTexturesPerShaderStage;
    const uint32_t requestedStorageTextures =
        supportedStorageTextures != WGPU_LIMIT_U32_UNDEFINED && supportedStorageTextures != 0 &&
                supportedStorageTextures >= kXeGTAOPrefilterStorageTextures
            ? kXeGTAOPrefilterStorageTextures
            : WGPU_LIMIT_U32_UNDEFINED;
    wgpu::CompatibilityModeLimits compatibilityModeLimits{wgpu::CompatibilityModeLimits::Init{
        .maxStorageBuffersInVertexStage = 2,
        .maxStorageBuffersInFragmentStage = 2,
    }};
    const wgpu::Limits requiredLimits{
        .nextInChain = &compatibilityModeLimits,
        // Use "best" supported limits
        .maxTextureDimension1D = supportedLimits.maxTextureDimension1D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension1D,
        .maxTextureDimension2D = supportedLimits.maxTextureDimension2D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension2D,
        .maxTextureDimension3D = supportedLimits.maxTextureDimension3D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension3D,
        .maxTextureArrayLayers = supportedLimits.maxTextureArrayLayers == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureArrayLayers,
        .maxStorageBuffersPerShaderStage = 2,
        .maxStorageTexturesPerShaderStage = requestedStorageTextures,
        .minUniformBufferOffsetAlignment =
            supportedLimits.minUniformBufferOffsetAlignment < 64 ? 64 : supportedLimits.minUniformBufferOffsetAlignment,
        .minStorageBufferOffsetAlignment =
            supportedLimits.minStorageBufferOffsetAlignment < 16 ? 16 : supportedLimits.minStorageBufferOffsetAlignment,
    };
    Log.info(
        "Using limits:"
        "\n  maxTextureDimension1D: {}"
        "\n  maxTextureDimension2D: {}"
        "\n  maxTextureDimension3D: {}"
        "\n  maxTextureArrayLayers: {}"
        "\n  maxStorageBuffersPerShaderStage: {}"
        "\n  maxStorageTexturesPerShaderStage: {} (supported: {}, requested: {})"
        "\n  minUniformBufferOffsetAlignment: {}"
        "\n  minStorageBufferOffsetAlignment: {}",
        requiredLimits.maxTextureDimension1D, requiredLimits.maxTextureDimension2D,
        requiredLimits.maxTextureDimension3D, requiredLimits.maxTextureArrayLayers,
        requiredLimits.maxStorageBuffersPerShaderStage, requiredLimits.maxStorageTexturesPerShaderStage,
        supportedStorageTextures, requestedStorageTextures, requiredLimits.minUniformBufferOffsetAlignment,
        requiredLimits.minStorageBufferOffsetAlignment);
    std::vector<wgpu::FeatureName> requiredFeatures;
    g_hasCoreFeatures = false;
    g_bcTexturesSupported = false;
    g_astcTexturesSupported = false;
    g_textureComponentSwizzleSupported = false;
    wgpu::SupportedFeatures supportedFeatures;
    g_adapter.GetFeatures(&supportedFeatures);
    for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
      const auto feature = supportedFeatures.features[i];
      if (feature == wgpu::FeatureName::CoreFeaturesAndLimits || feature == wgpu::FeatureName::TextureCompressionBC ||
          feature == wgpu::FeatureName::TextureCompressionASTC ||
          feature == wgpu::FeatureName::TextureComponentSwizzle) {
        if (feature == wgpu::FeatureName::CoreFeaturesAndLimits) {
          g_hasCoreFeatures = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionBC) {
          g_bcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionASTC) {
          g_astcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureComponentSwizzle) {
          g_textureComponentSwizzleSupported = true;
        }
        requiredFeatures.push_back(feature);
      }
      if (feature == wgpu::FeatureName::TimestampQuery) {
        requiredFeatures.push_back(feature);
      }
    }
#ifdef WEBGPU_DAWN
    // This is a storage-format experiment, not FP16 AO arithmetic. Request
    // the optional feature before device creation, never from the draw path.
    const char* aoImplementation = std::getenv("MP6_AO_XEGTAO");
    const char* aoDepthStorage = std::getenv("MP6_AO_XEGTAO_DEPTH_STORAGE");
    if (aoImplementation && std::string_view{aoImplementation} == "1" &&
        aoDepthStorage && std::string_view{aoDepthStorage} == "r16float") {
      if (g_adapter.HasFeature(wgpu::FeatureName::TextureFormatsTier1)) {
        requiredFeatures.push_back(wgpu::FeatureName::TextureFormatsTier1);
        Log.info("XeGTAO R16 storage experiment: enabling TextureFormatsTier1");
      } else {
        Log.warn("XeGTAO R16 storage experiment: TextureFormatsTier1 unsupported; keeping R32");
      }
    }
#endif

    std::string featureList;
    for (auto featureName : requiredFeatures) {
      featureList += "\n  ";
      featureList += magic_enum::enum_name(featureName);
    }
    Log.info("Enabling features: {}", featureList);
#if defined(WEBGPU_DAWN) && !defined(__EMSCRIPTEN__)
    wgpu::DawnCacheDeviceDescriptor cacheDescriptor({
        .isolationKey = nullptr,
        .loadDataFunction = load_from_cache,
        .storeDataFunction = store_to_cache,
        .functionUserdata = nullptr,
    });

    std::vector<const char*> enableToggles{
#if _WIN32
        "use_dxc",
#ifndef NDEBUG
        "emit_hlsl_debug_symbols",
#endif
#endif
#ifdef NDEBUG
        "skip_validation",
        "disable_robustness",
#endif
#ifndef ANDROID
        "use_user_defined_labels_in_backend",
#endif
        "allow_unsafe_apis",
        "disable_symbol_renaming",
        "enable_immediate_error_handling",
        "gl_allow_context_on_multi_threads",
    };
    // Allow release QA to report invalid GPU commands before an unchecked
    // command can turn into a driver/device failure. Normal releases retain
    // their existing toggle set and performance.
    const char* validation = std::getenv("MP6_GPU_VALIDATION");
    if (validation && std::string_view{validation} == "1") {
      std::erase_if(enableToggles, [](const char* toggle) {
        const std::string_view name{toggle};
        return name == "skip_validation" || name == "disable_robustness";
      });
      Log.info("GPU validation and robustness enabled for diagnostic run");
    }
    constexpr std::array disableToggles{
        "timestamp_quantization",
    };
    wgpu::DawnTogglesDescriptor togglesDescriptor(wgpu::DawnTogglesDescriptor::Init{
        .nextInChain = &cacheDescriptor,
        .enabledToggleCount = enableToggles.size(),
        .enabledToggles = enableToggles.data(),
        .disabledToggleCount = disableToggles.size(),
        .disabledToggles = disableToggles.data(),
    });
#endif
    wgpu::DeviceDescriptor deviceDescriptor({
#if defined(WEBGPU_DAWN) && !defined(__EMSCRIPTEN__)
        .nextInChain = &togglesDescriptor,
#endif
        .requiredFeatureCount = requiredFeatures.size(),
        .requiredFeatures = requiredFeatures.data(),
        .requiredLimits = &requiredLimits,
    });
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
#ifdef __EMSCRIPTEN__
          // Web: an invalid command buffer is dropped by the browser and
          // costs one frame; the device stays usable. Safari reports
          // "encoder state is not valid" for an RmlUi-layer frame now and
          // then (cause still unknown), and aborting there ended the game.
          // Keep running and let the page report it.
          if (g_initialized && type == wgpu::ErrorType::Validation) {
            static uint32_t s_webValidationErrors = 0;
            if (++s_webValidationErrors <= 20) {
              Log.error("WebGPU error {} (frame skipped, #{}): {}", underlying(type), s_webValidationErrors, message);
            }
            return;
          }
#endif
          if (g_initialized) {
            FATAL("WebGPU error {}: {}", underlying(type), message);
          } else {
            Log.warn("WebGPU error {}: {}", underlying(type), message);
          }
        });
    deviceDescriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device& device, wgpu::DeviceLostReason reason, wgpu::StringView message) {
          if (g_initialized) {
            FATAL("Device lost: {}", message);
          } else {
            Log.warn("Device lost: {}", message);
          }
        });
    const auto future =
        g_adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                  if (status == wgpu::RequestDeviceStatus::Success) {
                                    g_device = std::move(device);
                                  } else {
                                    Log.warn("Device request failed: {}", message);
                                  }
                                });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      Log.error("Failed to create device: {}", magic_enum::enum_name(status));
      return false;
    }
    if (!g_device) {
      return false;
    }
#ifndef __EMSCRIPTEN__
    g_device.SetLoggingCallback(wgpu_log);
#endif
    wgpu::Limits enabledLimits{};
    const wgpu::Status enabledLimitsStatus = g_device.GetLimits(&enabledLimits);
    if (enabledLimitsStatus == wgpu::Status::Success) {
      Log.info("Enabled maxStorageTexturesPerShaderStage: {}", enabledLimits.maxStorageTexturesPerShaderStage);
    } else {
      Log.warn("Failed to query enabled device limits: {}", magic_enum::enum_name(enabledLimitsStatus));
    }
  }
  g_queue = g_device.GetQueue();

  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);
  if (status != wgpu::Status::Success) {
    Log.error("Failed to get surface capabilities: {}", magic_enum::enum_name(status));
    return false;
  }
  if (g_surfaceCapabilities.formatCount == 0) {
    Log.error("Surface has no formats");
    return false;
  }
  if (g_surfaceCapabilities.presentModeCount == 0) {
    Log.error("Surface has no present modes");
    return false;
  }
  std::string supportedPresentModes;
  for (size_t i = 0; i < g_surfaceCapabilities.presentModeCount; ++i) {
    if (!supportedPresentModes.empty()) {
      supportedPresentModes += ", ";
    }
    supportedPresentModes += magic_enum::enum_name(g_surfaceCapabilities.presentModes[i]);
  }
  Log.info("Surface capabilities: supported-present-modes=[{}]", supportedPresentModes);
  auto surfaceFormat = best_surface_format();
  auto presentMode = best_present_mode(g_config.vsync);
  Log.info("Using surface format {}, present mode {}", magic_enum::enum_name(surfaceFormat),
           magic_enum::enum_name(presentMode));
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .surfaceConfiguration =
          wgpu::SurfaceConfiguration{
              .format = surfaceFormat,
              .usage = wgpu::TextureUsage::RenderAttachment,
              .width = size.native_fb_width,
              .height = size.native_fb_height,
              .presentMode = presentMode,
          },
      .depthFormat = wgpu::TextureFormat::Depth32Float,
      .msaaSamples = g_config.msaa,
      .textureAnisotropy = g_config.maxTextureAnisotropy,
  };
#ifndef __ANDROID__
  // Anti-Aliasing P3 (aurora-patches/0018): latch the SSAA factor before the
  // content framebuffer is built below. Desktop-only; <=1 or non-finite means
  // off/native (byte-identical). resize_swapchain scales the content targets
  // by this while the surface stays native.
  g_ssaaFactor = (std::isfinite(g_config.ssaa) && g_config.ssaa > 1.f) ? g_config.ssaa : 1.f;
#endif
  create_copy_pipeline();
  create_resample_pipeline();
  gpu_prof::initialize();
  resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  g_initialized = true;
  return true;
}

void shutdown() {
  g_initialized = false;
  gfx::gpu_synchronize();
  gpu_prof::shutdown();
  g_CopyBindGroupLayout = {};
  g_CopyPipeline = {};
  g_CopyPremultipliedAlphaPipeline = {};
  g_CopyBindGroup = {};
  g_ResampleBindGroupLayout = {};
  g_ResamplePipeline = {};
  g_ResampleUniformBuffer = {};
  g_hasResampleUniform = false;
  g_resampledFrameBuffer = {};
  g_frameBuffer = {};
  g_frameBufferResolved = {};
  g_depthBuffer = {};
  g_queue = {};
  g_surface = {};
  g_device = {};
  g_adapter = {};
  g_instance = {};
  cache_shutdown();
}

void release_surface() noexcept {
  gfx::gpu_synchronize();
  {
    window::SurfaceLock surfaceLock;
    release_surface_locked();
  }
}

// [MP6] Render scale (compat/aurora/windows/0007): a factor below 1 renders
// the content framebuffer at a fraction of the logical size and lets the
// present-resample pass upscale it, the mirror image of SSAA. 1 = off
// (byte-identical). Applied on every platform; set live between frames.
static float g_renderScale = 1.f;

void set_render_scale(float scale) noexcept {
  g_renderScale = (std::isfinite(scale) && scale > 0.f) ? std::clamp(scale, 0.25f, 1.f) : 1.f;
}

float render_scale() noexcept { return g_renderScale; }

static void resize_swapchain_internal(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight,
                                      bool force) {
  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {
    return;
  }
  // Anti-Aliasing P3 (aurora-patches/0018): the content framebuffer is built at
  // SSAA factor x the logical size while the surface stays native -- the game
  // viewport/scissor track it automatically (get_render_target_size derives
  // from g_frameBuffer.size) and the present-resample pass downsamples for
  // free. Factor 1 (off) makes fbWidth/fbHeight == width/height exactly, so the
  // SSAA-off path is byte-identical. Desktop-only (compiled out on __ANDROID__).
  uint32_t fbWidth = width;
  uint32_t fbHeight = height;
#ifndef __ANDROID__
  apply_ssaa_scale(width, height, fbWidth, fbHeight);
#endif
  if (g_renderScale < 1.f) {
    fbWidth = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(width) * g_renderScale)));
    fbHeight = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(height) * g_renderScale)));
  }
  const bool sizeChanged = g_graphicsConfig.surfaceConfiguration.width != nativeWidth ||
                           g_graphicsConfig.surfaceConfiguration.height != nativeHeight ||
                           g_frameBuffer.size.width != fbWidth || g_frameBuffer.size.height != fbHeight;
  if (!force && !sizeChanged) {
    return;
  }
  if (sizeChanged) {
    gx::clear_copy_texture_cache();
    gfx::clear_caches();
  }
  g_graphicsConfig.surfaceConfiguration.width = nativeWidth;
  g_graphicsConfig.surfaceConfiguration.height = nativeHeight;
  auto surfaceConfiguration = g_graphicsConfig.surfaceConfiguration;
  surfaceConfiguration.device = g_device;
  {
    window::SurfaceLock surfaceLock;
    g_surface.Configure(&surfaceConfiguration);
  }
  g_frameBuffer = create_render_texture(fbWidth, fbHeight, true);
  // Without MSAA the EFB pass renders into g_frameBuffer itself, which is
  // also the resolved image (present_source); a resolve target would be a
  // second frame-sized texture nothing writes or reads.
  g_frameBufferResolved =
      g_graphicsConfig.msaaSamples > 1 ? create_render_texture(fbWidth, fbHeight, false) : TextureWithSampler{};
  g_depthBuffer = create_depth_texture(fbWidth, fbHeight);
  g_CopyBindGroup = create_copy_bind_group(present_source());
}

bool refresh_surface(bool recreate) {
  gfx::gpu_synchronize();
  if (!g_instance || !g_device) {
    return false;
  }
  if (!window::is_presentable()) {
    {
      window::SurfaceLock surfaceLock;
      release_surface_locked();
    }
    return false;
  }
  if ((!g_surface || recreate) && !create_surface()) {
    return false;
  }
  uint32_t width = g_graphicsConfig.surfaceConfiguration.width;
  uint32_t height = g_graphicsConfig.surfaceConfiguration.height;
  uint32_t nativeWidth = width;
  uint32_t nativeHeight = height;
  if (window::get_sdl_window() != nullptr) {
    const auto size = window::get_window_size();
    width = size.fb_width;
    height = size.fb_height;
    nativeWidth = size.native_fb_width;
    nativeHeight = size.native_fb_height;
  }
  if (width != 0 && height != 0) {
    resize_swapchain_internal(width, height, nativeWidth, nativeHeight, true);
  }
  return true;
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force) {
  gfx::gpu_synchronize();
  resize_swapchain_internal(width, height, nativeWidth, nativeHeight, force);
}
} // namespace aurora::webgpu

extern "C" void mp6_aurora_set_aa(unsigned samples, float ssaa, int fxaa) {
  using namespace aurora;
  using namespace aurora::webgpu;
  samples = samples == 4 ? 4 : 1;
  ssaa = ssaa == 1.5f || ssaa == 2.f ? ssaa : 1.f;
  if (samples > 1) ssaa = 1.f;
  if (samples > 1 || ssaa > 1.f) fxaa = 0;
  const auto post = fxaa ? AURORA_POST_AA_FXAA : AURORA_POST_AA_NONE;
  if (g_graphicsConfig.msaaSamples == samples && g_ssaaFactor == ssaa) {
    set_post_aa(post);
    return;
  }
  gfx::gpu_synchronize();
  g_graphicsConfig.msaaSamples = samples;
  g_ssaaFactor = ssaa;
  g_config.msaa = samples;
  g_config.ssaa = ssaa;
  gx::clear_copy_texture_cache();
  gfx::clear_caches();
  g_resampledFrameBuffer = {};
  set_post_aa(post);
  refresh_surface(false);
  aurora::webgpu::Log.info("Live AA: MSAA {}, SSAA {}, FXAA {}", samples, ssaa, fxaa);
}

#ifdef __EMSCRIPTEN__
// Web: there is no present mode; V-Sync off makes the frame yield skip the
// display wait (Module.mp6Uncapped), unless ?uncapped already forces it.
EM_JS(void, aurora_web_set_vsync, (int enabled), {
  if (!Module.mp6UncappedForced) Module.mp6Uncapped = !enabled;
});
void aurora_web_apply_vsync(bool enabled) { aurora_web_set_vsync(enabled ? 1 : 0); }
#endif

void aurora_enable_vsync(const bool enabled) {
#ifdef __EMSCRIPTEN__
  aurora_web_apply_vsync(enabled);
#endif
  aurora::webgpu::g_graphicsConfig.surfaceConfiguration.presentMode = aurora::webgpu::best_present_mode(enabled);
  aurora::window::push_custom_event(aurora::window::CustomEvent::RefreshSurface);
}
