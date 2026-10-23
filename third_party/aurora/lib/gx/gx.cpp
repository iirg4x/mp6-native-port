#include "gx.hpp"
#include "command_processor.hpp"

#include "pipeline.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../internal.hpp"
#include "../gfx/common.hpp"
#include "../gfx/tex_palette_conv.hpp"
#include "../gfx/texture.hpp"
#include "../gfx/texture_convert.hpp"
#include "../gfx/texture_replacement.hpp"
#include "gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <tracy/Tracy.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <utility>

static aurora::Module Log("aurora::gx");

namespace aurora::gx {
using webgpu::g_device;
using webgpu::g_graphicsConfig;

GXState g_gxState{};

static wgpu::Sampler sEmptySampler;
static wgpu::Texture sEmptyTexture;
static wgpu::TextureView sEmptyTextureView;
static std::mutex sBindGroupLayoutMutex;
static absl::flat_hash_map<u32, wgpu::BindGroupLayout> sUniformBindGroupLayouts;
static absl::flat_hash_map<u32, std::pair<wgpu::BindGroupLayout, wgpu::BindGroupLayout>> sTextureBindGroupLayouts;
static wgpu::BindGroupLayout sTextureBindGroupLayout;
static wgpu::BindGroupLayout sSamplerBindGroupLayout;
static wgpu::PipelineLayout sPipelineLayout;
wgpu::BindGroup g_emptyTextureBindGroup;

namespace {
struct DynamicPaletteKey {
  const void* sourceIdentity = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;

  bool operator==(const DynamicPaletteKey& rhs) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const DynamicPaletteKey& key) {
    return H::combine(std::move(h), key.sourceIdentity, key.width, key.height, key.format);
  }
};

struct DynamicPaletteEntry {
  gfx::TextureHandle handle;
  u32 sourceRevision = 0;
  u32 tlutDataVersion = 0;
};

struct CachedTextureEntry {
  gfx::TextureHandle handle;
  gfx::TextureDataKey sourceData;
  gfx::PaletteDataKey paletteData;
  u32 lastUsed = 0;    // gfx::current_frame() of the last draw that resolved it
  u64 contentKey = 0;  // s_textureContentCache key, 0 when not content-addressed
  bool written = false; // the game wrote its source since the conversion; recheck the bytes
};

// Converted textures by content. The game loads the same texture data again
// and again (a board's models and sprites after every event, minigame and
// menu), each time into a fresh heap block, so the texture object cache keyed
// on the source address misses. An identical reload reuses the conversion.
// Entries no texture object owns any more stay for MP6_TEXTURE_KEEP_SECONDS
// (default 10) so a reload right after the free still finds them.
struct ContentTextureEntry {
  gfx::TextureHandle handle;
  u32 unownedSince = 0; // content_clock() second it lost its last owner, 0 while owned
};

struct CachedTlutTextureEntry {
  gfx::TextureHandle handle;
  gfx::PaletteDataKey sourceData;
};

struct TlutObjectCache {
  CachedTlutTextureEntry tlutTexture;
  absl::flat_hash_map<DynamicPaletteKey, DynamicPaletteEntry> dynamicPaletteTextures;
  absl::flat_hash_set<u32> staticTextureUsers;
  u32 lastUsed = 0;
};

absl::flat_hash_map<u32, CachedTextureEntry> s_textureObjectCaches;
absl::flat_hash_map<u64, ContentTextureEntry> s_textureContentCache;
absl::flat_hash_map<u32, TlutObjectCache> s_tlutObjectCaches;
std::atomic_bool s_staticTextureCacheClearPending = false;

void do_clear_static_texture_cache() noexcept {
  s_textureObjectCaches.clear();
  s_textureContentCache.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.staticTextureUsers.clear();
  }
  // Retire live static bindings as well as secondary cache entries. Destroyed
  // objects may retain an uploaded image, but their CPU source is no longer
  // guaranteed to exist. Copy images are unrelated to replacement reloads.
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& obj = g_gxState.loadedTextures[i];
    const auto tlutIdx = static_cast<size_t>(obj.tlut);
    const bool retiredPalette = is_palette_format(obj.format()) && tlutIdx < g_gxState.loadedTluts.size() &&
                                g_gxState.loadedTluts[tlutIdx].no_cache();
    auto& bound = g_gxState.textures[i];
    if (!obj.no_cache() && !retiredPalette && !g_gxState.copyTextures.contains(bound.texObj.data)) {
      bound.texObj.texObjId = 0;
    }
  }
}

DynamicPaletteKey make_dynamic_palette_key(const GXTexObj_& obj, const GXState::CopyTextureRef& source) {
  return {
      .sourceIdentity = source.handle.get(),
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
  };
}

void clear_texture_dependency(u32 texObjId, u32 tlutObjId) {
  if (texObjId == 0 || tlutObjId == 0) {
    return;
  }
  if (auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    it->second.staticTextureUsers.erase(texObjId);
    if (!it->second.tlutTexture.handle && it->second.dynamicPaletteTextures.empty() &&
        it->second.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it);
    }
  }
}

// Hash of everything a conversion reads: the texels with all mips, and the
// palette. 0 for formats the GX size rules don't cover.
u64 texture_content_key(const GXTexObj_& obj, const GXTlutObj_* tlut) noexcept {
  const u32 format = obj.format();
  if (obj.data == nullptr || format > GX_TF_CMPR) {
    return 0;
  }
  const u32 mips = obj.mip_count();
  const u32 bytes = GXGetTexBufferSize(obj.width(), obj.height(), format, mips > 1, mips);
  const u64 shape = u64{format} | u64{obj.width()} << 8 | u64{obj.height()} << 24 | u64{mips} << 40;
  u64 key = XXH3_64bits_withSeed(obj.data, bytes, shape);
  if (tlut != nullptr) {
    key = XXH3_64bits_withSeed(tlut->data, size_t{tlut->numEntries} * sizeof(u16),
                               key ^ (u64{tlut->numEntries} | u64{static_cast<u32>(tlut->format)} << 16));
  }
  return key != 0 ? key : 1;
}

gfx::TextureHandle find_content_texture(u64 contentKey) noexcept {
  if (contentKey == 0) {
    return {};
  }
  const auto it = s_textureContentCache.find(contentKey);
  return it != s_textureContentCache.end() ? it->second.handle : gfx::TextureHandle{};
}

