#include "command_processor.hpp"
#include "fifo.hpp"

#include "../gfx/common.hpp"
#include "../gfx/depth_peek.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "pipeline.hpp"
#include "ubershader.hpp"
#include "shader_info.hpp"
#include "../internal.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

// Value-only recording-thread memo. No GPU/resource/draw data is retained.
static bool g_pipelineConfigDirty = true;
struct ConfigMemo {
  PipelineConfig value{};
  u32 frame=0, samples=0;
  uint64_t serial=0; // changes whenever value is rebuilt
  GXPrimitive prim{};
  GXVtxFmt fmt{};
  bool valid=false;
};
static ConfigMemo sConfigMemo;
void invalidate_pipeline_config() noexcept {
  sConfigMemo.valid=false;
  g_pipelineConfigDirty=true;
}
#ifdef MP6_CONFIG_REUSE_DIAGNOSTIC
extern "C" unsigned mp6_diag_board_ticks(void);
static uint64_t sConfigCalls=0, sConfigHits=0;
static void report_config_reuse() {
  std::fprintf(stderr,"[CONFIG-REUSE] calls=%llu hits=%llu mismatch=0\n",
    static_cast<unsigned long long>(sConfigCalls),static_cast<unsigned long long>(sConfigHits));
}
#endif
static const PipelineConfig& get_pipeline_config(GXPrimitive prim, GXVtxFmt fmt) {
  const auto frame=gfx::current_frame();
  const auto samples=gfx::get_sample_count();
  const bool hit=sConfigMemo.valid && !g_pipelineConfigDirty && frame==sConfigMemo.frame &&
    samples==sConfigMemo.samples && prim==sConfigMemo.prim && fmt==sConfigMemo.fmt;
#ifdef MP6_CONFIG_REUSE_DIAGNOSTIC
  static bool registered=false;
  static PipelineConfig fresh;
  if (!registered) { std::atexit(report_config_reuse); registered=true; }
  fresh={};
  populate_pipeline_config(fresh,prim,fmt);
  const auto age=mp6_diag_board_ticks();
  if (age>=1800 && age<=5799) { ++sConfigCalls; if (hit) ++sConfigHits; }
  ASSERT(!hit || std::memcmp(&fresh,&sConfigMemo.value,sizeof(fresh))==0,
        "Pipeline config reuse mismatch");
  sConfigMemo.value=fresh;
#else
  if (hit) return sConfigMemo.value;
  // Restore all defaults, including inactive stages, without rebuilding the
  // large aggregate's identical member initializers on every cache miss.
  static constexpr PipelineConfig defaults{};
  sConfigMemo.value=defaults;
  populate_pipeline_config(sConfigMemo.value,prim,fmt);
#endif
  sConfigMemo.frame=frame; sConfigMemo.samples=samples;
  sConfigMemo.prim=prim; sConfigMemo.fmt=fmt; sConfigMemo.valid=true;
  ++sConfigMemo.serial;
  g_pipelineConfigDirty=false;
#ifdef MP6_CONFIG_REUSE_DIAGNOSTIC
  return fresh; // The oracle renders only freshly rebuilt data.
#else
  return sConfigMemo.value;
#endif
}



// Pipeline-config inputs also prevent merging the previous recorded draw.
static inline void mark_pipeline_state_dirty() noexcept {
  g_pipelineConfigDirty = true;
  g_gxState.stateDirty = true;
}

// Owned by the FIFO/game thread, independent of worker and GPU completion.
static uint64_t g_efbDepthWriteSerial = 0;
uint64_t efb_depth_write_serial() noexcept { return g_efbDepthWriteSerial; }

static void note_efb_depth_write() {
  if (g_gxState.depthCompare && g_gxState.depthUpdate && !gfx::is_offscreen()) {
    ++g_efbDepthWriteSerial;
  }
}

// [MP6] Retained replay (compat/aurora/windows/0006).
//
// Unlocked FPS presents extra frames by replaying the previous tick's GX
// stream with only its position/normal matrix payloads rewritten. Decoding the
// whole stream again for every such frame is most of a frame's CPU cost, so a
// replay stream is processed once with recording on: gfx keeps the region's
// commands and bytes, and this file records where each matrix lands in the
// uniforms and which stream bytes it came from. A later replay is re-emitted
// from the recording, with only those uniform bytes patched, when it is
// provably what a full decode would produce:
//   - every stream byte outside the direct pos/nrm matrix payloads is equal,
//   - the GX state it starts from is byte-equal (matrices aside: a matrix the
//     region reads before loading it is patched from the live state), and
//   - gfx sees the same frame position, targets, viewport and scissor.
// Anything else decodes in full and records again.
bool g_retainRecording = false;
namespace {
constexpr u32 RetainCarry = UINT32_MAX;     // loaded before the region: use live state
constexpr u32 RetainFixed = UINT32_MAX - 1; // indexed load from memory: unchanged
struct RetainPatch {
  u32 uniformOffset;
  u32 source;
  u8 slot;
  bool normal;
};
struct RetainState {
  bool valid = false;
  const u8* base = nullptr;
  u32 size = 0;
  std::array<u32, MaxPnMtx> pos{};
  std::array<u32, MaxPnMtx> nrm{};
  std::vector<RetainPatch> patches;
  std::vector<std::pair<u32, u32>> masked;
  std::vector<u8> stream;
  std::vector<u8> mask; // 0xFF over the matrix payloads, else 0; same size as stream
  std::vector<u8> state;
  std::vector<u32> hooks; // [MP6] hook offsets the recording was made with
  uint64_t depthSerialDelta = 0;
  uint64_t reused = 0, recorded = 0, refused = 0, streamMismatch = 0, stateMismatch = 0, gfxMismatch = 0;
};
RetainState sRetain;

const u8* retain_state_begin() { return reinterpret_cast<const u8*>(&g_gxState.currentPnMtx); }
size_t retain_state_size() {
  return static_cast<size_t>(reinterpret_cast<const u8*>(&g_gxState + 1) - retain_state_begin());
}

void retain_note_xf(u32 addr, const u8* payload, u32 count) {
  if (sRetain.base == nullptr || payload < sRetain.base || payload >= sRetain.base + sRetain.size) return;
  const auto offset = static_cast<u32>(payload - sRetain.base);
  if (addr < 0x78 && addr % 12 == 0 && count == 12) {
    sRetain.pos[addr / 12] = offset;
    sRetain.masked.emplace_back(offset, count * 4);
  } else if (addr >= 0x400 && addr < 0x45A && (addr - 0x400) % 9 == 0 && count == 9) {
    sRetain.nrm[(addr - 0x400) / 9] = offset;
    sRetain.masked.emplace_back(offset, count * 4);
  }
}

void retain_note_indexed(u32 addr) {
  if (addr < 0x78) {
    sRetain.pos[addr / 12] = RetainFixed;
  } else if (addr >= 0x400 && addr < 0x45A) {
    sRetain.nrm[(addr - 0x400) / 9] = RetainFixed;
  }
}

f32 retain_read_f32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, sizeof(v));
  v = bswap(v);
  f32 f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

// Writes the matrix exactly as copy_xf_data stores it.
void retain_write_mtx(Mat3x4<float>& mtx, const u8* payload, bool normal) {
  f32* flat = reinterpret_cast<f32*>(&mtx);
  if (normal) {
    for (u32 i = 0; i < 9; ++i) flat[(i / 3) * 4 + i % 3] = retain_read_f32(payload + i * 4);
  } else {
    for (u32 i = 0; i < 12; ++i) flat[i] = retain_read_f32(payload + i * 4);
  }
}
} // namespace

void retained_note_matrix(size_t uniformOffset, u32 slot, bool normal) noexcept {
  if (slot >= MaxPnMtx) return;
  const u32 source = normal ? sRetain.nrm[slot] : sRetain.pos[slot];
  if (source == RetainFixed) return;
  sRetain.patches.push_back({static_cast<u32>(uniformOffset), source, static_cast<u8>(slot), normal});
}

// Consume the whole command but submit only complete hardware primitives.
// Quads with three trailing vertices still form one triangle.
static u16 drawable_vertex_count(GXPrimitive prim, u16 count) {
  switch (prim) {
  case GX_QUADS:
    return count - (count % 4 == 3 ? 0 : count % 4);
  case GX_TRIANGLES:
    return count - count % 3;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN:
    return count >= 3 ? count : 0;
  case GX_LINES:
    return count - count % 2;
  case GX_LINESTRIP:
    return count >= 2 ? count : 0;
  case GX_POINTS:
    return count;
  default:
    FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  }
}

static u32 index_count(GXPrimitive prim, u16 vtxCount) {
  const u32 count = drawable_vertex_count(prim, vtxCount);
  if (count == 0) return 0;
  if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) return 6;
  return prim == GX_QUADS ? (count / 4) * 6 + (count % 4 == 3 ? 3 : 0) :
         prim == GX_TRIANGLES ? count : (count - 2) * 3;
}

static u32 write_idx_buffer(u16* out, GXPrimitive prim, u16 vtxStart, u16 vtxCount) {
  const u32 count = drawable_vertex_count(prim, vtxCount);
  if (count == 0) return 0;
  if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    constexpr std::array<u16, 6> quad{0, 1, 3, 3, 2, 0};
    for (const u16 index : quad) *out++ = index;
    return 6;
  }
  ASSERT(static_cast<u32>(vtxStart) + count <= 0xFFFFu, "triangle batch exceeds u16 vertex range");
  // Common single-primitive draws do not need the loop setup.
  if (count == 3) {
    *out++ = vtxStart;
    *out++ = static_cast<u16>(vtxStart + 1);
    *out++ = static_cast<u16>(vtxStart + 2);
    return 3;
  }
  if (count == 4) {
    const u16 v1 = vtxStart + 1, v2 = vtxStart + 2, v3 = vtxStart + 3;
    const std::array<u16, 6> quad{vtxStart, v1, v2,
        prim == GX_TRIANGLEFAN ? vtxStart : v2,
        prim == GX_TRIANGLESTRIP ? v1 : prim == GX_TRIANGLEFAN ? v2 : v3,
        prim == GX_QUADS ? vtxStart : v3};
    for (const u16 index : quad) *out++ = index;
    return 6;
  }
  const u32 numIndices = index_count(prim, vtxCount);
  const auto triangle = [&](u32 a, u32 b, u32 c) {
    *out++ = static_cast<u16>(vtxStart + a);
    *out++ = static_cast<u16>(vtxStart + b);
    *out++ = static_cast<u16>(vtxStart + c);
  };
  if (prim == GX_QUADS) {
    u32 v = 0;
    for (; v + 3 < count; v += 4) {
      triangle(v, v + 1, v + 2);
      triangle(v + 2, v + 3, v);
    }
    if (v + 2 < count) triangle(v, v + 1, v + 2);
  } else if (prim == GX_TRIANGLES) {
    for (u32 v = 0; v < count; ++v) *out++ = static_cast<u16>(vtxStart + v);
  } else if (prim == GX_TRIANGLEFAN) {
    for (u32 v = 2; v < count; ++v) triangle(0, v - 1, v);
  } else {
    for (u32 v = 2; v < count; ++v) {
      if (v & 1) triangle(v - 1, v - 2, v);
      else triangle(v - 2, v - 1, v);
    }
  }
  return numIndices;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
