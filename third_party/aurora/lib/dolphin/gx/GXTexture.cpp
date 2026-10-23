#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/common.hpp" // aurora::xxh3_hash_s -- mp6-port patch 0006, see mp6_content_fingerprint below
#include "../../gfx/texture.hpp"
#include "dolphin/gx/GXAurora.h"

#include <algorithm>
#include <cstring>

#include "tracy/Tracy.hpp"

namespace {
constexpr u8 GXTexMode0Ids[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr u8 GXTexMode1Ids[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
constexpr u8 GXTexImage0Ids[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
constexpr u8 GXTexImage1Ids[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr u8 GXTexImage2Ids[8] = {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3};
constexpr u8 GXTexImage3Ids[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
constexpr u8 GXTexTlutIds[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
constexpr u8 GX2HWFiltConv[6] = {0x00, 0x04, 0x01, 0x05, 0x02, 0x06};

u32 sNextTexObjId = 1;
u32 sNextTlutObjId = 1;

u32 next_tex_obj_id() {
  if (sNextTexObjId == 0) {
    FATAL("texObj ID overflow");
  }
  return sNextTexObjId++;
}

u32 next_tlut_obj_id() {
  if (sNextTlutObjId == 0) {
    FATAL("tlutObj ID overflow");
  }
  return sNextTlutObjId++;
}

int __cntlzw(unsigned int val) {
  if (val == 0)
    return 32; // PowerPC returns 32 if the input is 0
#ifdef _MSC_VER
  unsigned long idx;
  _BitScanReverse(&idx, val);
  return 31 - (int)idx;
#else
  return __builtin_clz(val);
#endif
}


/* mp6-port patch 0005 (leak fix): Hudson engines re-run GXInitTexObj/
 * GXInitTlutObj on stack-locals EVERY FRAME for the same underlying
 * texture data. With counter-minted ids, each re-init misses
 * s_textureObjectCaches and stores a brand-new converted texture keyed by
 * an id that will never be looked up again -- unbounded growth (~4MB/frame
 * observed in MP6's opening: 18GB RSS in minutes). Deriving the id from
 * the object's IDENTITY (data pointer, dimensions, format) makes identical
 * re-inits cache-hit instead; explicit data changes still invalidate via
 * the existing texDataVersion/tlutDataVersion bumps (GXInitTexObjData/
 * GXInitTlutObjData). id 0 is reserved ("uncached") by the resolve paths.
 * 32-bit collision odds across the few hundred live textures a scene has
 * are ~1e-5 and the failure mode is a visual glitch, not corruption. */
static u32 mp6_stable_obj_id(const void* data, u32 a, u32 b, u32 c) {
  uint64_t x = (uint64_t)(uintptr_t)data;
  x ^= ((uint64_t)a << 40) ^ ((uint64_t)b << 20) ^ (uint64_t)c;
  x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27; x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  u32 id = (u32)(x ^ (x >> 32));
  return id ? id : 1u;
}

/* mp6-port patch 0006 (staleness fix, follows patch 0005 above): patch
 * 0005 made texObjId stable by IDENTITY (data pointer + dims + format +
 * wrap) so per-frame re-inits of the SAME underlying data cache-hit
 * instead of leaking a new entry every frame -- but texDataVersion was
 * left a constant 1 at init time (below), compared for equality in
 * gx.cpp's resolve_static_texture()/resolve_static_palette_texture()
 * cache lookups. If a DIFFERENT texture's data later lands at the SAME
 * heap address with the SAME dimensions/format/wrap (routine across a
 * scene transition: free scene A's texture buffer, allocate scene B's --
 * same allocator, same size class, same address), the identity hash
 * collides AND texDataVersion still reads as "1 == 1", so the cache
 * serves scene A's stale converted texture under scene B's new content.
 * Reported symptom: correct textures during a scene, wrong/missing ones
 * right after a transition (e.g. opening -> title, HEAP_MODEL reused).
 *
 * Fix: make texDataVersion (and tlutDataVersion, GXInitTlutObj below) a
 * cheap CONTENT fingerprint taken at init time, so unchanged content
 * still cache-hits (same bytes -> same fingerprint -- the whole point of
 * patch 0005 stays intact) but different content at a reused address
 * correctly busts the cache. Bounded, constant cost regardless of the
 * texture's real size -- this runs on EVERY GXInitTexObj/GXInitTlutObj
 * call (i.e. every frame, per patch 0005's own comment above), so it
 * deliberately does NOT hash the whole buffer (see the existing, much
 * more expensive full-buffer XXH64 in gfx/texture_replacement.cpp's
 * compute_referenced_tlut_hash/find_replacement -- that one is fine
 * because it only runs on an actual cache miss, not every init): just
 * the first 64 + last 64 bytes (or the whole thing, contiguously, when
 * byteSize <= 128) plus the byte count, through the same xxh3 helper
 * already used throughout this codebase for pipeline/bind-group cache
 * keys (gfx/common.hpp's xxh3_hash_s). id/version 0 stays reserved
 * ("uncached"/mismatch-by-construction) as before. */
/* mp6-port patch 0026 (follows patch 0006 directly above): patch 0006's
 * head/tail-only sampling window is not discriminating enough for a whole
 * CLASS of real game textures, and the failure is silent + visually wrong.
 *
 * Measured, not theorised. MP6's board player-status panels carry a
 * per-character portrait ("kao", DATA_board 0x36..0x43): all 14 are
 * 56x56 RGB5A3, all exactly 6272 bytes, and every one of them has the
 * SAME fully-transparent texels in its first and last GX tile -- so
 * patch 0006's own window (first 64 bytes + last 64 bytes + byteSize)
 * hashes IDENTICALLY for every character, while the images obviously
 * differ. Instrumented directly on the retail Party route (board/status.c
 * under MP6_STATUS_DIAG): four different portraits, four different
 * full-buffer hashes (36d850de / 730928b0 / ccf8dd85 / a56c7f13), ONE
 * shared head/tail fingerprint (ec7d8e8d).
 *
 * That alone is harmless until the address is reused, which the board
 * opening does routinely: the turn-order dice permute the players
 * (board/opening.c PlayerOrderSet -> mbPlayerSwap) and mbStatusReset()
 * then tears every panel down and rebuilds it, so each panel's portrait
 * buffer is freed and immediately re-allocated -- same allocator, same
 * size class, same address -- now holding a DIFFERENT character's
 * pixels. Same address + same dims/format/wrap => same
 * mp6_stable_obj_id(); identical head/tail => same texDataVersion; so
 * gx.cpp's resolve_static_texture() serves the PREVIOUS character's
 * converted texture. Observed on screen as the four status panels
 * keeping their pre-dice faces (Mario still top-left) while every other
 * field on the same panel -- rank, coins, the CPU badge -- correctly
 * followed the new turn order.
 *
 * Fix: keep the same constant-cost contract (this still runs on EVERY
 * GXInitTexObj, i.e. potentially hundreds of times per frame, so the
 * whole buffer is still deliberately NOT hashed), but spread the sample
 * over the ENTIRE buffer instead of clustering it at the two ends, where
 * transparent borders make textures look alike. Up to 64 evenly-spaced
 * 32-byte chunks (2 KB sampled, bounded regardless of texture size),
 * still including offset 0 and the final chunk, so this window is a
 * strict superset of patch 0006's discriminating power, plus 62 interior
 * probes through the region where two different images actually differ.
 * Buffers at or below the sample budget are hashed whole, which is
 * strictly better than sampling them. Unchanged content still yields an
 * identical fingerprint, so patch 0005's per-frame cache-hit behaviour
 * (the reason any of this is sampled rather than counted) is intact. */
static u32 mp6_content_fingerprint(const void* data, u32 byteSize) {
  if (data == nullptr || byteSize == 0 || (uintptr_t)data < 0x10000) {
    return 1u; // no real data yet -- stable, distinct from 0 ("uncached")
  }
  constexpr u32 kChunkLen = 32;
  constexpr u32 kChunks = 64;
  constexpr u32 kSampleMax = kChunkLen * kChunks; // 2 KB, size-independent

  const auto* p = static_cast<const u8*>(data);
  u8 scratch[kSampleMax + sizeof(u32)];
  u32 n = 0;

  if (byteSize <= kSampleMax) {
    memcpy(scratch, p, byteSize);
    n = byteSize;
  } else {
    // Chunk i starts at floor(span * i / (kChunks - 1)) with span =
    // byteSize - kChunkLen, so i==0 is the head, i==kChunks-1 is the tail,
    // and the remaining probes are spread uniformly across the interior.
    const u64 span = static_cast<u64>(byteSize) - kChunkLen;
    for (u32 i = 0; i < kChunks; ++i) {
      const u32 off = static_cast<u32>(span * i / (kChunks - 1));
      memcpy(scratch + n, p + off, kChunkLen);
      n += kChunkLen;
    }
  }
  memcpy(scratch + n, &byteSize, sizeof(byteSize));
  n += sizeof(byteSize);
  const u32 h = static_cast<u32>(aurora::xxh3_hash_s(scratch, n));
  return h ? h : 1u;
}

void init_texobj_common(GXTexObj_& obj, const void* data, u16 width, u16 height, u32 format, GXTexWrapMode wrapS,
                        GXTexWrapMode wrapT, GXBool mipmap) {
  obj = {};
  obj.mWidth = width;
  obj.mHeight = height;
  obj.mFormat = format;
  obj.tlut = GX_TLUT0;
  obj.flags = 2;
  obj.texObjId = mp6_stable_obj_id(data, ((u32)width << 16) | height, format, (u32)wrapS ^ ((u32)wrapT << 4) ^ ((u32)mipmap << 8));
  // mp6-port patch 0006: content fingerprint, not a constant -- see the comment above
  // mp6_content_fingerprint. Base-level (no-mips) size is enough to distinguish real
  // content differences; matches the existing texture_base_level_size()/
  // GXGetTexBufferSize(..., false, 0) idiom already used elsewhere in this codebase
  // (gfx/texture_replacement.cpp) for "the size of the buffer at obj.data".
  obj.texDataVersion = mp6_content_fingerprint(data, GXGetTexBufferSize(width, height, format, GX_FALSE, 0));

  SET_REG_FIELD(0, obj.mode0, 2, 0, wrapS);
  SET_REG_FIELD(0, obj.mode0, 2, 2, wrapT);
  SET_REG_FIELD(0, obj.mode0, 1, 4, 1);
  if (mipmap) {
    obj.flags |= 1;
    obj.mode0 = (obj.mode0 & 0xFFFFFF1F) | 0xC0;
    const u32 maxDim = std::max(width, height);
    const u8 lmax = static_cast<u8>(16.0f * static_cast<float>(31 - __cntlzw(maxDim)));
    SET_REG_FIELD(0, obj.mode1, 8, 8, lmax);
  } else {
    obj.mode0 = (obj.mode0 & 0xFFFFFF1F) | 0x80;
  }
  SET_REG_FIELD(0, obj.image0, 10, 0, static_cast<u32>(width - 1));
  SET_REG_FIELD(0, obj.image0, 10, 10, static_cast<u32>(height - 1));
  SET_REG_FIELD(0, obj.image0, 4, 20, format & 0xF);

  obj.data = data;
}

void emit_loaded_texobj_metadata(const GXTexObj_& obj, GXTexMapID id) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_TEXOBJ);
  GX_WRITE_U8(static_cast<u8>(id));
  GX_WRITE_U64(reinterpret_cast<u64>(obj.data));
  GX_WRITE_U32(obj.width());
  GX_WRITE_U32(obj.height());
  GX_WRITE_U32(obj.format());
  GX_WRITE_U32(static_cast<u32>(obj.tlut));
  GX_WRITE_U8(static_cast<u8>(obj.has_mips()));
  GX_WRITE_U32(obj.texObjId);
  GX_WRITE_U32(obj.texDataVersion);
}

void emit_loaded_tlut_metadata(const GXTlutObj_& obj, u32 idx) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_TLUT);
  GX_WRITE_U8(static_cast<u8>(idx));
  GX_WRITE_U64(reinterpret_cast<u64>(obj.data));
  GX_WRITE_U32(static_cast<u32>(obj.format));
  GX_WRITE_U16(obj.numEntries);
  GX_WRITE_U32(obj.tlutObjId);
  GX_WRITE_U32(obj.tlutDataVersion);
}
} // namespace

extern "C" {
void GXInitTexObj(GXTexObj* obj_, const void* data, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS,
                  GXTexWrapMode wrapT, GXBool mipmap) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  init_texobj_common(*obj, data, width, height, format, wrapS, wrapT, mipmap);
}

void GXInitTexObjCI(GXTexObj* obj_, const void* data, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrapS,
                    GXTexWrapMode wrapT, GXBool mipmap, u32 tlut) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  init_texobj_common(*obj, data, width, height, format, wrapS, wrapT, mipmap);
  obj->tlut = static_cast<GXTlut>(tlut);
  obj->flags &= ~2u;
}

void GXInitTexObjLOD(GXTexObj* obj_, GXTexFilter minFilt, GXTexFilter magFilt, float minLod, float maxLod,
                     float lodBias, GXBool biasClamp, GXBool doEdgeLod, GXAnisotropy maxAniso) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  const float clampedBias = std::clamp(lodBias, -4.0f, 3.99f);
  const auto lbias = static_cast<u8>(32.0f * clampedBias);
  SET_REG_FIELD(0, obj->mode0, 8, 9, lbias);
  SET_REG_FIELD(0, obj->mode0, 1, 4, magFilt == GX_LINEAR ? 1 : 0);
  SET_REG_FIELD(0, obj->mode0, 3, 5, GX2HWFiltConv[minFilt]);
  SET_REG_FIELD(0, obj->mode0, 1, 8, doEdgeLod ? 0 : 1);
  obj->mode0 &= 0xFFFDFFFF;
  obj->mode0 &= 0xFFFBFFFF;
  SET_REG_FIELD(0, obj->mode0, 2, 19, maxAniso);
  SET_REG_FIELD(0, obj->mode0, 1, 21, biasClamp);