void store_cached_texture(const GXTexObj_& obj, gfx::TextureHandle handle, const GXTlutObj_* tlut, u64 contentKey) {
  if (obj.texObjId == 0) {
    return;
  }

  if (contentKey != 0) {
    auto& content = s_textureContentCache[contentKey];
    content.handle = handle;
    content.unownedSince = 0;
  }
  const auto paletteData = gfx::PaletteDataKey::from(tlut);
  auto& entry = s_textureObjectCaches[obj.texObjId];
  if (entry.paletteData.id != paletteData.id) {
    clear_texture_dependency(obj.texObjId, entry.paletteData.id);
  }
  // The same object with new content (a texture the game rewrites in place):
  // the old image will not be loaded again, so it gets no grace period.
  if (entry.contentKey != 0 && entry.contentKey != contentKey) {
    s_textureContentCache.erase(entry.contentKey);
  }
  entry = {std::move(handle), gfx::TextureDataKey::from(obj), paletteData, gfx::current_frame(), contentKey};
  if (paletteData.id != 0) {
    s_tlutObjectCaches[paletteData.id].staticTextureUsers.insert(obj.texObjId);
  }
}

gfx::TextureHandle get_tlut_texture(const GXTlutObj_& tlut) {
  const bool cacheable = tlut.tlutObjId != 0 && !tlut.no_cache();
  if (cacheable) {
    auto& cache = s_tlutObjectCaches[tlut.tlutObjId];
    cache.lastUsed = gfx::current_frame();
    if (cache.tlutTexture.handle && cache.tlutTexture.sourceData.matches(tlut)) {
      return cache.tlutTexture.handle;
    }
    cache.dynamicPaletteTextures.clear();
    // Static conversions may already use this exact palette, even when its
    // GPU TLUT image has not been needed yet. Retain those users; invalidate
    // only stale dependencies instead of uploading correct images again.
    for (auto it = cache.staticTextureUsers.begin(); it != cache.staticTextureUsers.end();) {
      const auto image = s_textureObjectCaches.find(*it);
      if (image == s_textureObjectCaches.end() || !image->second.paletteData.matches(tlut)) {
        if (image != s_textureObjectCaches.end()) {
          s_textureObjectCaches.erase(image);
        }
        cache.staticTextureUsers.erase(it++);
      } else {
        ++it;
      }
    }
  }

  const auto handle = gfx::new_static_texture_2d(
      tlut.numEntries, 1, 1, gfx::tlut_texture_format(tlut.format),
      {static_cast<const u8*>(tlut.data), static_cast<size_t>(tlut.numEntries) * sizeof(u16)}, true, "Loaded TLUT");
  if (cacheable) {
    s_tlutObjectCaches[tlut.tlutObjId].tlutTexture = {handle, gfx::PaletteDataKey::from(&tlut)};
  }
  return handle;
}

gfx::TextureHandle resolve_static_texture(const GXTexObj_& obj) {
  ZoneScoped;
  if (obj.texObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && !entry.written && entry.sourceData.matches(obj) && entry.paletteData.id == 0) {
        entry.lastUsed = gfx::current_frame();
        return entry.handle;
      }
    }
  }

  const u64 contentKey = obj.no_cache() ? 0 : texture_content_key(obj, nullptr);
  if (auto handle = find_content_texture(contentKey)) {
    store_cached_texture(obj, handle, nullptr, contentKey);
    return handle;
  }
  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj); replacement.has_value()) {
    handle = *replacement;
  } else {
#if DEBUG
    const auto name = gfx::texture_replacement::build_texture_replacement_name(obj);
    const auto nameStr = name.c_str();
#else
    const auto nameStr = "GX Static Texture";
#endif
    handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),
                                        {static_cast<const uint8_t*>(obj.data), UINT32_MAX}, false, nameStr);
  }
  if (!obj.no_cache()) {
    store_cached_texture(obj, handle, nullptr, contentKey);
  }
  return handle;
}

gfx::TextureHandle resolve_static_palette_texture(const GXTexObj_& obj, const GXTlutObj_& tlut) {
  ZoneScoped;
  if (obj.texObjId != 0 && tlut.tlutObjId != 0) {
    if (const auto it = s_textureObjectCaches.find(obj.texObjId); it != s_textureObjectCaches.end()) {
      auto& entry = it->second;
      if (entry.handle && !entry.written && entry.sourceData.matches(obj) && entry.paletteData.matches(tlut)) {
        entry.lastUsed = gfx::current_frame();
        if (auto tlutIt = s_tlutObjectCaches.find(tlut.tlutObjId); tlutIt != s_tlutObjectCaches.end()) {
          tlutIt->second.lastUsed = entry.lastUsed;
        }
        return entry.handle;
      }
    }
  }

  const bool cacheable = !obj.no_cache() && tlut.tlutObjId != 0 && !tlut.no_cache();
  const u64 contentKey = cacheable ? texture_content_key(obj, &tlut) : 0;
  if (auto handle = find_content_texture(contentKey)) {
    store_cached_texture(obj, handle, &tlut, contentKey);
    return handle;
  }
  gfx::TextureHandle handle;
  if (const auto replacement = gfx::texture_replacement::find_replacement(obj, tlut); replacement.has_value()) {
    handle = *replacement;
  } else {
    auto converted = gfx::convert_texture_palette(
        obj.format(), obj.width(), obj.height(), obj.mip_count(), {static_cast<const u8*>(obj.data), UINT32_MAX},
        tlut.format, tlut.numEntries, {static_cast<const u8*>(tlut.data), static_cast<size_t>(tlut.numEntries) * 2});
    if (converted.data.empty()) {
      return {};
    }
    handle =
        gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), GX_TF_RGBA8_PC,
                                   {converted.data.data(), converted.data.size()}, false, "GX Static Palette Texture");
    handle->hasArbitraryMips = converted.hasArbitraryMips;
  }
  if (cacheable) {
    store_cached_texture(obj, handle, &tlut, contentKey);
  }
  return handle;
}

gfx::TextureHandle resolve_dynamic_palette_texture(const GXTexObj_& obj, const GXState::CopyTextureRef& source,
                                                   const GXTlutObj_& tlut) {
  ZoneScoped;

  const auto tlutHandle = get_tlut_texture(tlut);
  // Anonymous/retired palettes have no persistent cache owner. A local entry
  // still leaves queued draws and the bound slot holding their GPU resources.
  DynamicPaletteEntry uncachedEntry;
  auto& entry = tlut.tlutObjId != 0 && !tlut.no_cache()
                    ? s_tlutObjectCaches[tlut.tlutObjId].dynamicPaletteTextures[make_dynamic_palette_key(obj, source)]
                    : uncachedEntry;
  // get_tlut_texture above marked this palette's cache as used this frame.
  const bool needsInitialConversion = !entry.handle;
  if (needsInitialConversion) {
    // Use source size instead of target (logical) size
    entry.handle = gfx::new_conv_texture(source.handle->size.width, source.handle->size.height, GX_TF_RGBA8,
                                         "GX Dynamic Palette Texture");
  }
  if (needsInitialConversion || entry.sourceRevision != source.revision ||
      entry.tlutDataVersion != tlut.tlutDataVersion) {
    gfx::queue_palette_conv({
        .variant = obj.format() == GX_TF_C4 ? gfx::tex_palette_conv::Variant::FromFloat4
                                            : gfx::tex_palette_conv::Variant::FromFloat8,
        .src = source.handle,
        .dst = entry.handle,
        .tlut = tlutHandle,
    });
    entry.sourceRevision = source.revision;
    entry.tlutDataVersion = tlut.tlutDataVersion;
  }
  return entry.handle;
}