static constexpr u8 CP_CMD_NOP = GX_NOP;
static constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
static constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
static constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
static constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
static constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
static constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
static constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
static constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
static constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
static constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
static constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

// Read helpers for big/little endian
#if _MSC_VER
template <typename T>
__forceinline // Yes, this was necessary.
    inline T unaligned_load(const void* ptr) {
  return *static_cast<const __unaligned T*>(ptr);
}
#else
// NOTE(mp6-native/aurora-patches): templated on T but deliberately takes a
// `const void*`, not `const T*`. A `const T*` parameter still carries T's
// alignment requirement in the abstract machine even though memcpy itself
// doesn't care about the source's alignment -- an optimizer is entitled to
// (and, under zig cc's LLVM backend, does) lower this memcpy into a plain
// `load ..., align sizeof(T)` and trap on the genuinely-unaligned FIFO
// command-stream pointers this is called with. Confirmed via direct repro:
// zig-cc-built aurora crashed with "panic: load of misaligned address ...
// which requires 4 byte alignment" inside this function on the very first
// aurora_end_frame()/fifo::drain() call. Taking `const void*` removes the
// alignment assumption at its source instead of only hiding it one call
// deeper (the call sites below also stopped forming a typed, possibly-
// misaligned `const T*` at all, for the same reason).
template <typename T>
inline T unaligned_load(const void* ptr) {
  T copy;
  memcpy(&copy, ptr, sizeof(T));
  return copy;
}
#endif

static inline u16 read_u16(const u8* ptr, bool bigEndian) {
  const u16 val = unaligned_load<u16>(ptr);
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static inline u32 read_u32(const u8* ptr, bool bigEndian) {
  const u32 val = unaligned_load<u32>(ptr);
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static u32 bp_get(u32 reg, u32 size, u32 shift);

static GXPixelFmt decode_pixel_fmt(u32 peCtrl, u32 cmode1) {
  switch (bp_get(peCtrl, 3, 0)) {
  case 0:
    return GX_PF_RGB8_Z24;
  case 1:
    return GX_PF_RGBA6_Z24;
  case 2:
    return GX_PF_RGB565_Z16;
  case 3:
    return GX_PF_Z24;
  case 4:
    switch (bp_get(cmode1, 2, 9)) {
    case 0:
      return GX_PF_Y8;
    case 1:
      return GX_PF_U8;
    case 2:
      return GX_PF_V8;
    default:
      Log.warn("command_processor: unsupported cmode1 pixel subtype {}", bp_get(cmode1, 2, 9));
      return GX_PF_Y8;
    }
  case 5:
    return GX_PF_YUV420;
  default:
    Log.warn("command_processor: unsupported PE pixel format {}", bp_get(peCtrl, 3, 0));
    return GX_PF_RGB8_Z24;
  }
}

static inline u64 read_u64(const u8* ptr, bool bigEndian) {
  u64 loaded;
  // Unaligned-safe load
  memcpy(&loaded, ptr, sizeof(u64));

  if (bigEndian) {
    return bswap(loaded);
  }

  return loaded;
}

struct TexBpRegMapping {
  u8 texMapId;
  enum class Kind : uint8_t { Mode0, Mode1, Image0, Image1, Image2, Image3, Tlut } kind;
};

static std::optional<TexBpRegMapping> decode_tex_bp_reg(u32 regId) {
  if (!((regId >= 0x80u && regId <= 0x9Bu) ||
        (regId >= 0xA0u && regId <= 0xBBu))) {
    return std::nullopt;
  }
  const u32 local = regId - 0x80u;
  const u32 bank = (local >> 5) & 1u;
  const u32 low5 = local & 0x1Fu;
  const u32 kind = low5 >> 2;
  const u32 slot = (low5 & 3u) + (bank << 2);
  return TexBpRegMapping{.texMapId = static_cast<u8>(slot),
                         .kind = static_cast<TexBpRegMapping::Kind>(kind)};
}

// Helper to convert packed RGBA8 to Vec4<float>
static Vec4<float> unpack_color(u32 packed) {
  return {
      static_cast<float>(packed >> 24 & 0xFF) / 255.f,
      static_cast<float>(packed >> 16 & 0xFF) / 255.f,
      static_cast<float>(packed >> 8 & 0xFF) / 255.f,
      static_cast<float>(packed & 0xFF) / 255.f,
  };
}

static inline f32 read_f32(const u8* ptr, bool bigEndian) {
  u32 bits = read_u32(ptr, bigEndian);
  f32 val;
  std::memcpy(&val, &bits, sizeof(val));
  return val;
}

static bool copy_xf_data(u32 addr, const u8* data, u32 len, bool bigEndian) {
  if (addr < 0x78) {
    // Position matrices (0x0000 - 0x0077)
    u32 mtxIdx = addr / 12;
    u32 startOffset = addr % 12;
    // We only support full writes to matrices
    CHECK(mtxIdx < MaxPnMtx, "XF: PosMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && len == 12, "XF: PosMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].pos;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
  } else if (addr < 0x0F0) {
    // Texture matrices (0x078-0x0EF)
    u32 texBase = addr - 0x078;
    u32 mtxIdx = texBase / 12;
    u32 startOffset = texBase % 12;
    CHECK(mtxIdx < MaxTexMtx, "XF TexMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && (len == 8 || len == 12), "XF TexMtx sub-copy unsupported: offs={}, len={}", startOffset,
          len);

    // Determine if 2x4 or 3x4 from count
    auto& mtx = g_gxState.texMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x400 && addr < 0x45A) {
    // Normal matrices (0x400-0x459)
    u32 nrmBase = addr - 0x400;
    u32 mtxIdx = nrmBase / 9;
    u32 startOffset = nrmBase % 9;
    // We only support full writes to matrices
    CHECK(mtxIdx < MaxPnMtx, "XF: NrmMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && len == 9, "XF: NrmMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].nrm;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      u32 xfIdx = i;
      u32 row = xfIdx / 3;
      u32 col = xfIdx % 3;
      if (row < 3) {
        flat[row * 4 + col] = read_f32(data + i * 4, bigEndian);
      }
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x500 && addr < 0x5F0) {
    // Post-transform texture matrices (0x500-0x5EF)
    u32 ptBase = addr - 0x500;
    u32 mtxIdx = ptBase / 12;
    u32 startOffset = ptBase % 12;
    CHECK(mtxIdx < MaxPTTexMtx, "XF: PTTexMtx copy oob? Should never happen; mtxIdx={}", mtxIdx);
    CHECK(startOffset == 0 && len == 12, "XF: PTTexMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.ptTexMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[startOffset + i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x600 && addr < 0x680) {
    // Lights (0x600-0x67F) - 8 lights, 16 values each
    u32 lightBase = addr - 0x600;
    u32 lightIdx = lightBase / 0x10;
    u32 startOffset = lightBase % 0x10;
    CHECK(lightIdx < 8, "XF: Light copy oob? Should never happen; lightIdx={}", lightIdx);
    CHECK(startOffset + len <= 0x10, "XF: Light copy that crosses across light boundaries unsupported: offs={}, len={}",
          startOffset, len);
    auto& light = g_gxState.lights[lightIdx];
    for (u32 i = 0; i < len; i++) {
      u32 field = startOffset + i;
      f32 val = read_f32(data + i * 4, bigEndian);
      u32 ival = read_u32(data + i * 4, bigEndian);
      switch (field) {
      case 3: // Color (packed u32)
        light.color = unpack_color(ival);
        break;
      case 4:
        light.cosAtt[0] = val;
        break; // a0
      case 5:
        light.cosAtt[1] = val;
        break; // a1
      case 6:
        light.cosAtt[2] = val;
        break; // a2
      case 7:
        light.distAtt[0] = val;
        break; // k0
      case 8:
        light.distAtt[1] = val;
        break; // k1
      case 9:
        light.distAtt[2] = val;
        break; // k2
      case 10:
        light.pos[0] = val;
        break; // px
      case 11:
        light.pos[1] = val;
        break; // py
      case 12:
        light.pos[2] = val;
        break; // pz
      case 13:
        light.dir[0] = val;
        break; // nx
      case 14:
        light.dir[1] = val;
        break; // ny
      case 15:
        light.dir[2] = val;
        break; // nz
      default:
        break; // padding (0-2)
      }
    }
    g_gxState.stateDirty = true;
    return true;
  }
  return false;
}

// Forward declarations for register handlers
static void handle_bp(u32 value, bool bigEndian);
static void handle_cp(u8 addr, u32 value, bool bigEndian);
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian);
static void handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian);
static void handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian);

static void invalidate_vertex_cache() {
  // Uploaded ranges are snapshots of mutable CPU arrays. An explicit GX
  // invalidation must retire them before another draw can use those arrays,
  // even if their addresses and layouts have not changed.
  for (auto& array : g_gxState.arrays) {
    array.cachedRange = {};
  }
  mark_pipeline_state_dirty(); // Never merge across old/new array snapshots.
}

static u32 skip_nop_padding(const u8* data, u32 pos, u32 size) {
  while (size - pos >= sizeof(std::uint64_t)) {
    std::uint64_t word;
    std::memcpy(&word, data + pos, sizeof(word));
    if (word & UINT64_C(0xf8f8f8f8f8f8f8f8)) break;
    pos += sizeof(word);
  }
  while (pos < size && (data[pos] & 0xf8) == 0) ++pos;
  return pos;
}