  const auto clampedMin = std::clamp(minLod, 0.0f, 10.0f);
  const auto clampedMax = std::clamp(maxLod, 0.0f, 10.0f);
  SET_REG_FIELD(0, obj->mode1, 8, 0, static_cast<u8>(16.0f * clampedMin));
  SET_REG_FIELD(0, obj->mode1, 8, 8, static_cast<u8>(16.0f * clampedMax));
}

void GXInitTexObjData(GXTexObj* obj_, const void* data) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  obj->data = data;
  ++obj->texDataVersion;
}

void GXInitTexObjWrapMode(GXTexObj* obj_, GXTexWrapMode wrapS, GXTexWrapMode wrapT) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 2, 0, wrapS);
  SET_REG_FIELD(0, obj->mode0, 2, 2, wrapT);
}

void GXInitTexObjTlut(GXTexObj* obj_, u32 tlut) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  obj->tlut = static_cast<GXTlut>(tlut);
}

void GXInitTexObjFilter(GXTexObj* obj_, GXTexFilter minFilt, GXTexFilter magFilt) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 3, 5, GX2HWFiltConv[minFilt]);
  SET_REG_FIELD(0, obj->mode0, 1, 4, magFilt == GX_LINEAR ? 1 : 0);
}

void GXInitTexObjMaxLOD(GXTexObj* obj_, float maxLod) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  const auto clampedMax = std::clamp(maxLod, 0.0f, 10.0f);
  SET_REG_FIELD(0, obj->mode1, 8, 8, static_cast<u8>(16.0f * clampedMax));
}