u32 resolved_format_for_handle(const gfx::TextureHandle& handle) {
  if (!handle) {
    return GX_TF_RGBA8;
  }
  if (handle->gxFormat != gfx::InvalidTextureFormat) {
    return handle->gxFormat;
  }
  return GX_TF_RGBA8_PC;
}

template <typename T>
T round_away_from_zero(float value) noexcept {
  return static_cast<T>(value < 0.0f ? std::floor(value) : std::ceil(value));
}

std::pair<f32, f32> polygon_offset_for_cull_mode(GXCullMode cullMode) noexcept {
  if (cullMode == GX_CULL_FRONT) {
    return {g_gxState.backOffset, g_gxState.backScale};
  }
  return {g_gxState.frontOffset, g_gxState.frontScale};
}
} // namespace

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);

  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;

  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));

  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

/* WS16 (mp6-native port, black-bar/boxing live-resize fix).
 *
 * g_gxState.logicalViewport/logicalScissor are the RAW arguments from the
 * game's own last GXSetViewport/GXSetScissor call, expressed in units of
 * "the logical framebuffer size AT THAT CALL" (every widened camera/2D
 * setup in this port passes mp6_widescreen_render_width() as this exact
 * width, by construction, matching whatever RenderMode->fbWidth was at
 * that moment). On real hardware that size is a process-lifetime constant,
 * so this is a non-issue there: GXSetViewport is set once and its effect
 * stays valid forever, matching a real HW register that never needs
 * resubmitting. This port's own dynamic-widescreen feature intentionally
 * breaks that invariant: RenderMode->fbWidth changes on every live window
 * resize (via VIConfigure, see vi.cpp's configure() below), independent of
 * whether the game ever re-issues GXSetViewport/GXSetScissor again -- and
 * several scene/camera/UI setups in this game run their OWN GXSetViewport
 * call exactly ONCE, matching the real-hardware idiom.
 *
 * map_logical_viewport()/map_logical_scissor() already recompute their
 * scale ratio every single frame (gfx::begin_frame(), lib/gfx/common.cpp:
 * "Refresh render viewport/scissor from logical in case FB size changed")
 * from a FRESH logical_fb_size() denominator -- but a STALE numerator
 * (this cached logicalViewport/logicalScissor, left over from whichever
 * GXSetViewport call happened before the resize) still maps to the WRONG
 * absolute rectangle even so: scaleX = newTargetWidth/newLogicalWidth,
 * applied to an old-logical-width-shaped rect, undershoots the new target.
 * Confirmed by direct capture + math: a live 1280x720->2560x1180 resize
 * left content filling only ~2085 of 2560px -- exactly
 * 2560*(848/1040), the ratio between the OLD (1280x720-derived, 848) and
 * NEW (2560x1180-derived, 1040) logical widths this port computes, i.e.
 * the frame was mapped using the NEW denominator against the STALE
 * numerator instead of a numerator that agrees with it.
 *
 * Fix: whenever vi::configure() (VIConfigure) actually changes
 * logical_fb_size()'s own non-offscreen source (vi::configured_fb_size()),
 * proportionally rescale the CACHED logicalViewport/logicalScissor from
 * the OLD size to the NEW one -- preserving whatever fraction of the
 * logical frame they covered (a full-frame viewport stays full-frame; a
 * genuine sub-rect, e.g. a split-screen half, stays the same fraction
 * too), so the very next map_logical_viewport()/map_logical_scissor() call
 * (already running every frame, see above) maps a numerator that once
 * again agrees with the fresh denominator -- with no dependency on the
 * game ever re-issuing GXSetViewport/GXSetScissor. Deliberately NOT hooked
 * inside logical_fb_size()/map_logical_viewport() themselves, since those
 * also run for OFFSCREEN targets (gfx::is_offscreen()) whose size
 * legitimately changes on every EFB-copy call for unrelated reasons (a
 * texture-copy destination's own size, nothing to do with this port's
 * live-resize tracking) -- rescaling on THOSE changes would corrupt a
 * viewport/scissor the game just correctly set for that specific copy.
 * vi::configure() is the one and only place logical_fb_size()'s onscreen
 * source actually changes, so hooking there has no such risk. A true
 * no-op whenever oldLogicalSize/newLogicalSize actually match (true every
 * tick except the rare one where a live resize -- or any other
 * VIConfigure call that changes size -- just happened) or either old axis
 * is zero (nothing sane to rescale against, e.g. a never-yet-configured
 * RenderMode). */