void process(const u8* data, u32 size, bool bigEndian) {
  ZoneScoped;
  u32 pos = 0;

  while (pos < size) {
    u8 cmd = data[pos++];
    u8 opcode = cmd & CP_OPCODE_MASK;
    // Log.warn("Processing opcode {:02x} at pos {} (size {})", opcode, pos - 1, size);

    switch (opcode) {
    case CP_CMD_NOP:
      pos = skip_nop_padding(data, pos, size);
      continue;

    case CP_CMD_LOAD_BP_REG: {
      CHECK(pos + 4 <= size, "BP reg read overrun");
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_bp(value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      CHECK(pos + 5 <= size, "CP reg read overrun");
      u8 addr = data[pos++];
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_cp(addr, value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      handle_xf(data, pos, size, bigEndian);
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      // Indexed XF load: 4 bytes of data
      CHECK(pos + 4 <= size, "indexed XF read overrun");
      u32 arrayType = GX_POS_MTX_ARRAY + (opcode - (CP_CMD_LOAD_INDX_A / 0x08));
      u8 srcArrayIdx = data[pos++];
      auto const& array = g_gxState.arrays[arrayType];
      u8* srcData = ((u8*)array.data) + srcArrayIdx * array.stride;
      u16 addrLen = read_u16(data + pos, bigEndian);
      u16 len = (addrLen >> 12) + 1;
      u16 dstAddr = addrLen & 0x0FFF;
      if (g_retainRecording) retain_note_indexed(dstAddr);
      if (!copy_xf_data(dstAddr, srcData, len, bigEndian)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      pos += 4;
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      CHECK(pos + 8 <= size, "call DL read overrun");
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      pos += 8;
      break;
    }

    case CP_CMD_INVAL_VTX: {
      invalidate_vertex_cache();
      break;
    }

    case GX_AURORA: {
      handle_aurora(data, pos, size, bigEndian);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, data, pos, size, bigEndian);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, data, pos, size, bigEndian);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          u32 dumpStart = (pos > 17) ? pos - 17 : 0;
          u32 dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (u32 i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, pos - 1);
      }
      break;
    }
  }
}

// Helper to extract bit fields from a 32-bit register
inline static u32 bp_get(u32 reg, u32 size, u32 shift) { return reg >> shift & (1u << size) - 1; }

// Raw-register equality is not enough when another command can change the
// decoded field. Keep BP's raw words intact: selective writes still merge them.
static inline bool repeated_bp_register(u32 reg, u32 value) {
  if (g_gxState.bpRegCache[reg] != value) return false;
  if (reg == 0x00) {
    // XF also writes numChans / numTexGens. The stage check admits the first
    // zero GENMODE write (raw zero means one TEV stage, not zero stages).
    return g_gxState.numChans == bp_get(value, 3, 4) &&
           g_gxState.numTexGens == bp_get(value, 4, 0) &&
           g_gxState.numTevStages == bp_get(value, 4, 10) + 1;
  }
  return reg != 0x52 && reg != 0x65; // Copy and palette-load commands must execute.
}

static inline bool repeated_xf_register(u32 reg, u32 value) {
  if (reg > 0x19) return false; // Matrix memory, viewport and projection are not scalar cache entries.
  const u32 mask = 1u << reg;
  if ((g_gxState.xfRegCacheValid & mask) && g_gxState.xfRegCache[reg] == value) {
    if (reg == 0x09) return g_gxState.numChans == value; // Also written by BP GENMODE.
    if (reg == 0x18) return g_gxState.currentPnMtx == bp_get(value, 6, 0) / 3; // Also written by CP.
    return true;
  }
  g_gxState.xfRegCache[reg] = value;
  g_gxState.xfRegCacheValid |= mask;
  return false; // First zero writes must decode nonzero defaults.
}

// BP register handler - decodes BP (RAS/pixel engine) register writes and updates g_gxState
static void handle_bp(u32 value, bool bigEndian) {
  u32 regId = (value >> 24) & 0xFF;
  // Mask off the register ID from the value for field extraction
  // (the regId is stored in bits 24-31, data is in bits 0-23)

  if (regId == 0xFE) {
    g_gxState.bpRegCache[regId] = value & 0x00FFFFFF;
    return;
  } else {
    u32 ssMask = g_gxState.bpRegCache[0xFE];
    g_gxState.bpRegCache[0xFE] = 0x00FFFFFF;
    const u32 merged = (g_gxState.bpRegCache[regId] & ~ssMask) | (value & ssMask);
    value = (regId << 24) | (merged & 0x00FFFFFF);
    if (repeated_bp_register(regId, value))
      return;
    g_gxState.bpRegCache[regId] = value;
  }

  // TEV color combiner stages (0xC0, 0xC2, 0xC4, ... 0xDE)
  if (regId >= 0xC0 && regId <= 0xDE && (regId & 1) == 0) {
    u32 stage = (regId - 0xC0) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.colorPass.d = static_cast<GXTevColorArg>(bp_get(value, 4, 0));
      s.colorPass.c = static_cast<GXTevColorArg>(bp_get(value, 4, 4));
      s.colorPass.b = static_cast<GXTevColorArg>(bp_get(value, 4, 8));
      s.colorPass.a = static_cast<GXTevColorArg>(bp_get(value, 4, 12));
      s.colorOp.clamp = bp_get(value, 1, 19) != 0;
      s.colorOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        // Bias==3 means compare mode: reconstruct GXTevOp enum (8 + 3-bit hw value)
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.colorOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.colorOp.bias = GX_TB_ZERO;
        s.colorOp.scale = GX_CS_SCALE_1;
      } else {
        // Normal mode: bit18 is op (0=ADD, 1=SUB), bits16-17 is bias, bits20-21 is scale
        s.colorOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.colorOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.colorOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  // TEV alpha combiner stages (0xC1, 0xC3, 0xC5, ... 0xDF)
  if (regId >= 0xC1 && regId <= 0xDF && (regId & 1) == 1) {
    u32 stage = (regId - 0xC1) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.tevSwapRas = static_cast<GXTevSwapSel>(bp_get(value, 2, 0));
      s.tevSwapTex = static_cast<GXTevSwapSel>(bp_get(value, 2, 2));
      s.alphaPass.d = static_cast<GXTevAlphaArg>(bp_get(value, 3, 4));
      s.alphaPass.c = static_cast<GXTevAlphaArg>(bp_get(value, 3, 7));
      s.alphaPass.b = static_cast<GXTevAlphaArg>(bp_get(value, 3, 10));
      s.alphaPass.a = static_cast<GXTevAlphaArg>(bp_get(value, 3, 13));
      s.alphaOp.clamp = bp_get(value, 1, 19) != 0;
      s.alphaOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.alphaOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.alphaOp.bias = GX_TB_ZERO;
        s.alphaOp.scale = GX_CS_SCALE_1;
      } else {
        s.alphaOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.alphaOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.alphaOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  switch (regId) {
  // genMode (0x00)
  case 0x00: {
    g_gxState.numTexGens = bp_get(value, 4, 0);
    g_gxState.numChans = bp_get(value, 3, 4);
    g_gxState.numTevStages = bp_get(value, 4, 10) + 1;
    u32 hwCull = bp_get(value, 2, 14);
    // Swap front/back to match GX convention
    switch (hwCull) {
    case GX_CULL_FRONT:
      g_gxState.cullMode = GX_CULL_BACK;
      break;
    case GX_CULL_BACK:
      g_gxState.cullMode = GX_CULL_FRONT;
      break;
    default:
      g_gxState.cullMode = static_cast<GXCullMode>(hwCull);
      break;
    }
    g_gxState.numIndStages = bp_get(value, 3, 16);
    mark_pipeline_state_dirty();
    break;
  }

  // BP mask (0x0F) - internal, applies to next BP write
  case 0x0F:
#ifndef NDEBUG
    Log.debug("BP mask set to {:06x}, but selective updates are not implemented", value & 0xFFFFFF);
#endif
    break;

  // TEV indirect stages (0x10-0x1F)
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1A:
  case 0x1B:
  case 0x1C:
  case 0x1D:
  case 0x1E:
  case 0x1F: {
    u32 stage = regId - 0x10;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.indTexStage = static_cast<GXIndTexStageID>(bp_get(value, 2, 0));
      s.indTexFormat = static_cast<GXIndTexFormat>(bp_get(value, 2, 2));
      s.indTexBiasSel = static_cast<GXIndTexBiasSel>(bp_get(value, 3, 4));
      s.indTexAlphaSel = static_cast<GXIndTexAlphaSel>(bp_get(value, 2, 7));
      s.indTexMtxId = static_cast<GXIndTexMtxID>(bp_get(value, 4, 9));
      s.indTexWrapS = static_cast<GXIndTexWrap>(bp_get(value, 3, 13));
      s.indTexWrapT = static_cast<GXIndTexWrap>(bp_get(value, 3, 16));
      s.indTexUseOrigLOD = bp_get(value, 1, 19) != 0;
      s.indTexAddPrev = bp_get(value, 1, 20) != 0;
      mark_pipeline_state_dirty();
    }
    break;
  }

  // Scissor registers (0x20, 0x21)
  case 0x20:
  case 0x21: {
    const u32 scis0 = g_gxState.bpRegCache[0x20];
    const u32 scis1 = g_gxState.bpRegCache[0x21];
    const int32_t tp = static_cast<int32_t>(bp_get(scis0, 11, 0)) - 342;
    const int32_t lf = static_cast<int32_t>(bp_get(scis0, 11, 12)) - 342;
    const int32_t bm = static_cast<int32_t>(bp_get(scis1, 11, 0)) - 342;
    const int32_t rt = static_cast<int32_t>(bp_get(scis1, 11, 12)) - 342;
    const int32_t wd = std::max(rt - lf + 1, 0);
    const int32_t ht = std::max(bm - tp + 1, 0);
    set_logical_scissor({lf, tp, wd, ht});
    break;
  }

  // Line/point size (0x22)
  case 0x22: {
    g_gxState.lineWidth = static_cast<u8>(bp_get(value, 8, 0));
    g_gxState.pointSize = static_cast<u8>(bp_get(value, 8, 8));
    g_gxState.lineTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 16));
    g_gxState.pointTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 19));
    g_gxState.lineHalfAspect = bp_get(value, 1, 22) != 0;
    mark_pipeline_state_dirty();
    break;
  }

  // Indirect texture scale (0x25, 0x26)
  case 0x25: {
    if (MaxIndStages > 0) {
      g_gxState.indStages[0].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[0].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 1) {
      g_gxState.indStages[1].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[1].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }
  case 0x26: {
    if (MaxIndStages > 2) {
      g_gxState.indStages[2].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[2].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 3) {
      g_gxState.indStages[3].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[3].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Indirect texture reference (0x27)
  case 0x27: {
    for (u32 i = 0; i < MaxIndStages; i++) {
      g_gxState.indStages[i].texMapId = static_cast<GXTexMapID>(bp_get(value, 3, i * 6));
      g_gxState.indStages[i].texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, i * 6 + 3));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // TEV order / tref (0x28-0x2F) - 2 stages per register
  case 0x28:
  case 0x29:
  case 0x2A:
  case 0x2B:
  case 0x2C:
  case 0x2D:
  case 0x2E:
  case 0x2F: {
    u32 idx = regId - 0x28;
    u32 stage0 = idx * 2;
    u32 stage1 = idx * 2 + 1;

    // Channel ID reverse mapping from hardware to GX
    static const GXChannelID r2c[] = {GX_COLOR0A0, GX_COLOR1A1,   GX_COLOR0A0,    GX_COLOR1A1,
                                      GX_COLOR0A0, GX_ALPHA_BUMP, GX_ALPHA_BUMPN, GX_COLOR_ZERO};

    if (stage0 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage0];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 0));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 3));
      // bit 6 = tex enable
      if (!bp_get(value, 1, 6)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 7);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    if (stage1 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage1];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 12));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 15));
      if (!bp_get(value, 1, 18)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 19);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Z mode (0x40)
  case 0x40: {
    g_gxState.depthCompare = bp_get(value, 1, 0) != 0;
    g_gxState.depthFunc = static_cast<GXCompare>(bp_get(value, 3, 1));
    g_gxState.depthUpdate = bp_get(value, 1, 4) != 0;
    mark_pipeline_state_dirty();
    break;
  }

  // Blend mode / cmode0 (0x41)
  case 0x41: {
    bool blendEn = bp_get(value, 1, 0) != 0;
    bool logicEn = bp_get(value, 1, 1) != 0;
    bool dither = bp_get(value, 1, 2) != 0;
    g_gxState.colorUpdate = bp_get(value, 1, 3) != 0;
    g_gxState.alphaUpdate = bp_get(value, 1, 4) != 0;
    g_gxState.blendFacDst = static_cast<GXBlendFactor>(bp_get(value, 3, 5));
    g_gxState.blendFacSrc = static_cast<GXBlendFactor>(bp_get(value, 3, 8));
    bool subtract = bp_get(value, 1, 11) != 0;
    g_gxState.blendOp = static_cast<GXLogicOp>(bp_get(value, 4, 12));

    if (subtract) {
      g_gxState.blendMode = GX_BM_SUBTRACT;
    } else if (blendEn) {
      g_gxState.blendMode = GX_BM_BLEND;
    } else if (logicEn) {
      g_gxState.blendMode = GX_BM_LOGIC;
    } else {
      g_gxState.blendMode = GX_BM_NONE;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Dst alpha / cmode1 (0x42)
  case 0x42: {
    u8 alpha = bp_get(value, 8, 0);
    bool enabled = bp_get(value, 1, 8) != 0;
    g_gxState.dstAlpha = enabled ? alpha : UINT32_MAX;
    g_gxState.pixelFmt = decode_pixel_fmt(g_gxState.bpRegCache[0x43], value);
    mark_pipeline_state_dirty();
    break;
  }

  // PE control (0x43) - pixel format, z format, zcomp location
  case 0x43: {
    g_gxState.pixelFmt = decode_pixel_fmt(value, g_gxState.bpRegCache[0x42]);
    g_gxState.zFmt = static_cast<GXZFmt16>(bp_get(value, 3, 3));
    g_gxState.zCompLocBeforeTex = bp_get(value, 1, 6) != 0;
    mark_pipeline_state_dirty();
    break;
  }

  // Texture copy
  case 0x52: {
    const bool clear = bp_get(value, 1, 11) != 0;
    if (bp_get(value, 1, 14) != 0) {
      Log.warn("STUB: display copy is not implemented");
    } else {
      copy_tex(g_gxState.texCopyDest, clear);
    }
    break;
  }

  // TLUT load address / execute (0x64, 0x65)
  case 0x64:
    break;
  case 0x65: {
    const auto idx = bp_get(value, 10, 0);
    if (idx < MaxTluts) {
      auto& slot = g_gxState.loadedTluts[idx];
      slot.loadTlut0 = g_gxState.bpRegCache[0x64];
      slot.numEntries = static_cast<u16>(bp_get(value, 10, 10) + 1);
    }
    break;
  }

  // Alpha compare (0xF3)
  case 0xF3: {
    g_gxState.alphaCompare.ref0 = bp_get(value, 8, 0);
    g_gxState.alphaCompare.ref1 = bp_get(value, 8, 8);
    g_gxState.alphaCompare.comp0 = static_cast<GXCompare>(bp_get(value, 3, 16));
    g_gxState.alphaCompare.comp1 = static_cast<GXCompare>(bp_get(value, 3, 19));
    g_gxState.alphaCompare.op = static_cast<GXAlphaOp>(bp_get(value, 2, 22));
    mark_pipeline_state_dirty();
    break;
  }

  // TEV K color/alpha select (0xF6-0xFD)
  case 0xF6:
  case 0xF7:
  case 0xF8:
  case 0xF9:
  case 0xFA:
  case 0xFB:
  case 0xFC:
  case 0xFD: {
    u32 kselIdx = regId - 0xF6;
    // Swap table entries (packed into pairs of ksel registers)
    if (kselIdx < MaxTevSwap * 2) {
      u32 swapIdx = kselIdx / 2;
      if (kselIdx & 1) {
        g_gxState.tevSwapTable[swapIdx].blue = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].alpha = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      } else {
        g_gxState.tevSwapTable[swapIdx].red = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].green = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      }
    }
    // K color/alpha selection for 2 stages per register
    u32 stage0 = kselIdx * 2;
    u32 stage1 = kselIdx * 2 + 1;
    if (stage0 < MaxTevStages) {
      g_gxState.tevStages[stage0].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 4));
      g_gxState.tevStages[stage0].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 9));
    }
    if (stage1 < MaxTevStages) {
      g_gxState.tevStages[stage1].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 14));
      g_gxState.tevStages[stage1].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 19));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Fog A/B parameters (0xEE-0xF0)
  // FOG0 (0xEE): A parameter - sign(1)|exp(8)|mantissa(11) partial IEEE 754 float
  case 0xEE: {
    g_gxState.fog.fog0Raw = value;
    // Reconstruct A = a_encoded * 2^b_s
    u32 a_mant = bp_get(value, 11, 0);
    u32 a_exp = bp_get(value, 8, 11);
    u32 a_sign = bp_get(value, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    float a_encoded;
    std::memcpy(&a_encoded, &a_bits, sizeof(a_encoded));
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    g_gxState.fog.a = std::ldexp(a_encoded, static_cast<int>(b_s));
    mark_pipeline_state_dirty();
    break;
  }
  // FOG1 (0xEF): B mantissa (24-bit)
  case 0xEF: {
    g_gxState.fog.fog1Raw = value;
    u32 b_m = bp_get(value, 24, 0);
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    float B_mant = static_cast<float>(b_m) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    mark_pipeline_state_dirty();
    break;
  }
  // FOG2 (0xF0): B shift/exponent (5-bit)
  case 0xF0: {
    g_gxState.fog.fog2Raw = value;
    u32 b_s = bp_get(value, 5, 0);
    // Recompute A with updated b_s
    u32 a_mant = bp_get(g_gxState.fog.fog0Raw, 11, 0);
    u32 a_exp = bp_get(g_gxState.fog.fog0Raw, 8, 11);
    u32 a_sign = bp_get(g_gxState.fog.fog0Raw, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    float a_encoded;
    std::memcpy(&a_encoded, &a_bits, sizeof(a_encoded));
    g_gxState.fog.a = std::ldexp(a_encoded, static_cast<int>(b_s));
    // Recompute B with updated b_s
    u32 b_m = bp_get(g_gxState.fog.fog1Raw, 24, 0);
    float B_mant = static_cast<float>(b_m) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    mark_pipeline_state_dirty();
    break;
  }

  // Fog type + C parameter from FOG3 (0xF1)
  case 0xF1: {
    GXFogType fogType = static_cast<GXFogType>(bp_get(value, 3, 21));
    g_gxState.fog.type = fogType;
    // Decode C parameter (same partial float encoding as A)
    u32 c_mant = bp_get(value, 11, 0);
    u32 c_exp = bp_get(value, 8, 11);
    u32 c_sign = bp_get(value, 1, 19);
    u32 c_bits = (c_sign << 31) | (c_exp << 23) | (c_mant << 12);
    std::memcpy(&g_gxState.fog.c, &c_bits, sizeof(g_gxState.fog.c));
    mark_pipeline_state_dirty();
    break;
  }

  // Fog color from FOGCLR (0xF2)
  case 0xF2: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    u8 r = bp_get(value, 8, 16);
    g_gxState.fog.color = {
        static_cast<float>(r) / 255.f,
        static_cast<float>(g) / 255.f,
        static_cast<float>(b) / 255.f,
        1.f,
    };
    mark_pipeline_state_dirty();
    break;
  }

  // TEV color registers / K color registers (0xE0-0xE7)
  // RA registers: 0xE0, 0xE2, 0xE4, 0xE6 (even)
  // BG registers: 0xE1, 0xE3, 0xE5, 0xE7 (odd)
  // Bit 23 distinguishes: 0 = TEV color register, 1 = K color register
  case 0xE0:
  case 0xE1:
  case 0xE2:
  case 0xE3:
  case 0xE4:
  case 0xE5:
  case 0xE6:
  case 0xE7: {
    u32 idx = (regId - 0xE0) / 2;
    bool isRA = (regId & 1) == 0;
    bool isKColor = bp_get(value, 1, 23) != 0;

    if (isKColor) {
      // K color register (8-bit components)
      if (idx < GX_MAX_KCOLOR) {
        auto& kc = g_gxState.kcolors[idx];
        if (isRA) {
          kc[0] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // R
          kc[3] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // A
        } else {
          kc[2] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // B
          kc[1] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // G
        }
        mark_pipeline_state_dirty();
      }
    } else {
      // TEV color register (11-bit signed components)
      if (idx < MaxTevRegs) {
        auto& cr = g_gxState.colorRegs[idx];
        if (isRA) {
          // 11-bit signed: sign-extend from 11 bits
          s32 r = bp_get(value, 11, 0);
          if (r & 0x400)
            r |= ~0x7FF; // sign extend
          s32 a = bp_get(value, 11, 12);
          if (a & 0x400)
            a |= ~0x7FF;
          cr[0] = static_cast<float>(r) / 255.f;
          cr[3] = static_cast<float>(a) / 255.f;
        } else {
          s32 b = bp_get(value, 11, 0);
          if (b & 0x400)
            b |= ~0x7FF;
          s32 g = bp_get(value, 11, 12);
          if (g & 0x400)
            g |= ~0x7FF;
          cr[2] = static_cast<float>(b) / 255.f;
          cr[1] = static_cast<float>(g) / 255.f;
        }
        mark_pipeline_state_dirty();
      }
    }
    break;
  }

  // Indirect texture matrices (0x06-0x0E)
  // Each matrix uses 3 consecutive registers (one per row of the 3x2 matrix).
  // Matrix 0: 0x06-0x08, Matrix 1: 0x09-0x0B, Matrix 2: 0x0C-0x0E
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x0E: {
    u32 idx = (regId - 0x06) / 3;    // matrix index (0-2)
    u32 column = (regId - 0x06) % 3; // column index (0-2)
    auto& info = g_gxState.indTexMtxs[idx];

    // Decode one packed matrix column: [m[0][column], m[1][column]].
    s32 col0 = bp_get(value, 11, 0);
    if (col0 & 0x400)
      col0 |= ~0x7FF; // sign-extend from 11 bits
    s32 col1 = bp_get(value, 11, 11);
    if (col1 & 0x400)
      col1 |= ~0x7FF;

    auto& packedColumn = column == 0 ? info.mtx.m0 : (column == 1 ? info.mtx.m1 : info.mtx.m2);
    packedColumn.x = static_cast<float>(col0) / 1024.0f;
    packedColumn.y = static_cast<float>(col1) / 1024.0f;

    // Accumulate the indirect matrix scale exponent. The SDK writes two bits per column, but
    // the hardware appears to ignore the top bit from the third column, leaving an effective
    // 5-bit value for adjScale = scaleExp + 17.
    u32 scaleBits = bp_get(value, 2, 22);
    u32 shift = column * 2;
    if (column == 2) {
      info.adjScaleRaw = (info.adjScaleRaw & ~(1u << shift)) | ((scaleBits & 1u) << shift);
    } else {
      info.adjScaleRaw = (info.adjScaleRaw & ~(3u << shift)) | (scaleBits << shift);
    }
    info.scaleExp = static_cast<s8>(info.adjScaleRaw) - 17;

    mark_pipeline_state_dirty();
    break;
  }

  // SU texture coordinate scale registers (0x30-0x3F)
  // Even registers (suTs0): S-axis scale, bias, cyl wrap, line/point offset
  // Odd registers (suTs1): T-axis scale, bias, cyl wrap
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
  case 0x38:
  case 0x39:
  case 0x3A:
  case 0x3B:
  case 0x3C:
  case 0x3D:
  case 0x3E:
  case 0x3F: {
    u32 coordIdx = (regId - 0x30) / 2;
    bool isT = (regId & 1) != 0;
    auto& tcs = g_gxState.texCoordScales[coordIdx];
    if (isT) {
      tcs.scaleT = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasT = bp_get(value, 1, 16) != 0;
      tcs.cylWrapT = bp_get(value, 1, 17) != 0;
    } else {
      tcs.scaleS = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasS = bp_get(value, 1, 16) != 0;
      tcs.cylWrapS = bp_get(value, 1, 17) != 0;
      tcs.lineOffset = bp_get(value, 1, 18) != 0;
      tcs.pointOffset = bp_get(value, 1, 19) != 0;
    }
    g_gxState.stateDirty = true;
    break;
  }

  // Copy clear color (0x4F-0x50) and depth (0x51)
  case 0x4F: {
    u8 r = bp_get(value, 8, 0);
    u8 a = bp_get(value, 8, 8);
    g_gxState.clearColor[0] = static_cast<float>(r) / 255.f;
    g_gxState.clearColor[3] = static_cast<float>(a) / 255.f;
    mark_pipeline_state_dirty();
    break;
  }
  case 0x50: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    g_gxState.clearColor[2] = static_cast<float>(b) / 255.f;
    g_gxState.clearColor[1] = static_cast<float>(g) / 255.f;
    mark_pipeline_state_dirty();
    break;
  }
  case 0x51: {
    g_gxState.clearDepth = bp_get(value, 24, 0);
    mark_pipeline_state_dirty();
    break;
  }

  default:
    if (const auto mapping = decode_tex_bp_reg(regId); mapping.has_value()) {
      auto& slot = g_gxState.loadedTextures[mapping->texMapId];
      switch (mapping->kind) {
      // The resolved image fast path keys on image ID/version, not sampler
      // state. Refresh the bound sampler words here, only on changed BP writes.
      // Already-recorded draws retain their immutable sampler handles.
      case TexBpRegMapping::Kind::Mode0:
        slot.mode0 = value;
        g_gxState.textures[mapping->texMapId].texObj.mode0 = value;
        break;
      case TexBpRegMapping::Kind::Mode1:
        slot.mode1 = value;
        g_gxState.textures[mapping->texMapId].texObj.mode1 = value;
        break;
      case TexBpRegMapping::Kind::Image0:
        slot.image0 = value;
        slot.mWidth = 0;
        slot.mHeight = 0;
        slot.mFormat = gfx::InvalidTextureFormat;
        break;
      case TexBpRegMapping::Kind::Image3:
        slot.image3 = value;
        break;
      case TexBpRegMapping::Kind::Tlut:
        // TLUT region's TMEM offset
        break;
      case TexBpRegMapping::Kind::Image1:
      case TexBpRegMapping::Kind::Image2:
        // GXTexRegion regs
        break;
      }
      g_gxState.stateDirty = true;
    } else {
#ifndef NDEBUG
      Log.debug("Unhandled BP register 0x{:02X} (value 0x{:06X})", regId, value & 0xFFFFFF);
#endif
    }
    break;
  }
}