void GXInitTexObjMinLOD(GXTexObj* obj_, float minLod) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  const auto clampedMin = std::clamp(minLod, 0.0f, 10.0f);
  SET_REG_FIELD(0, obj->mode1, 8, 0, static_cast<u8>(16.0f * clampedMin));
}

void GXInitTexObjLODBias(GXTexObj* obj_, float lodBias) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  const float clampedBias = std::clamp(lodBias, -4.0f, 3.99f);
  const auto lbias = static_cast<u8>(32.0f * clampedBias);
  SET_REG_FIELD(0, obj->mode0, 8, 9, lbias);
}

void GXInitTexObjBiasClamp(GXTexObj* obj_, GXBool biasClamp) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 1, 21, biasClamp);
}

void GXInitTexObjEdgeLOD(GXTexObj* obj_, GXBool doEdgeLod) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 1, 8, doEdgeLod ? 0 : 1);
}

void GXInitTexObjMaxAniso(GXTexObj* obj_, GXAnisotropy maxAniso) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 2, 19, maxAniso);
}

void GXInitTexObjUserData(GXTexObj* obj_, void* userData) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  obj->userData = userData;
}

void* GXGetTexObjUserData(const GXTexObj* obj_) {
  const auto* obj = reinterpret_cast<const GXTexObj_*>(obj_);
  return const_cast<void*>(obj->userData);
}