void rescale_logical_viewport_scissor(Vec2<uint32_t> oldLogicalSize, Vec2<uint32_t> newLogicalSize) noexcept {
  if (oldLogicalSize.x == 0 || oldLogicalSize.y == 0 || oldLogicalSize == newLogicalSize) {
    return;
  }
  const float scaleX = static_cast<float>(newLogicalSize.x) / static_cast<float>(oldLogicalSize.x);
  const float scaleY = static_cast<float>(newLogicalSize.y) / static_cast<float>(oldLogicalSize.y);
  auto& vp = g_gxState.logicalViewport;
  vp.left *= scaleX;
  vp.width *= scaleX;
  vp.top *= scaleY;
  vp.height *= scaleY;
  auto& sc = g_gxState.logicalScissor;
  sc.x = round_away_from_zero<int32_t>(static_cast<float>(sc.x) * scaleX);
  sc.width = round_away_from_zero<int32_t>(static_cast<float>(sc.width) * scaleX);
  sc.y = round_away_from_zero<int32_t>(static_cast<float>(sc.y) * scaleY);
  sc.height = round_away_from_zero<int32_t>(static_cast<float>(sc.height) * scaleY);
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

Mat4x4<float> viewport_clip_projection(const Mat4x4<float>& proj) noexcept {
#ifdef __EMSCRIPTEN__
  const auto& vp = g_gxState.renderViewport;
  const auto size = gfx::get_render_target_size();
  gfx::Viewport clipped = vp;
  if (!gfx::clip_viewport_to_target(clipped, static_cast<float>(size.x), static_cast<float>(size.y)) ||
      clipped.width <= 0.f || clipped.height <= 0.f || vp.width <= 0.f || vp.height <= 0.f) {
    return proj;
  }
  // Pixel x = left + (ndc.x + 1) / 2 * width must hold for the clipped
  // viewport too (y runs down from top, ndc.y up): ndc' = s * ndc + o,
  // applied in clip space as x' = s * x + o * w.
  const float sx = vp.width / clipped.width;
  const float ox = (2.f * (vp.left - clipped.left) + vp.width - clipped.width) / clipped.width;
  const float sy = vp.height / clipped.height;
  const float oy = (clipped.height - 2.f * (vp.top - clipped.top) - vp.height) / clipped.height;
  Mat4x4<float> out = proj;
  out.m0 = proj.m0 * Vec4<float>{sx, sx, sx, sx} + proj.m3 * Vec4<float>{ox, ox, ox, ox};
  out.m1 = proj.m1 * Vec4<float>{sy, sy, sy, sy} + proj.m3 * Vec4<float>{oy, oy, oy, oy};
  return out;
#else
  return proj;
#endif
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(g_gxState.logicalScissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

void evict_texture_object(u32 texObjId) noexcept {
  if (const auto it = s_textureObjectCaches.find(texObjId); it != s_textureObjectCaches.end()) {
    clear_texture_dependency(texObjId, it->second.paletteData.id);
    s_textureObjectCaches.erase(it);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}

void evict_tlut_object(u32 tlutObjId) noexcept {
  if (const auto it = s_tlutObjectCaches.find(tlutObjId); it != s_tlutObjectCaches.end()) {
    for (const u32 texObjId : it->second.staticTextureUsers) {
      s_textureObjectCaches.erase(texObjId);
    }
    s_tlutObjectCaches.erase(it);
  }
  // If there is a loaded slot with this ID, mark it as no_cache to avoid inserting it when it's resolved.
  // This also handles the case where the texture was created, loaded, and immediately destroyed before we resolved it.
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}

// GX has no "texture freed" call: the game frees textures with its heap. The
// host reports each released block or heap (aurora_texture_sources_released),
// and conversions read from there are dropped at the end of the frame, so GPU
// memory follows the game's own memory. Textures that are only drawn now and
// then stay converted while their source lives.
static std::mutex g_releasedMutex;
static std::vector<std::pair<uintptr_t, uintptr_t>> g_releasedRanges;
// Copy destinations written this frame. A release reported in the same frame
// may predate the copy into reused memory, so those copies stay.
static absl::flat_hash_set<const void*> s_copiedThisFrame;

void note_copy_texture(const void* dest) noexcept { s_copiedThisFrame.insert(dest); }
void evict_copy_texture(const void* dest) noexcept;

static void drop_released_textures() noexcept {
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
  absl::flat_hash_set<const void*> copiedThisFrame;
  copiedThisFrame.swap(s_copiedThisFrame);
  {
    std::lock_guard lock{g_releasedMutex};
    if (g_releasedRanges.empty()) return;
    ranges.swap(g_releasedRanges);
  }
  std::sort(ranges.begin(), ranges.end());
  size_t merged = 0;
  for (const auto& range : ranges) {
    if (merged != 0 && range.first <= ranges[merged - 1].second) {
      ranges[merged - 1].second = std::max(ranges[merged - 1].second, range.second);
    } else {
      ranges[merged++] = range;
    }
  }
  ranges.resize(merged);
  const auto released = [&](const void* p) {
    const auto at = reinterpret_cast<uintptr_t>(p);
    auto it = std::upper_bound(ranges.begin(), ranges.end(), at,
                               [](uintptr_t value, const auto& range) { return value < range.first; });
    return it != ranges.begin() && at < (--it)->second;
  };
  // An EFB copy outranks memory at its destination, so once that memory is
  // freed the copy must go too: a texture the game later writes there (the
  // instruction preview after the bus menu) would otherwise sample the copy.
  std::vector<const void*> copyDests;
  for (const auto& [dest, _] : g_gxState.copyTextures) {
    if (released(dest) && !copiedThisFrame.contains(dest)) copyDests.push_back(dest);
  }
  for (const auto& [key, _] : g_gxState.copyTextureCache) {
    if (released(key.dest) && !copiedThisFrame.contains(key.dest)) copyDests.push_back(key.dest);
  }
  for (const void* dest : copyDests) evict_copy_texture(dest);
  for (auto it = s_textureObjectCaches.begin(); it != s_textureObjectCaches.end();) {
    const auto& entry = it->second;
    if (released(entry.sourceData.data) || (entry.paletteData.id != 0 && released(entry.paletteData.data))) {
      clear_texture_dependency(it->first, entry.paletteData.id);
      s_textureObjectCaches.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto it = s_tlutObjectCaches.begin(); it != s_tlutObjectCaches.end();) {
    auto& cache = it->second;
    if (cache.tlutTexture.handle && released(cache.tlutTexture.sourceData.data)) {
      cache.tlutTexture = {};
      cache.dynamicPaletteTextures.clear();
    }
    if (!cache.tlutTexture.handle && cache.dynamicPaletteTextures.empty() && cache.staticTextureUsers.empty()) {
      s_tlutObjectCaches.erase(it++);
    } else {
      ++it;
    }
  }
}

// Some textures are drawn into by the game itself (Mowtown's mowed lawn), and
// GXInitTexObj's version is only a sampled fingerprint, so a small edit leaves
// it unchanged. Like the GameCube, the game flushes the CPU cache over what it
// wrote before the GPU reads it; the host reports those flushes
// (aurora_texture_sources_written). Conversions and binds read from there are
// rechecked at their next draw: unchanged bytes find their conversion by
// content, changed bytes convert again.
static std::mutex g_writtenMutex;
static std::vector<std::pair<uintptr_t, uintptr_t>> g_writtenRanges;
static std::atomic_bool g_writtenPending{false};

static uintptr_t texture_source_end(const gfx::TextureDataKey& key) noexcept {
  const auto begin = reinterpret_cast<uintptr_t>(key.data);
  if (key.format > GX_TF_CMPR) return begin + 1;
  return begin + GXGetTexBufferSize(key.width, key.height, key.format, key.mipCount > 1, key.mipCount);
}

static void apply_written_textures() noexcept {
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
  {
    std::lock_guard lock{g_writtenMutex};
    ranges.swap(g_writtenRanges);
  }
  if (ranges.empty()) return;
  const auto written = [&](const void* data, uintptr_t end) {
    const auto begin = reinterpret_cast<uintptr_t>(data);
    if (data == nullptr) return false;
    for (const auto& range : ranges) {
      if (begin < range.second && range.first < end) return true;
    }
    return false;
  };
  const auto palette_written = [&](const gfx::PaletteDataKey& key) {
    return key.id != 0 && written(key.data, reinterpret_cast<uintptr_t>(key.data) + size_t{key.numEntries} * 2);
  };
  for (auto& [_, entry] : s_textureObjectCaches) {
    if (written(entry.sourceData.data, texture_source_end(entry.sourceData)) || palette_written(entry.paletteData)) {
      entry.written = true;
    }
  }
  for (auto& [_, cache] : s_tlutObjectCaches) {
    if (cache.tlutTexture.handle && palette_written(cache.tlutTexture.sourceData)) {
      cache.tlutTexture = {};
      cache.dynamicPaletteTextures.clear();
    }
  }
  for (auto& bind : g_gxState.textures) {
    if (bind.ref && !g_gxState.copyTextures.contains(bind.texObj.data) &&
        (written(bind.sourceData.data, texture_source_end(bind.sourceData)) || palette_written(bind.paletteData))) {
      bind.reset();
    }
  }
}

// A slower fallback for sources the host never reports: textures no draw has
// used for MP6_TEXTURE_IDLE_SECONDS (default 30, 0 keeps them) are dropped.
// Bound textures stay.
static bool texture_bound(const gfx::TextureHandle& handle) noexcept {
  for (const auto& bind : g_gxState.textures) {
    if (bind.ref == handle) return true;
  }
  return false;
}

static std::atomic_int g_textureIdleOverride{0};
} // namespace aurora::gx
namespace aurora::gfx {
void set_texture_idle_seconds(int seconds) noexcept { gx::g_textureIdleOverride.store(seconds, std::memory_order_relaxed); }
} // namespace aurora::gfx
namespace aurora::gx {

static void sweep_content_textures() noexcept {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  static u32 lastSecond = 0;
  static const u32 envKeepSeconds = [] {
    const char* env = std::getenv("MP6_TEXTURE_KEEP_SECONDS");
    return env != nullptr && env[0] != '\0' ? static_cast<u32>(std::max(0, std::atoi(env))) : 10u;
  }();
  // +1 so 0 stays free to mean "owned".
  const u32 second = static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count()) + 1;
  if (second == lastSecond) return;
  lastSecond = second;
  // A host under memory pressure (the web GPU budget) keeps nothing unowned.
  const u32 keepSeconds = g_textureIdleOverride.load(std::memory_order_relaxed) > 0 ? 0 : envKeepSeconds;
  absl::flat_hash_set<u64> owned;
  owned.reserve(s_textureObjectCaches.size());
  for (const auto& [_, entry] : s_textureObjectCaches) {
    if (entry.contentKey != 0) owned.insert(entry.contentKey);
  }
  for (auto it = s_textureContentCache.begin(); it != s_textureContentCache.end();) {
    auto& entry = it->second;
    if (owned.contains(it->first)) {
      entry.unownedSince = 0;
    } else if (entry.unownedSince == 0 && keepSeconds != 0) {
      entry.unownedSince = second;
    } else if (keepSeconds == 0 || second - entry.unownedSince >= keepSeconds) {
      s_textureContentCache.erase(it++);
      continue;
    }
    ++it;
  }
}

void sweep_idle_textures() noexcept {
  drop_released_textures();
  sweep_content_textures();
  using Clock = std::chrono::steady_clock;
  static const int envIdleSeconds = [] {
    const char* env = std::getenv("MP6_TEXTURE_IDLE_SECONDS");
    return env != nullptr && env[0] != '\0' ? std::max(0, std::atoi(env)) : 30;
  }();
  // A host under memory pressure (the web GPU budget) may shorten it.
  const int overrideSeconds = g_textureIdleOverride.load(std::memory_order_relaxed);
  const int idleSeconds =
      envIdleSeconds == 0 ? 0 : overrideSeconds > 0 ? std::min(overrideSeconds, envIdleSeconds) : envIdleSeconds;
  if (idleSeconds == 0) return;
  // The frame counter at each of the last few one-second marks: a texture is
  // idle when no frame since `idleSeconds` ago resolved it.
  static std::array<u32, 16> marks{};
  static size_t markCount = 0;
  static Clock::time_point nextMark{};
  const auto now = Clock::now();
  if (now < nextMark) return;
  nextMark = now + std::chrono::seconds(1);
  marks[markCount++ % marks.size()] = gfx::current_frame();
  if (markCount <= static_cast<size_t>(idleSeconds)) return;
  const u32 idleBefore = marks[(markCount - 1 - static_cast<size_t>(idleSeconds)) % marks.size()];
  const u32 frame = gfx::current_frame();
  const auto idle = [&](u32 lastUsed) { return frame - lastUsed > frame - idleBefore; };
  for (auto it = s_textureObjectCaches.begin(); it != s_textureObjectCaches.end();) {
    if (idle(it->second.lastUsed) && !texture_bound(it->second.handle)) {
      clear_texture_dependency(it->first, it->second.paletteData.id);
      s_textureObjectCaches.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto it = s_tlutObjectCaches.begin(); it != s_tlutObjectCaches.end();) {
    auto& cache = it->second;
    if (cache.staticTextureUsers.empty() && idle(cache.lastUsed)) {
      bool bound = false;
      for (const auto& [_, entry] : cache.dynamicPaletteTextures) bound = bound || texture_bound(entry.handle);
      if (!bound) {
        s_tlutObjectCaches.erase(it++);
        continue;
      }
    }
    ++it;
  }
}

void clear_copy_texture_cache() noexcept {
  g_gxState.copyTextures.clear();
  g_gxState.copyTextureCache.clear();
  for (auto& [_, cache] : s_tlutObjectCaches) {
    cache.dynamicPaletteTextures.clear();
  }
}

} // namespace aurora::gx

extern "C" void aurora_texture_sources_released(const void* begin, const void* end) {
  if (begin >= end) return;
  std::lock_guard lock{aurora::gx::g_releasedMutex};
  aurora::gx::g_releasedRanges.emplace_back(reinterpret_cast<uintptr_t>(begin), reinterpret_cast<uintptr_t>(end));
}

extern "C" void aurora_texture_sources_written(const void* begin, const void* end) {
  if (begin >= end) return;
  {
    std::lock_guard lock{aurora::gx::g_writtenMutex};
    aurora::gx::g_writtenRanges.emplace_back(reinterpret_cast<uintptr_t>(begin), reinterpret_cast<uintptr_t>(end));
  }
  aurora::gx::g_writtenPending.store(true, std::memory_order_release);
}

namespace aurora::gx {
void clear_static_texture_cache() noexcept { s_staticTextureCacheClearPending.store(true, std::memory_order_release); }
bool static_texture_cache_clear_pending() noexcept {
  return s_staticTextureCacheClearPending.load(std::memory_order_acquire);
}

void evict_copy_texture(const void* dest) noexcept {
  absl::flat_hash_set<const void*> sourceIdentities;
  if (const auto it = g_gxState.copyTextures.find(dest); it != g_gxState.copyTextures.end()) {
    if (it->second.handle) {
      sourceIdentities.insert(it->second.handle.get());
    }
    g_gxState.copyTextures.erase(it);
  }

  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      if (it->second.handle) {
        sourceIdentities.insert(it->second.handle.get());
      }
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }

  if (sourceIdentities.empty()) {
    return;
  }

  for (auto& [_, cache] : s_tlutObjectCaches) {
    for (auto it = cache.dynamicPaletteTextures.begin(); it != cache.dynamicPaletteTextures.end();) {
      if (sourceIdentities.contains(it->first.sourceIdentity)) {
        cache.dynamicPaletteTextures.erase(it++);
      } else {
        ++it;
      }
    }
  }
}

void resolve_sampled_textures(const ShaderInfo& info) noexcept {
  ZoneScoped;
  if (g_writtenPending.load(std::memory_order_relaxed) && g_writtenPending.exchange(false, std::memory_order_acquire)) {
    apply_written_textures();
  }
  // UI replacement reload requests are consumed on the render producer, before
  // the binding fast path. No atomic RMW on ordinary draws or texture misses.
  if (s_staticTextureCacheClearPending.load(std::memory_order_relaxed) &&
      s_staticTextureCacheClearPending.exchange(false, std::memory_order_acq_rel)) {
    do_clear_static_texture_cache();
  }

  for (u32 i = 0; i < MaxTextures; ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }

    const auto& obj = g_gxState.loadedTextures[i];
    auto& textureBind = g_gxState.textures[i];
    const bool palette = is_palette_format(obj.format());
    const auto tlutIdx = static_cast<size_t>(obj.tlut);
    const GXTlutObj_* tlut = palette && tlutIdx < g_gxState.loadedTluts.size()
                                ? &g_gxState.loadedTluts[tlutIdx] : nullptr;
    if (textureBind.ref && obj.texObjId != 0 && obj.texObjId == textureBind.texObj.texObjId &&
        textureBind.sourceData.matches(obj) &&
        (!palette || (tlut && tlut->tlutObjId != 0 && textureBind.paletteData.matches(*tlut)))) {
      continue;
    }

    gfx::TextureHandle handle;
    const auto copyIt = g_gxState.copyTextures.find(obj.data);
    const GXState::CopyTextureRef* copyRef = copyIt != g_gxState.copyTextures.end() ? &copyIt->second : nullptr;
    if (palette) {
      if (tlut && tlut->data != nullptr && tlut->numEntries != 0) {
        if (copyRef != nullptr) {
          handle = resolve_dynamic_palette_texture(obj, *copyRef, *tlut);
        } else if (obj.has_data()) {
          handle = resolve_static_palette_texture(obj, *tlut);
        }
      }
    } else if (copyRef != nullptr) {
      handle = copyRef->handle;
    } else if (obj.has_data()) {
      handle = resolve_static_texture(obj);
    }

    textureBind = gfx::TextureBind{obj, std::move(handle), tlut};
    textureBind.texObj.mFormat = resolved_format_for_handle(textureBind.ref);
  }
}

static inline wgpu::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return wgpu::BlendFactor::Zero;
  case GX_BL_ONE:
    return wgpu::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (isDst) {
      return wgpu::BlendFactor::Src;
    } else {
      return wgpu::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (isDst) {
      return wgpu::BlendFactor::OneMinusSrc;
    } else {
      return wgpu::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return wgpu::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return wgpu::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

static inline wgpu::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return wgpu::CompareFunction::Always;
  }
}

static inline wgpu::BlendState to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac,
                                              GXLogicOp op, u32 dstAlpha) {
  wgpu::BlendComponent colorBlendComponent;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
    break;
  case GX_BM_BLEND:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false),
        .dstFactor = to_blend_factor(dstFac, true),
    };
    break;
  case GX_BM_SUBTRACT:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::ReverseSubtract,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::One,
    };
    break;
  case GX_BM_LOGIC:
    switch (op) {
      DEFAULT_FATAL("unsupported logic op {}", underlying(op));
    case GX_LO_CLEAR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_COPY:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }
    break;
  }
  wgpu::BlendComponent alphaBlendComponent;
  if (dstAlpha != UINT32_MAX) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::Constant,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
  } else {
    alphaBlendComponent = colorBlendComponent;
  }
  return {
      .color = colorBlendComponent,
      .alpha = alphaBlendComponent,
  };
}