// Only these pure CP registers own vtxDesc/vtxFmts. Matrix registers also
// have XF writers, and array commands can refer to changed data at the same
// address, so neither belongs in this cache. Keep the validity with GXState:
// a first zero write must still decode, and a state reset resets the cache.
static inline bool repeated_vertex_format_register(u32 index, u32 value) {
  const u32 mask = 1u << index;
  if ((g_gxState.vertexFormatRegsValid & mask) && g_gxState.vertexFormatRegs[index] == value)
    return true;
  g_gxState.vertexFormatRegs[index] = value;
  g_gxState.vertexFormatRegsValid |= mask;
  return false;
}

// CP register handler - decodes CP register writes and updates g_gxState
static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  switch (addr) {
  // VCD low (0x50)
  case 0x50: {
    if (repeated_vertex_format_register(0, value)) return;
    auto& vd = g_gxState.vtxDesc;
    vd[GX_VA_PNMTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 0));
    vd[GX_VA_TEX0MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 1));
    vd[GX_VA_TEX1MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 2));
    vd[GX_VA_TEX2MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 3));
    vd[GX_VA_TEX3MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 4));
    vd[GX_VA_TEX4MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 5));
    vd[GX_VA_TEX5MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 6));
    vd[GX_VA_TEX6MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 7));
    vd[GX_VA_TEX7MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 8));
    vd[GX_VA_POS] = static_cast<GXAttrType>(bp_get(value, 2, 9));
    vd[GX_VA_NRM] = static_cast<GXAttrType>(bp_get(value, 2, 11));
    vd[GX_VA_CLR0] = static_cast<GXAttrType>(bp_get(value, 2, 13));
    vd[GX_VA_CLR1] = static_cast<GXAttrType>(bp_get(value, 2, 15));
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    break;
  }

  // VCD high (0x60)
  case 0x60: {
    if (repeated_vertex_format_register(1, value)) return;
    auto& vd = g_gxState.vtxDesc;
    vd[GX_VA_TEX0] = static_cast<GXAttrType>(bp_get(value, 2, 0));
    vd[GX_VA_TEX1] = static_cast<GXAttrType>(bp_get(value, 2, 2));
    vd[GX_VA_TEX2] = static_cast<GXAttrType>(bp_get(value, 2, 4));
    vd[GX_VA_TEX3] = static_cast<GXAttrType>(bp_get(value, 2, 6));
    vd[GX_VA_TEX4] = static_cast<GXAttrType>(bp_get(value, 2, 8));
    vd[GX_VA_TEX5] = static_cast<GXAttrType>(bp_get(value, 2, 10));
    vd[GX_VA_TEX6] = static_cast<GXAttrType>(bp_get(value, 2, 12));
    vd[GX_VA_TEX7] = static_cast<GXAttrType>(bp_get(value, 2, 14));
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    break;
  }

  // Matrix index A (0x30)
  case 0x30: {
    g_gxState.currentPnMtx = bp_get(value, 6, 0) / 3;
    g_gxState.stateDirty = true;
    break;
  }

  // Matrix index B (0x40)
  case 0x40:
    // Texture matrix indices - used for multi-matrix texgen
    break;

  default:
    // VAT A registers (0x70-0x77)
    if (addr >= 0x70 && addr <= 0x77) {
      u32 fmt = addr - 0x70;
      if (repeated_vertex_format_register(2 + fmt, value)) return;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_POS].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_POS].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_POS].frac = static_cast<u8>(bp_get(value, 5, 4));
      const auto nrm_cnt = bp_get(value, 1, 9);
      const auto nrm_nbt3 = bp_get(value, 1, 31);
      vf.attrs[GX_VA_NRM].cnt = static_cast<GXCompCnt>(nrm_nbt3 ? GX_NRM_NBT3 : (nrm_cnt ? GX_NRM_NBT : GX_NRM_XYZ));
      vf.attrs[GX_VA_NRM].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      if (vf.attrs[GX_VA_NRM].type == GX_U8 || vf.attrs[GX_VA_NRM].type == GX_S8) {
        vf.attrs[GX_VA_NRM].frac = 6;
      } else if (vf.attrs[GX_VA_NRM].type == GX_U16 || vf.attrs[GX_VA_NRM].type == GX_S16) {
        vf.attrs[GX_VA_NRM].frac = 14;
      } else {
        vf.attrs[GX_VA_NRM].frac = 0;
      }
      vf.attrs[GX_VA_CLR0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 13));
      vf.attrs[GX_VA_CLR0].type = static_cast<GXCompType>(bp_get(value, 3, 14));
      vf.attrs[GX_VA_CLR0].frac = 0;
      vf.attrs[GX_VA_CLR1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 17));
      vf.attrs[GX_VA_CLR1].type = static_cast<GXCompType>(bp_get(value, 3, 18));
      vf.attrs[GX_VA_CLR1].frac = 0;
      vf.attrs[GX_VA_TEX0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 21));
      vf.attrs[GX_VA_TEX0].type = static_cast<GXCompType>(bp_get(value, 3, 22));
      vf.attrs[GX_VA_TEX0].frac = static_cast<u8>(bp_get(value, 5, 25));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // VAT B registers (0x80-0x87)
    else if (addr >= 0x80 && addr <= 0x87) {
      u32 fmt = addr - 0x80;
      if (repeated_vertex_format_register(10 + fmt, value)) return;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_TEX1].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_TEX1].frac = static_cast<u8>(bp_get(value, 5, 4));
      vf.attrs[GX_VA_TEX2].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 9));
      vf.attrs[GX_VA_TEX2].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      vf.attrs[GX_VA_TEX2].frac = static_cast<u8>(bp_get(value, 5, 13));
      vf.attrs[GX_VA_TEX3].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 18));
      vf.attrs[GX_VA_TEX3].type = static_cast<GXCompType>(bp_get(value, 3, 19));
      vf.attrs[GX_VA_TEX3].frac = static_cast<u8>(bp_get(value, 5, 22));
      vf.attrs[GX_VA_TEX4].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 27));
      vf.attrs[GX_VA_TEX4].type = static_cast<GXCompType>(bp_get(value, 3, 28));
      // TEX4 frac is in VAT C
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // VAT C registers (0x90-0x97)
    else if (addr >= 0x90 && addr <= 0x97) {
      u32 fmt = addr - 0x90;
      if (repeated_vertex_format_register(18 + fmt, value)) return;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX4].frac = static_cast<u8>(bp_get(value, 5, 0));
      vf.attrs[GX_VA_TEX5].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 5));
      vf.attrs[GX_VA_TEX5].type = static_cast<GXCompType>(bp_get(value, 3, 6));
      vf.attrs[GX_VA_TEX5].frac = static_cast<u8>(bp_get(value, 5, 9));
      vf.attrs[GX_VA_TEX6].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 14));
      vf.attrs[GX_VA_TEX6].type = static_cast<GXCompType>(bp_get(value, 3, 15));
      vf.attrs[GX_VA_TEX6].frac = static_cast<u8>(bp_get(value, 5, 18));
      vf.attrs[GX_VA_TEX7].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 23));
      vf.attrs[GX_VA_TEX7].type = static_cast<GXCompType>(bp_get(value, 3, 24));
      vf.attrs[GX_VA_TEX7].frac = static_cast<u8>(bp_get(value, 5, 27));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // Array base addresses (0xA0-0xAF)
    else if (addr >= 0xA0 && addr <= 0xAF) {
      Log.error("CP_REG_ARRAYBASE_ID is not supported on Aurora. Use GX_AURORA_LOAD_ARRAYBASE instead.");
    }
    // Array strides (0xB0-0xBF)
    else if (addr >= 0xB0 && addr <= 0xBF) {
      u32 attrIdx = addr - 0xB0 + GX_VA_POS;
      if (attrIdx < GX_VA_MAX_ATTR) {
        auto& array = g_gxState.arrays[attrIdx];
        const auto newStride = static_cast<u8>(value);
        if (array.stride != newStride) {
          array.stride = newStride;
          mark_pipeline_state_dirty();
        }
      }
    }
    break;
  }
}