void GXLoadTexObj(GXTexObj* obj_, GXTexMapID id) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  SET_REG_FIELD(0, obj->mode0, 8, 24, GXTexMode0Ids[id]);
  SET_REG_FIELD(0, obj->mode1, 8, 24, GXTexMode1Ids[id]);
  SET_REG_FIELD(0, obj->image0, 8, 24, GXTexImage0Ids[id]);

  u32 image1 = 0;
  u32 image2 = 0;
  SET_REG_FIELD(0, image1, 8, 24, GXTexImage1Ids[id]);
  SET_REG_FIELD(0, image2, 8, 24, GXTexImage2Ids[id]);
  SET_REG_FIELD(0, obj->image3, 8, 24, GXTexImage3Ids[id]);

  GX_WRITE_RAS_REG(obj->mode0);
  GX_WRITE_RAS_REG(obj->mode1);
  GX_WRITE_RAS_REG(obj->image0);
  GX_WRITE_RAS_REG(image1);
  GX_WRITE_RAS_REG(image2);
  GX_WRITE_RAS_REG(obj->image3);

  if ((obj->flags & 2) == 0) {
    u32 tlut = 0;
    SET_REG_FIELD(0, tlut, 10, 0, static_cast<u32>(obj->tlut));
    SET_REG_FIELD(0, tlut, 8, 24, GXTexTlutIds[id]);
    GX_WRITE_RAS_REG(tlut);
  }

  __gx->tImage0[id] = obj->image0;
  __gx->tMode0[id] = obj->mode0;
  __gx->dirtyState |= 1;
  __gx->bpSent = 1;

  emit_loaded_texobj_metadata(*obj, id);
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mips, u8 maxLod) {
  s32 shiftX = 0;
  s32 shiftY = 0;
  switch (fmt) {
  case GX_TF_I4:
  case GX_TF_C4:
  case GX_TF_CMPR:
  case GX_CTF_R4:
  case GX_CTF_Z4:
    shiftX = 3;
    shiftY = 3;
    break;
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_C8:
  case GX_TF_Z8:
  case GX_CTF_RA4:
  case GX_CTF_A8:
  case GX_CTF_R8:
  case GX_CTF_G8:
  case GX_CTF_B8:
  case GX_CTF_Z8M:
  case GX_CTF_Z8L:
    shiftX = 3;
    shiftY = 2;
    break;
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_RGBA8:
  case GX_TF_C14X2:
  case GX_TF_Z16:
  case GX_TF_Z24X8:
  case GX_CTF_RA8:
  case GX_CTF_RG8:
  case GX_CTF_GB8:
  case GX_CTF_Z16L:
    shiftX = 2;
    shiftY = 2;
    break;
  default:
    break;
  }
  u32 bitSize = fmt == GX_TF_RGBA8 || fmt == GX_TF_Z24X8 ? 64 : 32;
  u32 bufLen = 0;
  if (mips) {
    while (maxLod != 0) {
      const u32 tileX = ((width + (1 << shiftX) - 1) >> shiftX);
      const u32 tileY = ((height + (1 << shiftY) - 1) >> shiftY);
      bufLen += bitSize * tileX * tileY;

      if (width == 1 && height == 1) {
        return bufLen;
      }

      width = (width < 2) ? 1 : width / 2;
      height = (height < 2) ? 1 : height / 2;
      --maxLod;
    };
  } else {
    const u32 tileX = ((width + (1 << shiftX) - 1) >> shiftX);
    const u32 tileY = ((height + (1 << shiftY) - 1) >> shiftY);
    bufLen = bitSize * tileX * tileY;
  }

  return bufLen;
}