static inline wgpu::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  wgpu::ColorWriteMask writeMask = wgpu::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

static inline wgpu::PrimitiveState to_primitive_state(GXCullMode gx_cullMode) {
  auto cullMode = wgpu::CullMode::None;
  switch (gx_cullMode) {
    DEFAULT_FATAL("unsupported cull mode {}", underlying(gx_cullMode));
  case GX_CULL_FRONT:
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_BACK:
    cullMode = wgpu::CullMode::Back;
    break;
  case GX_CULL_NONE:
    break;
  }
  return {
      .topology = wgpu::PrimitiveTopology::TriangleList,
      .stripIndexFormat = wgpu::IndexFormat::Undefined,
      .frontFace = wgpu::FrontFace::CW,
      .cullMode = cullMode,
  };
}

wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept {
  ZoneScoped;
  const float depthBias = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetBits);
  const float depthBiasSlopeScale = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetScaleBits);
  const float depthBiasClamp = webgpu::g_hasCoreFeatures
                                   ? std::bit_cast<float>(config.polygonOffsetClampBits)
                                   : 0.0f;
  const wgpu::DepthStencilState depthStencil{
      .format = g_graphicsConfig.depthFormat,
      .depthWriteEnabled = config.depthCompare && config.depthUpdate,
      .depthCompare = config.depthCompare ? to_compare_function(config.depthFunc) : wgpu::CompareFunction::Always,
      .depthBias = round_away_from_zero<int32_t>(depthBias),
      .depthBiasSlopeScale = depthBiasSlopeScale,
      .depthBiasClamp = depthBiasClamp,
  };
  const auto blendState =
      to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp, config.dstAlpha);
  // An enabled ONE/ZERO blend is mathematically a replacement, but still
  // advertises a framebuffer dependency to mobile tile/hidden-surface paths.
  // Only written channels matter; retain destination-alpha and real blending.
  const auto replaces = [](const wgpu::BlendComponent& c) {
    return c.operation == wgpu::BlendOperation::Add &&
           c.srcFactor == wgpu::BlendFactor::One && c.dstFactor == wgpu::BlendFactor::Zero;
  };
  const bool opaque = (!config.colorUpdate || replaces(blendState.color)) &&
                      (!config.alphaUpdate || replaces(blendState.alpha));
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .blend = opaque ? nullptr : &blendState,
      .writeMask = to_write_mask(config.colorUpdate, config.alphaUpdate),
  }};
  const wgpu::FragmentState fragmentState{
      .module = shader,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = sPipelineLayout,
      .vertex =
          {
              .module = shader,
              .entryPoint = "vs_main",
              .bufferCount = static_cast<uint32_t>(vtxBuffers.size()),
              .buffers = vtxBuffers.data(),
          },
      .primitive = to_primitive_state(config.cullMode),
      .depthStencil = &depthStencil,
      .multisample =
          wgpu::MultisampleState{
              .count = config.msaaSamples,
          },
      .fragment = &fragmentState,
  };
  return webgpu::create_render_pipeline(descriptor);
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ZoneScoped;

  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig.fogType = g_gxState.fog.type;
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nbt3 = nbt3,
    };
    switch (type) {
    case GX_DIRECT: {
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    }
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 3 : 1;
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 6 : 2;
      break;
    default:
      Log.fatal("populate_pipeline_config: Invalid vertex type {}", type);
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  const auto cullMode = config.shaderConfig.lineMode == 0 ? g_gxState.cullMode : GX_CULL_NONE;
  const auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  config = {
      .msaaSamples = gfx::get_sample_count(),
      .shaderConfig = config.shaderConfig,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = g_gxState.blendFacSrc,
      .blendFacDst = g_gxState.blendFacDst,
      .blendOp = g_gxState.blendOp,
      .dstAlpha = g_gxState.dstAlpha,
      .polygonOffsetBits = std::bit_cast<uint32_t>(polygonOffset),
      .polygonOffsetScaleBits = std::bit_cast<uint32_t>(polygonOffsetScale),
      .polygonOffsetClampBits = std::bit_cast<uint32_t>(g_gxState.clamp),
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = g_gxState.depthUpdate,
      .alphaUpdate = g_gxState.alphaUpdate,
      .colorUpdate = g_gxState.colorUpdate,
  };
}