// XF register handler - decodes XF (transform unit) register writes and updates g_gxState
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {
  CHECK(pos + 4 <= size, "XF header read overrun");
  u32 header = read_u32(data + pos, bigEndian);
  pos += 4;

  u32 count = ((header >> 16) & 0xFFFF) + 1;
  u32 addr = header & 0xFFFF;
  u32 dataBytes = count * 4;
  // Log.warn("  xf: addr {:04x} count {} dataBytes {} pos {} -> {}", addr, count, dataBytes, pos, pos + dataBytes);
  CHECK(pos + dataBytes <= size, "XF data read overrun: need {} bytes at pos {}", dataBytes, pos);

  const u8* xfData = data + pos;
  if (g_retainRecording) retain_note_xf(addr, xfData, count);

  if (copy_xf_data(addr, xfData, count, bigEndian)) {
    // copy_xf_data handled everything.
  } else if (addr >= 0x1000) {
    // XF registers (0x1000+)
    u32 xfAddr = addr - 0x1000;
    for (u32 i = 0; i < count; i++) {
      u32 reg = xfAddr + i;
      u32 val = read_u32(xfData + i * 4, bigEndian);

      if (repeated_xf_register(reg, val))
        continue;

      switch (reg) {
      case 0x08:
        // XF vertex specs (numColors, numNormals, numTexCoords) - informational
        break;
      case 0x09:
        // numChans
        g_gxState.numChans = val;
        mark_pipeline_state_dirty();
        break;
      case 0x0A:
        // Ambient color 0
        g_gxState.colorChannelState[GX_COLOR0].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0B:
        // Ambient color 1
        g_gxState.colorChannelState[GX_COLOR1].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0C:
        // Material color 0
        g_gxState.colorChannelState[GX_COLOR0].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0D:
        // Material color 1
        g_gxState.colorChannelState[GX_COLOR1].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0E:
      case 0x0F:
      case 0x10:
      case 0x11: {
        // Channel control registers
        u32 chanId = reg - 0x0E;
        if (chanId < MaxColorChannels) {
          auto& chan = g_gxState.colorChannelConfig[chanId];
          chan.matSrc = static_cast<GXColorSrc>(bp_get(val, 1, 0));
          chan.lightingEnabled = bp_get(val, 1, 1) != 0;
          u32 lightsLo = bp_get(val, 4, 2);
          chan.ambSrc = static_cast<GXColorSrc>(bp_get(val, 1, 6));
          chan.diffFn = static_cast<GXDiffuseFn>(bp_get(val, 2, 7));
          // Encoding: bit 9 = (attnFn != GX_AF_SPEC), bit 10 = (attnFn != GX_AF_NONE)
          bool bit9 = bp_get(val, 1, 9) != 0;
          bool bit10 = bp_get(val, 1, 10) != 0;
          u32 lightsHi = bp_get(val, 4, 11);
          if (!bit10) {
            chan.attnFn = GX_AF_NONE;
          } else if (!bit9) {
            chan.attnFn = GX_AF_SPEC;
          } else {
            chan.attnFn = GX_AF_SPOT;
          }
          u32 lightMask = lightsLo | (lightsHi << 4);
          g_gxState.colorChannelState[chanId].lightMask = GX::LightMask{lightMask};
          mark_pipeline_state_dirty();
        }
        break;
      }
      case 0x18: {
        // Matrix index A: PnMtx + TexCoord0-3 matrix indices
        g_gxState.currentPnMtx = bp_get(val, 6, 0) / 3;
        for (u32 i = 0; i < 4 && i < MaxTexCoord; i++) {
          auto texMtx = static_cast<GXTexMtx>(bp_get(val, 6, 6 + i * 6));
          assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
          g_gxState.tcgs[i].mtx = texMtx;
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x19: {
        // Matrix index B: TexCoord4-7 matrix indices
        for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
          g_gxState.tcgs[i + 4].mtx = static_cast<GXTexMtx>(bp_get(val, 6, i * 6));
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x1A:
      case 0x1B:
      case 0x1C:
      case 0x1D:
      case 0x1E:
      case 0x1F: {
        // Viewport: sx, sy, sz, ox, oy, oz at XF 0x101A-0x101F
        u32 vpOff = reg - 0x1A;
        if (vpOff == 0 && count >= 6) {
          f32 sx = read_f32(xfData + 0, bigEndian);
          f32 sy = read_f32(xfData + 4, bigEndian);
          f32 sz = read_f32(xfData + 8, bigEndian);
          f32 ox = read_f32(xfData + 12, bigEndian);
          f32 oy = read_f32(xfData + 16, bigEndian);
          f32 oz = read_f32(xfData + 20, bigEndian);
          f32 width = sx * 2.0f;
          f32 height = -sy * 2.0f;
          set_logical_viewport({
              .left = ox - 340.0f - width / 2.0f,
              .top = oy - 340.0f - height / 2.0f,
              .width = width,
              .height = height,
              .znear = (oz - sz) / 1.6777215e7f,
              .zfar = oz / 1.6777215e7f,
          });
        }
        break;
      }
      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23:
      case 0x24:
      case 0x25:
      case 0x26: {
        // Projection: 6 params + type at XF 0x1020-0x1026
        u32 projOff = reg - 0x20;
        if (projOff == 0 && count >= 7) {
          f32 p0 = read_f32(xfData + 0, bigEndian);
          f32 p1 = read_f32(xfData + 4, bigEndian);
          f32 p2 = read_f32(xfData + 8, bigEndian);
          f32 p3 = read_f32(xfData + 12, bigEndian);
          f32 p4 = read_f32(xfData + 16, bigEndian);
          f32 p5 = read_f32(xfData + 20, bigEndian);
          u32 projType = read_u32(xfData + 24, bigEndian);
          g_gxState.projType = static_cast<GXProjectionType>(projType);
          // Reconstruct 4x4 projection matrix from 6 params
          auto& proj = g_gxState.proj;
          proj = {};
          proj.m0[0] = p0;
          proj.m1[1] = p2;
          proj.m2[2] = p4;
          proj.m2[3] = p5;
          if (projType == GX_ORTHOGRAPHIC) {
            proj.m0[3] = p1;
            proj.m1[3] = p3;
            proj.m3[3] = 1.0f;
          } else {
            proj.m0[2] = p1;
            proj.m1[2] = p3;
            proj.m3[2] = -1.0f;
          }
          g_gxState.stateDirty = true;
        }
        break;
      }
      case 0x3F:
        // numTexGens
        if (g_gxState.numTexGens != val) g_pipelineConfigDirty = true;
        g_gxState.numTexGens = val;
        g_gxState.stateDirty = true;
        break;
      default:
        // TexGen config (0x40-0x4F) and post-transform (0x50-0x5F)
        if (reg >= 0x40 && reg <= 0x4F) {
          u32 tcIdx = reg - 0x40;
          if (tcIdx < MaxTexCoord) {
            auto& tcg = g_gxState.tcgs[tcIdx];
            const auto previous = tcg;
            bool proj = bp_get(val, 1, 1) != 0;
            u32 form = bp_get(val, 1, 2);
            u32 tgType = bp_get(val, 3, 4);
            u32 srcRow = bp_get(val, 5, 7);

            if (tgType == 0) {
              tcg.type = proj ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            } else if (tgType == 1) {
              // Bump mapping: type encodes emboss light
              tcg.type = static_cast<GXTexGenType>(bp_get(val, 3, 15) + 2);
            } else if (tgType == 2 || tgType == 3) {
              tcg.type = GX_TG_SRTG;
            }
            // Emboss source texcoord (bits 12-14); 0 for non-bump types
            tcg.embossSrc = bp_get(val, 3, 12);

            // Decode source from row
            static const GXTexGenSrc rowToSrc[] = {GX_TG_POS,  GX_TG_NRM,  GX_TG_COLOR0, GX_TG_BINRM, GX_TG_TANGENT,
                                                   GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2,   GX_TG_TEX3,  GX_TG_TEX4,
                                                   GX_TG_TEX5, GX_TG_TEX6, GX_TG_TEX7};
            if (srcRow < 13) {
              tcg.src = rowToSrc[srcRow];
            }
            if (tcg != previous) g_pipelineConfigDirty = true;
            g_gxState.stateDirty = true;
          }
        } else if (reg >= 0x50 && reg <= 0x5F) {
          u32 tcIdx = reg - 0x50;
          if (tcIdx < MaxTexCoord) {
            const auto previous = g_gxState.tcgs[tcIdx];
            g_gxState.tcgs[tcIdx].postMtx = static_cast<GXPTTexMtx>(bp_get(val, 6, 0) + 64);
            g_gxState.tcgs[tcIdx].normalize = bp_get(val, 1, 8) != 0;
            if (g_gxState.tcgs[tcIdx] != previous) g_pipelineConfigDirty = true;
            g_gxState.stateDirty = true;
          }
        } else {
#ifndef NDEBUG
          Log.debug("Unhandled XF register 0x{:04X} (value 0x{:08X})", reg, val);
#endif
        }
        break;
      }
    }
  }

  pos += dataBytes;
}

static void handle_draw_overrun [[noreturn]] (uint64_t totalVtxBytes, const u8* data, const u32& pos, u32 size) {
  // Hex dump around the draw command for debugging
  u32 cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  u32 dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  u32 dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (u32 i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, size);
}

// Draw command handler - parses vertices inline and caches results
static u32 calculate_last_vtx_size(GXVtxFmt fmt) {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      const auto& attrFmt = vtxFmt.attrs[i];
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += (i == GX_VA_NRM && vtxFmt.attrs[i].cnt == GX_NRM_NBT3) ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += (i == GX_VA_NRM && vtxFmt.attrs[i].cnt == GX_NRM_NBT3) ? 6 : 2;
      break;
    }
  }

  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;

  return vtxSize;
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange);
static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices);