void GXInitTlutObj(GXTlutObj* obj_, const void* data, GXTlutFmt format, u16 entries) {
  memset(obj_, 0, sizeof(GXTlutObj));
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  obj->data = data;
  obj->format = format;
  obj->numEntries = entries;
  obj->tlutObjId = mp6_stable_obj_id(data, entries, (u32)format, 0x7107u);
  // mp6-port patch 0006: content fingerprint, not a constant -- same staleness fix as
  // init_texobj_common above, "same for tlut". Buffer size matches the existing
  // tlut_bytes()/get_tlut_texture() idiom already used elsewhere in this file/codebase:
  // numEntries 16-bit TLUT entries.
  obj->tlutDataVersion = mp6_content_fingerprint(data, (u32)entries * sizeof(u16));

  SET_REG_FIELD(0, obj->tlut, 2, 10, format);
  SET_REG_FIELD(0, obj->loadTlut0, 8, 24, 0x64);
}

void GXInitTlutObjData(GXTlutObj* obj_, const void* data) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  obj->data = data;
  ++obj->tlutDataVersion;
}

void GXLoadTlut(const GXTlutObj* obj_, u32 idx) {
  auto* obj = reinterpret_cast<const GXTlutObj_*>(obj_);
  __GXFlushTextureState();
  GX_WRITE_RAS_REG(obj->loadTlut0);

  u32 loadTlut1 = 0;
  SET_REG_FIELD(0, loadTlut1, 10, 0, idx);
  SET_REG_FIELD(0, loadTlut1, 10, 10, obj->numEntries - 1);
  SET_REG_FIELD(0, loadTlut1, 8, 24, 0x65);
  GX_WRITE_RAS_REG(loadTlut1);
  __GXFlushTextureState();

  emit_loaded_tlut_metadata(*obj, idx);
}