// Recording-thread-only memo. These are all inputs to get_descriptor(), not
// texture identity: two textures may safely use the same immutable sampler.
struct SamplerMemoKey {
  u32 mode0;
  u32 mode1;
  u32 anisotropy;
  u32 mipFlags;
  bool operator==(const SamplerMemoKey&) const = default;
};
struct SamplerMemo {
  SamplerMemoKey key{};
  wgpu::Sampler sampler;
};
static std::array<SamplerMemo, MaxTextures> sSamplerMemo;

struct TextureBindGroupMemoKey {
  WGPUBindGroupLayout layout{};
  std::array<WGPUTextureView, MaxTextures> textureViews{};
  std::array<WGPUSampler, MaxTextures> samplers{};
  bool operator==(const TextureBindGroupMemoKey&) const = default;
};
struct TextureBindGroupMemo {
  TextureBindGroupMemoKey key{};
  GXBindGroups bindGroups{};
  u32 frame = UINT32_MAX;
  bool valid = false;
};
static TextureBindGroupMemo sTextureBindGroupMemo;

constexpr size_t TextureBindGroupMemoTableSize = 64;
struct TextureBindGroupMemoTableEntry {
  TextureBindGroupMemoKey key{};
  GXBindGroups bindGroups{};
  bool valid = false;
};
static std::array<TextureBindGroupMemoTableEntry, TextureBindGroupMemoTableSize> sTextureBindGroupMemoTable;
static u32 sTextureBindGroupMemoFrame = UINT32_MAX;