// Draw command handler - parses vertices inline and caches results

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, const u8* data, u32& pos, u32 size) {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calculate_last_vtx_size(fmt); }

  const uint64_t totalVtxBytes = static_cast<uint64_t>(vtxCount) * vtxSize;
  if (pos > size || totalVtxBytes > size - pos)
    UNLIKELY { handle_draw_overrun(totalVtxBytes, data, pos, size); }
  const u32 vertexPos = pos;
  pos += static_cast<u32>(totalVtxBytes);
  vtxCount = drawable_vertex_count(prim, vtxCount);
  if (vtxCount == 0) return;

  auto* lastDraw = !g_gxState.stateDirty ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->mergeable && lastDraw->vtxFmt == fmt &&
                        prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS &&
                        lastDraw->vtxCount + static_cast<u32>(vtxCount) <= 0xFFFFu;

  // Push raw vertex data to buffer. Merged draws must remain byte-contiguous with the previous range.
  gfx::Range vertRange = gfx::push_verts(data + vertexPos, vtxCount * vtxSize, canMerge ? 0 : 4);

  // Try to merge with previous draw call
  if (canMerge) {
    // Only if the previous draw call was a single instance draw (no lines/points handling)
    u32 numIndices = 0;
    gfx::Range idxRange;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    const bool generatePrevious = lastDraw->indexCount == 0 && prim != GX_TRIANGLES;
    const u32 previousIndices = generatePrevious ? index_count(GX_TRIANGLES, static_cast<u16>(lastDraw->vtxCount)) : 0;
    const u32 currentIndices = (lastDraw->indexCount != 0 || generatePrevious) ? index_count(prim, vtxCount) : 0;
    const u32 generatedIndices = previousIndices + currentIndices;
    if (generatedIndices != 0) {
      const auto reservation = gfx::reserve_indices(generatedIndices * sizeof(u16), hadIndexRange ? 0 : 4);
      if (reservation.data != nullptr) {
        auto* out = reinterpret_cast<u16*>(reservation.data);
        if (generatePrevious) {
          lastDraw->indexCount = write_idx_buffer(out, GX_TRIANGLES, 0, static_cast<u16>(lastDraw->vtxCount));
          out += lastDraw->indexCount;
        }
        if (lastDraw->indexCount != 0) {
          numIndices = write_idx_buffer(out, prim, static_cast<u16>(lastDraw->vtxCount), vtxCount);
        }
        idxRange = reservation.range;
      }
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    ++gfx::g_mergedDrawCallCount;
    note_efb_depth_write();
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
}

static void handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {
  GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);

  ASSERT(pos <= size && size - pos >= 2, "draw vtxCount read overrun");
  u16 vtxCount = read_u16(data + pos, bigEndian);
  pos += 2;

  draw_prim(prim, fmt, vtxCount, data, pos, size);
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("write idx buffer");
    numIndices = index_count(prim, vtxCount);
    if (numIndices != 0) {
      const auto reservation = gfx::reserve_indices(numIndices * sizeof(u16), 4);
      if (reservation.data != nullptr) {
        numIndices = write_idx_buffer(reinterpret_cast<u16*>(reservation.data), prim, 0, vtxCount);
        idxRange = reservation.range;
      } else {
        numIndices = 0;
      }
    }
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

// The ready specialized pipeline found for get_pipeline_config's current value.
static struct {
  gfx::PipelineRef ref{};
  uint64_t generation = 0;
  uint64_t configSerial = 0;
  ShaderInfo info{};
  bool valid = false;
} sDrawPipeline;

// MP6_DRAW_PIPELINE_VERIFY=1: every reused pipeline is looked up again and
// must match; the counts print at exit.
static bool draw_pipeline_verify() {
  static const bool on = [] {
    const char* env = std::getenv("MP6_DRAW_PIPELINE_VERIFY");
    return env != nullptr && env[0] == '1';
  }();
  return on;
}

static bool same_shader_info(const ShaderInfo& a, const ShaderInfo& b) {
  return a.sampledTexCoords == b.sampledTexCoords && a.sampledTextures == b.sampledTextures &&
         a.sampledKColors == b.sampledKColors && a.sampledColorChannels == b.sampledColorChannels &&
         a.loadsTevReg == b.loadsTevReg && a.writesTevReg == b.writesTevReg && a.usesPTTexMtx == b.usesPTTexMtx &&
         a.usesTexMtx == b.usesTexMtx && a.indexAttr == b.indexAttr && a.usedIndStages == b.usedIndStages &&
         a.sampledIndTextures == b.sampledIndTextures && a.usedIndTexMtxs == b.usedIndTexMtxs &&
         a.uniformSize == b.uniformSize && a.usesFog == b.usesFog && a.lightingEnabled == b.lightingEnabled &&
         a.compactMatrices == b.compactMatrices && a.lineMode == b.lineMode;
}

static void verify_draw_pipeline(const PipelineConfig& config, gfx::PipelineRef reused, const ShaderInfo& info) {
  static uint64_t checked = 0, mismatched = 0;
  if (checked == 0) {
    std::atexit([] {
      std::fprintf(stderr, "[MP6-DRAW-PIPELINE] verify: %llu reused, %llu mismatched\n",
                   static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatched));
    });
  }
  ShaderInfo fresh{};
  bool uber = false;
  const auto ref = gfx::find_gx_pipeline(config, fresh, uber);
  ++checked;
  if (uber || ref != reused || !same_shader_info(fresh, info)) {
    ++mismatched;
  }
}