// TODO GXInitTexCacheRegion
// TODO GXInitTexPreLoadRegion
// TODO GXInitTlutRegion
// TODO GXInvalidateTexRegion

void GXInvalidateTexAll() {
  // no-op?
}

// TODO GXPreLoadEntireTexture
// TODO GXSetTexRegionCallback
// TODO GXSetTlutRegionCallback
// TODO GXLoadTexObjPreLoaded
void GXSetTexCoordScaleManually(GXTexCoordID coord, GXBool enable, u16 ss, u16 ts) {
  __gx->tcsManEnab = (__gx->tcsManEnab & ~(1 << coord)) | (enable << coord);
  if (enable) {
    SET_REG_FIELD(0, __gx->suTs0[coord], 16, 0, static_cast<u16>(ss - 1));
    SET_REG_FIELD(0, __gx->suTs1[coord], 16, 0, static_cast<u16>(ts - 1));
    GX_WRITE_RAS_REG(__gx->suTs0[coord]);
    GX_WRITE_RAS_REG(__gx->suTs1[coord]);
    __gx->bpSent = 1;
  }
}

void GXSetTexCoordCylWrap(GXTexCoordID coord, GXBool s_enable, GXBool t_enable) {
  SET_REG_FIELD(0, __gx->suTs0[coord], 1, 17, s_enable);
  SET_REG_FIELD(0, __gx->suTs1[coord], 1, 17, t_enable);
  if (__gx->tcsManEnab & (1 << coord)) {
    GX_WRITE_RAS_REG(__gx->suTs0[coord]);
    GX_WRITE_RAS_REG(__gx->suTs1[coord]);
    __gx->bpSent = 1;
  }
}

void GXSetTexCoordBias(GXTexCoordID coord, GXBool s_enable, GXBool t_enable) {
  SET_REG_FIELD(0, __gx->suTs0[coord], 1, 16, s_enable);
  SET_REG_FIELD(0, __gx->suTs1[coord], 1, 16, t_enable);
  if (__gx->tcsManEnab & (1 << coord)) {
    GX_WRITE_RAS_REG(__gx->suTs0[coord]);
    GX_WRITE_RAS_REG(__gx->suTs1[coord]);
    __gx->bpSent = 1;
  }
}

void __GXFlushTextureState() {
  GX_WRITE_RAS_REG(__gx->bpMask);
  __gx->bpSent = 1;
}
}