static_assert(std::is_trivially_copyable_v<TextureBindGroupMemoKey>);
static_assert(std::has_unique_object_representations_v<TextureBindGroupMemoKey>);
static_assert(sizeof(TextureBindGroupMemoKey) == sizeof(WGPUBindGroupLayout) +
              sizeof(std::array<WGPUTextureView, MaxTextures>) + sizeof(std::array<WGPUSampler, MaxTextures>));

static size_t texture_bind_group_memo_slot(const TextureBindGroupMemoKey& key) noexcept {
  return static_cast<size_t>(xxh3_hash(key) & (TextureBindGroupMemoTableSize - 1));
}
void clear_bind_group_memo() noexcept {
  sTextureBindGroupMemo = {};
  for (auto& entry : sTextureBindGroupMemoTable) {
    entry.valid = false;
  }
  sTextureBindGroupMemoFrame = UINT32_MAX;
}

static void refresh_texture_bind_group_memo_frame(u32 frame) noexcept {
  if (sTextureBindGroupMemoFrame == frame) {
    return;
  }
  sTextureBindGroupMemo.valid = false;
  for (auto& entry : sTextureBindGroupMemoTable) {
    entry.valid = false;
  }
  sTextureBindGroupMemoFrame = frame;
}

static WGPUSampler texture_sampler(u32 slot, const gfx::TextureBind& tex) {
  const SamplerMemoKey key{
      tex.texObj.mode0, tex.texObj.mode1,
      static_cast<u32>(g_graphicsConfig.textureAnisotropy),
      (tex.ref->isReplacement ? 1u : 0u) |
          (tex.ref->generatedMips ? 2u : 0u) |
          (tex.ref->hasArbitraryMips ? 4u : 0u)};
  auto& memo = sSamplerMemo[slot];
  if (!memo.sampler || !(memo.key == key)) {
    memo.sampler = gfx::sampler_ref(tex.get_descriptor());
    memo.key = key;
  }
  return memo.sampler.Get();
}

GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    // Don't bother re-binding anything
    return {};
  }

  TextureBindGroupMemoKey key{};
  key.layout = sTextureBindGroupLayout.Get();
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    if (tex && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      key.textureViews[i] = tex.ref->sampleTextureView.Get();
      key.samplers[i] = texture_sampler(i, tex);
    } else {
      key.textureViews[i] = sEmptyTextureView.Get();
      key.samplers[i] = sEmptySampler.Get();
    }
  }
  const auto frame = gfx::current_frame();
  refresh_texture_bind_group_memo_frame(frame);
  if (sTextureBindGroupMemo.valid && sTextureBindGroupMemo.frame == frame &&
      sTextureBindGroupMemo.key == key) {
    return sTextureBindGroupMemo.bindGroups;
  }

  auto& tableEntry = sTextureBindGroupMemoTable[texture_bind_group_memo_slot(key)];
  if (tableEntry.valid && tableEntry.key == key) {
    sTextureBindGroupMemo = {
        .key = tableEntry.key,
        .bindGroups = tableEntry.bindGroups,
        .frame = frame,
        .valid = true,
    };
    return tableEntry.bindGroups;
  }

  // Using C WGPU types instead of C++ wrappers to avoid destructor overhead
  std::array<WGPUBindGroupEntry, MaxTextures * 2> textureEntries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    WGPUBindGroupEntry& textureEntry = textureEntries[i * 2];
    WGPUBindGroupEntry& samplerEntry = textureEntries[i * 2 + 1];
    textureEntry.binding = i * 2;
    samplerEntry.binding = i * 2 + 1;
    textureEntry.textureView = key.textureViews[i];
    samplerEntry.sampler = key.samplers[i];
  }
  const WGPUBindGroupDescriptor textureBindGroupDescriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = key.layout,
      .entryCount = textureEntries.size(),
      .entries = textureEntries.data(),
  };
  const GXBindGroups bindGroups{
      .textureBindGroup = gfx::bind_group_ref(textureBindGroupDescriptor),
  };
  sTextureBindGroupMemo = {
      .key = key,
      .bindGroups = bindGroups,
      .frame = frame,
      .valid = true,
  };
  tableEntry = {
      .key = key,
      .bindGroups = bindGroups,
      .valid = true,
  };
  return bindGroups;
}