// gfx::find_gx_pipeline for the config get_pipeline_config just returned.
// Consecutive draws with the same config (only matrices, colours or geometry
// changed) reuse the previous draw's ready specialized pipeline instead of
// hashing the multi-kilobyte config again. An ubershader result is never
// reused: its specialized pipeline may be ready by the next draw.
static gfx::PipelineRef find_draw_pipeline(const PipelineConfig& config, ShaderInfo& info, bool& uber) {
  const uint64_t generation = gfx::gx_ready_pipeline_generation();
  if (sDrawPipeline.valid && sDrawPipeline.configSerial == sConfigMemo.serial &&
      sDrawPipeline.generation == generation) {
    uber = false;
    info = sDrawPipeline.info;
    if (draw_pipeline_verify()) {
      verify_draw_pipeline(config, sDrawPipeline.ref, info);
    }
    return sDrawPipeline.ref;
  }
  const auto pipeline = gfx::find_gx_pipeline(config, info, uber);
  sDrawPipeline = {
      .ref = pipeline, .generation = generation, .configSerial = sConfigMemo.serial, .info = info, .valid = !uber};
  return pipeline;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) {
  // Build pipeline, bind groups, and push draw command
  note_efb_depth_write();
  BindGroupRanges ranges{};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = g_gxState.arrays[i];
    if (array.cachedRange.size > 0) {
      ranges.vaRanges[i - GX_VA_POS] = array.cachedRange;
    } else {
      const auto range = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
      ranges.vaRanges[i - GX_VA_POS] = range;
      array.cachedRange = range;
    }
  }

  const auto& config=get_pipeline_config(prim,fmt);
  ShaderInfo info{};
  // A pipeline that isn't built yet draws through the ubershader this frame
  // (ubershader.hpp), with its config packed into the uniform block.
  bool uber = false;
  const auto pipeline = find_draw_pipeline(config, info, uber);
  resolve_sampled_textures(info);
  const auto bindGroups = build_bind_groups(info);

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  gfx::push_draw_command(DrawData{
      .pipeline = pipeline,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = uber ? build_uber_uniform(config.shaderConfig, info, vertRange.offset, ranges)
                           : build_uniform(info, vertRange.offset, ranges),
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = bindGroups,
      .dstAlpha = g_gxState.dstAlpha,
      .vtxFmt = fmt,
      .mergeable = prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS,
  });
}

std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {
  CHECK(pos + 2 <= size, "Aurora string length read overrun");
  const u16 length = read_u16(data + pos, bigEndian);
  pos += 2;

  CHECK(pos + length <= size, "Aurora string read overrun");
  std::string str(reinterpret_cast<const char*>(data) + pos, length);
  pos += length;
  return str;
}

void handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian) {
  ZoneScoped;
  ASSERT(pos <= size && size - pos >= 2, "Aurora cmd read overrun");
  u16 subCmd = read_u16(data + pos, bigEndian);
  pos += 2;

  // Setting of vertex array bases.
  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    CHECK(pos + 24 <= size, "GX_AURORA_LOAD_VIEWPORT_RENDER read overrun");
    const f32 left = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 top = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 width = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 height = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 nearZ = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 farZ = read_f32(data + pos, bigEndian);
    pos += 4;
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    CHECK(pos + 16 <= size, "GX_AURORA_LOAD_SCISSOR_RENDER read overrun");
    const int32_t left = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t top = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t width = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t height = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    CHECK(pos + 64 <= size, "GX_AURORA_LOAD_PROJECTION_FULL read overrun");
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = read_f32(data + pos, bigEndian);
        pos += 4;
      }
    }
    g_gxState.stateDirty = true;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    CHECK(pos + 13 <= size, "GX_AURORA_LOAD_ARRAYBASE read overrun");
    u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;

    u64 arrayAddr = read_u64(data + pos, bigEndian);
    pos += 8;
    u32 arraySize = read_u32(data + pos, bigEndian);
    pos += 4;
    bool le = data[pos] == 1;
    pos += 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      array.data = newData;
      array.size = arraySize;
      if (array.le != le) g_pipelineConfigDirty = true;
      array.le = le;
      // Only drop the cached upload when the backing array actually changes.
      array.cachedRange = {};
      g_gxState.stateDirty = true;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    CHECK(pos + 34 <= size, "GX_AURORA_LOAD_TEXOBJ read overrun");
    const auto texMapId = data[pos];
    pos += 1;
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    slot.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    slot.mWidth = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.mHeight = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.mFormat = static_cast<GXTexFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    slot.tlut = static_cast<GXTlut>(read_u32(data + pos, bigEndian));
    pos += 4;
    if (data[pos] != 0) {
      slot.flags |= 1u;
    } else {
      slot.flags &= ~1u;
    }
    pos += 1;
    slot.texObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.texDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.set_no_cache(false); // Reset no-cache flag
    g_gxState.stateDirty = true;
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    CHECK(pos + 23 <= size, "GX_AURORA_LOAD_TLUT read overrun");
    const auto idx = data[pos];
    pos += 1;
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    slot.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    slot.format = static_cast<GXTlutFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    slot.numEntries = read_u16(data + pos, bigEndian);
    pos += 2;
    slot.tlutObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.tlutDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.set_no_cache(false); // Reset no-cache flag
    g_gxState.stateDirty = true;
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    CHECK(pos + 20 <= size, "GX2_SET_POLYGON_OFFSET read overrun");
    g_gxState.frontOffset = read_f32(data + pos, bigEndian);
    pos += 4;
    g_gxState.frontScale = read_f32(data + pos, bigEndian);
    pos += 4;
    g_gxState.backOffset = read_f32(data + pos, bigEndian);
    pos += 4;
    g_gxState.backScale = read_f32(data + pos, bigEndian);
    pos += 4;
    g_gxState.clamp = read_f32(data + pos, bigEndian);
    pos += 4;
    mark_pipeline_state_dirty();
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    CHECK(pos + 16 <= size, "GX_AURORA_LOAD_COPY_SRC read overrun");
    const int32_t left = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t top = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t width = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t height = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    CHECK(pos + 13 <= size, "GX_AURORA_LOAD_COPY_DST read overrun");
    g_gxState.texCopyDstWidth = read_u32(data + pos, bigEndian);
    pos += 4;
    g_gxState.texCopyDstHeight = read_u32(data + pos, bigEndian);
    pos += 4;
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    CHECK(pos + 8 <= size, "GX_AURORA_LOAD_COPY_DEST read overrun");
    g_gxState.texCopyDest = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    CHECK(pos + 8 <= size, "GX_AURORA_BEGIN_OFFSCREEN read overrun");
    const u32 width = read_u32(data + pos, bigEndian);
    pos += 4;
    const u32 height = read_u32(data + pos, bigEndian);
    pos += 4;
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_SET_COPY_MIP_GEN) {
    CHECK(pos + 1 <= size, "GX_AURORA_SET_COPY_MIP_GEN read overrun");
    g_gxState.texCopyMipGen = data[pos] != 0;
    pos += 1;
  } else if (subCmd == GX_AURORA_DESTROY_FRAMEBUFFER_CACHE) {
    gfx::clear_offscreen_cache();
  } else if (subCmd == GX_AURORA_REPLAY_COPY_CLEAR) {
    CHECK(pos + 1 <= size, "GX_AURORA_REPLAY_COPY_CLEAR read overrun");
    const bool clear = data[pos] != 0;
    pos += 1;
    if (clear) {
      gfx::replay_copy_clear(g_gxState.colorUpdate, g_gxState.alphaUpdate, g_gxState.depthUpdate,
                             g_gxState.clearColor, clear_depth_value());
    }
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    CHECK(pos + 4 <= size, "GX_AURORA_DESTROY_TEXOBJ read overrun");
    evict_texture_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    CHECK(pos + 4 <= size, "GX_AURORA_DESTROY_TLUT read overrun");
    evict_tlut_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    CHECK(pos + 8 <= size, "GX_AURORA_DESTROY_COPY_TEX read overrun");
    evict_copy_texture(reinterpret_cast<const void*>(read_u64(data + pos, bigEndian)));
    pos += 8;
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    ASSERT(pos <= size && size - pos >= 5, "GX_AURORA_DRAW_SIZED read overrun");
    u8 cmd = data[pos];
    pos += 1;
    u32 byteLen = read_u32(data + pos, bigEndian);
    pos += 4;
    ASSERT(byteLen <= size - pos, "GX_AURORA_DRAW_SIZED data overrun");
    GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calculate_last_vtx_size(fmt);
      }
      ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
             "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), data, pos, size);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    ASSERT(pos <= size && size - pos >= 7, "GX_AURORA_DRAW_INDEXED read overrun");
    const u8 cmd = data[pos];
    pos += 1;
    const u16 vtxCount = read_u16(data + pos, bigEndian);
    pos += 2;
    const u32 indexCount = read_u32(data + pos, bigEndian);
    pos += 4;
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
           static_cast<u32>(prim));
    const uint64_t idxBytes = static_cast<uint64_t>(indexCount) * sizeof(u16);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calculate_last_vtx_size(fmt);
    }
    const uint64_t totalVtxBytes = static_cast<uint64_t>(vtxCount) * vtxSize;
    ASSERT(idxBytes + totalVtxBytes <= size - pos, "GX_AURORA_DRAW_INDEXED data overrun");
    if (indexCount == 0 || vtxCount == 0) {
      pos += static_cast<u32>(idxBytes + totalVtxBytes);
      return;
    }
    // Validate the entire payload before either upload. The proven lengths
    // now fit the command's u32 byte range; index data stays host-endian.
    const gfx::Range idxRange = gfx::push_indices(data + pos, static_cast<u32>(idxBytes), 4);
    pos += static_cast<u32>(idxBytes);
    const gfx::Range vertRange = gfx::push_verts(data + pos, static_cast<u32>(totalVtxBytes), 4);
    pos += static_cast<u32>(totalVtxBytes);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

} // namespace aurora::gx::fifo