void initialize() noexcept {
  fifo::invalidate_pipeline_config();
  clear_bind_group_memo();
  sSamplerMemo = {};
  {
    std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      textureEntries[i * 2] = {
          .binding = i * 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      textureEntries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      };
    }
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Texture Bind Group Layout",
        .entryCount = textureEntries.size(),
        .entries = textureEntries.data(),
    };
    sTextureBindGroupLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  {
    constexpr wgpu::SamplerDescriptor descriptor{.label = "Empty sampler"};
    sEmptySampler = gfx::sampler_ref(descriptor);
  }
  {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty texture",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    sEmptyTexture = g_device.CreateTexture(&descriptor);
    sEmptyTextureView = sEmptyTexture.CreateView();
  }
  {
    std::array<wgpu::BindGroupEntry, MaxTextures * 2> entries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      entries[i * 2] = {
          .binding = i * 2,
          .textureView = sEmptyTextureView,
      };
      entries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .sampler = sEmptySampler,
      };
    }
    const wgpu::BindGroupDescriptor desc{
        .label = "GX Empty Texture Bind Group",
        .layout = sTextureBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_emptyTextureBindGroup = g_device.CreateBindGroup(&desc);
  }
  {
    const std::array layouts{
        gfx::g_staticBindGroupLayout,
        gfx::g_uniformBindGroupLayout,
        sTextureBindGroupLayout,
    };
    const wgpu::PipelineLayoutDescriptor desc{
        .label = "GX Pipeline Layout",
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    sPipelineLayout = g_device.CreatePipelineLayout(&desc);
  }
}

void shutdown() noexcept {
  fifo::invalidate_pipeline_config();
  clear_bind_group_memo();
  sSamplerMemo = {};
  // TODO we should probably store this all in g_state.gx instead
  sSamplerBindGroupLayout = {};
  sTextureBindGroupLayout = {};
  {
    std::lock_guard lock{sBindGroupLayoutMutex};
    sUniformBindGroupLayouts.clear();
    sTextureBindGroupLayouts.clear();
  }
  for (auto& item : g_gxState.textures) {
    item.ref.reset();
  }
  s_textureObjectCaches.clear();
  s_tlutObjectCaches.clear();
  g_gxState.loadedTextures.fill({});
  g_gxState.loadedTluts.fill({});
  clear_copy_texture_cache();
}
} // namespace aurora::gx

static wgpu::AddressMode wgpu_address_mode(GXTexWrapMode mode) {
  switch (mode) {
    DEFAULT_FATAL("invalid wrap mode {}", underlying(mode));
  case GX_CLAMP:
    return wgpu::AddressMode::ClampToEdge;
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  }
}

static std::pair<wgpu::FilterMode, wgpu::MipmapFilterMode> wgpu_filter_mode(GXTexFilter filter) {
  switch (filter) {
    DEFAULT_FATAL("invalid filter mode {}", static_cast<int>(filter));
  case GX_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Undefined};
  case GX_LINEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Undefined};
  case GX_NEAR_MIP_NEAR:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Nearest};
  case GX_LIN_MIP_NEAR:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Nearest};
  case GX_NEAR_MIP_LIN:
    return {wgpu::FilterMode::Nearest, wgpu::MipmapFilterMode::Linear};
  case GX_LIN_MIP_LIN:
    return {wgpu::FilterMode::Linear, wgpu::MipmapFilterMode::Linear};
  }
}

static u16 wgpu_aniso(GXAnisotropy aniso) {
  switch (aniso) {
    DEFAULT_FATAL("invalid aniso {}", static_cast<int>(aniso));
  case GX_ANISO_1:
  case GX_MAX_ANISOTROPY:
    return 1;
  case GX_ANISO_2:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy / 2, 1);
  case GX_ANISO_4:
    return std::max<u16>(aurora::webgpu::g_graphicsConfig.textureAnisotropy, 1);
  }
}

wgpu::SamplerDescriptor aurora::gfx::TextureBind::get_descriptor() const noexcept {
  auto [minFilter, mipFilter] = wgpu_filter_mode(texObj.min_filter());
  auto [magFilter, _] = wgpu_filter_mode(texObj.mag_filter());
  const bool mipsEnabled = mipFilter != wgpu::MipmapFilterMode::Undefined;
  float minLod = texObj.min_lod();
  float maxLod = texObj.max_lod();
  u16 maxAnisotropy = wgpu_aniso(texObj.max_aniso());
  if (ref && ref->isReplacement) {
    minLod = 0.f;
    maxLod = 1000.f;
    if (!mipsEnabled) {
      mipFilter = wgpu::MipmapFilterMode::Nearest;
    }
  } else if (ref && ref->generatedMips) {
    // Renderer-generated mip chain on an EFB-copy texture
    // (GX_AURORA_SET_COPY_MIP_GEN): the guest texobj is single-level by
    // construction, so honor its GX_LINEAR min filter with trilinear
    // sampling across the generated chain instead of pinning LOD 0.
    minLod = 0.f;
    maxLod = 1000.f;
    mipFilter = minFilter == wgpu::FilterMode::Linear ? wgpu::MipmapFilterMode::Linear
                                                      : wgpu::MipmapFilterMode::Nearest;
  } else if (mipFilter == wgpu::MipmapFilterMode::Undefined) {
    minLod = 0.f;
    maxLod = 0.f;
  }
  if ((ref && ref->hasArbitraryMips) || !mipsEnabled) {
    maxAnisotropy = 1;
  } else if (maxAnisotropy > 1) {
    magFilter = wgpu::FilterMode::Linear;
    minFilter = wgpu::FilterMode::Linear;
    mipFilter = wgpu::MipmapFilterMode::Linear;
  }
  return {
      .label = "Generated Filtering Sampler",
      .addressModeU = wgpu_address_mode(texObj.wrap_s()),
      .addressModeV = wgpu_address_mode(texObj.wrap_t()),
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = magFilter,
      .minFilter = minFilter,
      .mipmapFilter = mipFilter,
      .lodMinClamp = minLod,
      .lodMaxClamp = maxLod,
      .maxAnisotropy = maxAnisotropy,
  };
} // namespace aurora::gx