// ---------------------------------------------------------------------------
// [MP6] Retained replay entry points (compat/aurora/windows/0006).
// ---------------------------------------------------------------------------
namespace aurora::gx::fifo {
// [MP6] Host hooks run between stream segments (aurora_gx_replay_stream_hooked).
struct ReplayHooks {
  const u32* offsets = nullptr;
  u32 count = 0;
  void (*fn)(u32 index, void* user) = nullptr;
  void* user = nullptr;
};
static ReplayHooks sHooks;

static void process_hooked(const u8* bytes, u32 size) {
  u32 at = 0;
  for (u32 i = 0; i < sHooks.count; ++i) {
    const u32 offset = std::min(sHooks.offsets[i], size);
    if (offset > at) process(bytes + at, offset - at, true);
    at = std::max(at, offset);
    sHooks.fn(i, sHooks.user);
  }
  if (size > at) process(bytes + at, size - at, true);
}

static bool hooks_match() {
  const auto& h = sRetain.hooks;
  return h.size() == sHooks.count &&
         (sHooks.count == 0 || std::memcmp(h.data(), sHooks.offsets, sHooks.count * sizeof(u32)) == 0);
}

static bool retained_check(const u8* bytes, u32 size) {
  auto& r = sRetain;
  if (!hooks_match()) return false;
  // A pending static-texture-cache clear is consumed by the next decode, which
  // then binds different textures than the recording did.
  if (!r.valid || size != r.stream.size() || static_texture_cache_clear_pending()) return false;
  if (std::memcmp(retain_state_begin(), r.state.data(), r.state.size()) != 0) {
    ++r.stateMismatch;
    return false;
  }
  // One streaming pass: (new ^ recorded) & ~mask must be zero everywhere.
  // Thousands of short memcmp calls between payloads cost more than this.
  const u8* old = r.stream.data();
  const u8* mask = r.mask.data();
  u32 i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t a, b, m;
    std::memcpy(&a, bytes + i, 8);
    std::memcpy(&b, old + i, 8);
    std::memcpy(&m, mask + i, 8);
    if (((a ^ b) & ~m) != 0) {
      ++r.streamMismatch;
      return false;
    }
  }
  for (; i < size; ++i) {
    if (((bytes[i] ^ old[i]) & ~mask[i]) != 0) {
      ++r.streamMismatch;
      return false;
    }
  }
  if (!gfx::retained_can_reuse()) {
    ++r.gfxMismatch;
    return false;
  }
  return true;
}

// Rewrites the recorded uniforms' matrices for this stream.
static bool retained_patch(const u8* bytes) {
  auto& r = sRetain;
  u32 base = 0, uniformSize = 0;
  u8* uniforms = gfx::retained_uniform_bytes(base, uniformSize);
  for (const auto& patch : r.patches) {
    const u32 dst = patch.uniformOffset - base;
    if (patch.uniformOffset < base || dst + sizeof(Mat3x4<float>) > uniformSize) return false;
    auto& live = patch.normal ? g_gxState.pnMtx[patch.slot].nrm : g_gxState.pnMtx[patch.slot].pos;
    Mat3x4<float> mtx;
    if (patch.source == RetainCarry) {
      mtx = live;
    } else {
      std::memcpy(static_cast<void*>(&mtx), uniforms + dst, sizeof(mtx));
      retain_write_mtx(mtx, bytes + patch.source, patch.normal);
    }
    std::memcpy(uniforms + dst, static_cast<const void*>(&mtx), sizeof(mtx));
  }
  return true;
}

static bool retained_try_reuse(const u8* bytes, u32 size) {
  auto& r = sRetain;
  if (!retained_check(bytes, size) || !retained_patch(bytes)) return false;
  if (!gfx::retained_reuse()) return false;
  // Leave the matrix state where a full decode of this stream would.
  for (u32 slot = 0; slot < MaxPnMtx; ++slot) {
    if (r.pos[slot] < RetainFixed) retain_write_mtx(g_gxState.pnMtx[slot].pos, bytes + r.pos[slot], false);
    if (r.nrm[slot] < RetainFixed) retain_write_mtx(g_gxState.pnMtx[slot].nrm, bytes + r.nrm[slot], true);
  }
  g_efbDepthWriteSerial += r.depthSerialDelta;
  ++r.reused;
  return true;
}

static void retained_record(const u8* bytes, u32 size) {
  auto& r = sRetain;
  r.valid = false;
  if (!gfx::retained_begin_record()) {
    process_hooked(bytes, size);
    ++r.refused;
    return;
  }
  r.base = bytes;
  r.size = size;
  r.pos.fill(RetainCarry);
  r.nrm.fill(RetainCarry);
  r.patches.clear();
  r.masked.clear();
  r.state.assign(retain_state_begin(), retain_state_begin() + retain_state_size());
  const auto serial = g_efbDepthWriteSerial;
  // The region must not merge into a draw recorded before it.
  g_gxState.stateDirty = true;
  g_retainRecording = true;
  gfx::retained_allow_custom_draws(sHooks.count != 0);
  process_hooked(bytes, size);
  gfx::retained_allow_custom_draws(false);
  g_retainRecording = false;
  r.hooks.assign(sHooks.offsets, sHooks.offsets + sHooks.count);
  r.base = nullptr;
  if (!gfx::retained_end_record()) {
    ++r.refused;
    return;
  }
  r.stream.assign(bytes, bytes + size);
  r.mask.assign(size, 0);
  for (const auto& [offset, length] : r.masked) std::memset(r.mask.data() + offset, 0xFF, length);
  r.depthSerialDelta = g_efbDepthWriteSerial - serial;
  r.valid = true;
  ++r.recorded;
}
} // namespace aurora::gx::fifo

extern "C" {
static int sRetainVerify = 0;
static uint64_t sRetainVerifyOk = 0, sRetainVerifyFail = 0;

// MP6_FI_RETAINED_VERIFY: whenever a reuse would happen, decode in full anyway
// and require the decode's recording to equal the patched previous recording.
void aurora_gx_replay_set_verify(int on) { sRetainVerify = on; }
void aurora_gx_replay_verify_stats(uint64_t* ok, uint64_t* fail) {
  *ok = sRetainVerifyOk;
  *fail = sRetainVerifyFail;
}

// Process one replay stream (already-encoded, big-endian GX commands) into the
// open frame, re-emitting the previous replay's recorded draws when that is
// exactly equivalent. Returns 1 when the recording was reused, 0 after a full
// decode. A full decode records only when `record` is set: recording costs
// extra copies, so a caller that cannot fit another replay of this stream
// should not pay for one.
int aurora_gx_replay_stream(const void* data, uint32_t size, int record) {
  aurora::gx::fifo::drain();
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (sRetainVerify) {
    if (aurora::gx::fifo::retained_check(bytes, size) && aurora::gx::fifo::retained_patch(bytes)) {
      aurora::gfx::retained_snapshot();
      aurora::gx::fifo::retained_record(bytes, size);
      const char* why = aurora::gfx::retained_compare_snapshot();
      if (why == nullptr) {
        ++sRetainVerifyOk;
      } else if (++sRetainVerifyFail <= 20) {
        std::fprintf(stderr, "[RETAINED-VERIFY] mismatch: %s\n", why);
      }
      return 0;
    }
    aurora::gx::fifo::retained_record(bytes, size);
    return 0;
  }
  if (aurora::gx::fifo::retained_try_reuse(bytes, size)) return 1;
  if (record) {
    aurora::gx::fifo::retained_record(bytes, size);
  } else {
    aurora::gx::fifo::sRetain.valid = false;
    aurora::gx::fifo::process(bytes, size, true);
    ++aurora::gx::fifo::sRetain.refused;
  }
  return 0;
}

// [MP6] As aurora_gx_replay_stream, with host hooks: hook(i) runs exactly
// where offsets[i] (ascending, at command boundaries) falls in the stream. The
// hooks may only push custom draws with inline payloads; those are recorded
// and re-emitted in order on reuse, so on a reused frame the hooks do not run.
// A recording is reused only with the same hook offsets.
int aurora_gx_replay_stream_hooked(const void* data, uint32_t size, int record, const uint32_t* offsets,
                                   uint32_t count, void (*hook)(uint32_t index, void* user), void* user) {
  using namespace aurora::gx::fifo;
  sHooks = {offsets, hook ? count : 0, hook, user};
  int reused = 0;
  if (sRetainVerify) {
    // Verification compares two recordings; keep it on the plain path.
    sHooks = {};
    aurora::gx::fifo::drain();
    const auto* bytes = static_cast<const uint8_t*>(data);
    sRetain.valid = false;
    sHooks = {offsets, hook ? count : 0, hook, user};
    process_hooked(bytes, size);
    sHooks = {};
    return 0;
  }
  aurora::gx::fifo::drain();
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (retained_try_reuse(bytes, size)) {
    reused = 1;
  } else if (record) {
    retained_record(bytes, size);
  } else {
    sRetain.valid = false;
    process_hooked(bytes, size);
    ++sRetain.refused;
  }
  sHooks = {};
  return reused;
}

void aurora_gx_replay_invalidate(void) {
  aurora::gx::fifo::sRetain.valid = false;
  aurora::gfx::retained_invalidate();
}

void aurora_gx_replay_stats(uint64_t* out /*6*/) {
  const auto& r = aurora::gx::fifo::sRetain;
  out[0] = r.reused;
  out[1] = r.recorded;
  out[2] = r.refused;
  out[3] = r.streamMismatch;
  out[4] = r.stateMismatch;
  out[5] = r.gfxMismatch;
}
} // extern "C"
