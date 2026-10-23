#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXCommandList.h>

template <class T>
struct ArrayRef {
  const T* data;
  std::size_t size;
};

struct TestLog {
  template <class... Args>
  void warn(Args&&...) {}
  template <class... Args>
  [[noreturn]] void fatal(Args&&...) {
    throw std::runtime_error("renderer fatal");
  }
} Log;

#define ZoneScoped
#define ZoneScopedN(name)
#define LIKELY
#define UNLIKELY
#define AURORA_ALIGN(value, alignment) (((value) + (alignment)-1) & ~((alignment)-1))
#ifdef MP6_RENDERER_RELEASE
#define NDEBUG
#endif
#include "checks.inc"
#ifdef MP6_RENDERER_RELEASE
#undef NDEBUG
#endif
#include "buffer.inc"

namespace gfx {
struct Range {
  u32 offset = 0;
  u32 size = 0;
};
struct IndexReservation {
  Range range;
  u8* data = nullptr;
};
struct FramePacket {
  ByteBuffer indices;
};
static FramePacket frame;
static ByteBuffer verts;
static bool recording = true;
static unsigned copiedIndexUploads = 0;

static FramePacket& current_frame_packet() { return frame; }
static bool check_recording(const char*) { return recording; }

#include "reserve.inc"

static Range push_indices(const u8* data, std::size_t length, std::size_t alignment) {
  if (!recording) return {};
  auto& target = frame.indices;
  const auto begin = target.size();
  const auto aligned = alignment == 0 ? begin : AURORA_ALIGN(begin, alignment);
  if (aligned > begin) target.append_zeroes(aligned - begin);
  const auto offset = target.size();
  target.append(data, length);
  ++copiedIndexUploads;
  return {static_cast<u32>(offset), static_cast<u32>(length)};
}
static Range push_verts(const u8* data, std::size_t length, std::size_t alignment) {
  const auto begin = verts.size();
  const auto aligned = alignment == 0 ? begin : AURORA_ALIGN(begin, alignment);
  if (aligned > begin) verts.append_zeroes(aligned - begin);
  const auto offset = verts.size();
  verts.append(data, length);
  return {static_cast<u32>(offset), static_cast<u32>(length)};
}
static Range push_storage(const u8*, std::size_t) { return {}; }
} // namespace gfx

static std::vector<u16> commands;
static std::vector<u16> scratchOracle;

struct GXBindGroups {};
struct DrawData {
  int pipeline = 0;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  u32 vtxCount = 0;
  u32 indexCount = 0;
  u32 instanceCount = 1;
  GXBindGroups bindGroups;
  u32 dstAlpha = 0;
  GXVtxFmt vtxFmt = GX_MAX_VTXFMT;
  bool mergeable = false;
};

struct State {
  bool stateDirty = true;
  GXVtxFmt lastVtxFmt = GX_MAX_VTXFMT;
  u32 lastVtxSize = 0;
  u32 dstAlpha = 0;
  GXAttrType vtxDesc[GX_VA_TEX7 + 1]{};
  struct {
    gfx::Range cachedRange;
    const void* data = nullptr;
    std::size_t size = 0;
  } arrays[GX_VA_TEX7 + 1]{};
} g_gxState;

static std::vector<DrawData> drawCommands;

namespace gfx {
static int g_mergedDrawCallCount = 0;
template <class T>
static T* get_last_draw_command() {
  return drawCommands.empty() ? nullptr : reinterpret_cast<T*>(&drawCommands.back());
}
static void push_draw_command(DrawData data) { drawCommands.push_back(data); }
static int find_gx_pipeline(...) { return 1; }
} // namespace gfx

struct BindGroupRanges {
  gfx::Range vaRanges[GX_VA_TEX7 - GX_VA_POS + 1];
};
struct ShaderConfig {};
struct PipelineConfig { ShaderConfig shaderConfig; };
struct ShaderInfo {};
static void populate_pipeline_config(PipelineConfig&, GXPrimitive, GXVtxFmt) {}
// Pipeline preparation is outside this index-reservation fixture. The
// dedicated config-reuse fixture exercises the actual memo helper separately.
static const PipelineConfig& get_pipeline_config(GXPrimitive prim, GXVtxFmt fmt) {
  static PipelineConfig config;
  populate_pipeline_config(config, prim, fmt);
  return config;
}
static int find_draw_pipeline(const PipelineConfig&, ShaderInfo&, bool& uber) {
  uber = false;
  return gfx::find_gx_pipeline();
}
static void resolve_sampled_textures(const ShaderInfo&) {}
static GXBindGroups build_bind_groups(const ShaderInfo&) { return {}; }
static gfx::Range build_uniform(const ShaderInfo&, u32, BindGroupRanges&) {
  g_gxState.stateDirty = false;
  return {};
}
// The ubershader path (a pipeline still building) is outside this fixture.
static gfx::Range build_uber_uniform(const ShaderConfig&, const ShaderInfo& info, u32 offset,
                                     BindGroupRanges& ranges) {
  return build_uniform(info, offset, ranges);
}
static void note_efb_depth_write() {}
static u32 calculate_last_vtx_size(GXVtxFmt fmt) {
  g_gxState.lastVtxFmt = fmt;
  return g_gxState.lastVtxSize = fmt == GX_VTXFMT0 ? 4 : 8;
}
[[noreturn]] static void handle_draw_overrun(std::uint64_t, const u8*, const u32&, u32) {
  throw std::runtime_error("overrun");
}
static void handle_draw_unmerged(GXPrimitive, GXVtxFmt, u16, gfx::Range);
static void push_gx_draw(GXPrimitive, GXVtxFmt, u16, gfx::Range, gfx::Range, u32);

#include "indices.inc"
#include "draws.inc"

static std::vector<u16> legacy_indices(GXPrimitive prim, u16 vtxStart, u16 vtxCount) {
  const u32 count = drawable_vertex_count(prim, vtxCount);
  std::vector<u16> out;
  auto tri = [&](u32 a, u32 b, u32 c) {
    out.push_back(static_cast<u16>(vtxStart + a));
    out.push_back(static_cast<u16>(vtxStart + b));
    out.push_back(static_cast<u16>(vtxStart + c));
  };
  if (count == 0) return out;
  if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    return {0, 1, 3, 3, 2, 0};
  }
  if (prim == GX_QUADS) {
    for (u32 v = 0; v + 3 < count; v += 4) {
      tri(v, v + 1, v + 2);
      tri(v + 2, v + 3, v);
    }
    if (count % 4 == 3) {
      const u32 v = count - 3;
      tri(v, v + 1, v + 2);
    }
  } else if (prim == GX_TRIANGLES) {
    for (u32 v = 0; v < count; ++v) out.push_back(static_cast<u16>(vtxStart + v));
  } else if (prim == GX_TRIANGLEFAN) {
    for (u32 v = 2; v < count; ++v) tri(0, v - 1, v);
  } else {
    for (u32 v = 2; v < count; ++v) {
      if (v & 1) tri(v - 1, v - 2, v);
      else tri(v - 2, v - 1, v);
    }
  }
  return out;
}

static void reset() {
  drawCommands.clear();
  gfx::frame.indices.clear();
  gfx::verts.clear();
  gfx::recording = true;
  gfx::copiedIndexUploads = 0;
  gfx::g_mergedDrawCallCount = 0;
  g_gxState = State{};
}

static std::array<GXPrimitive, 7> primitives() {
  return {GX_QUADS, GX_TRIANGLES, GX_TRIANGLEFAN, GX_TRIANGLESTRIP,
          GX_LINES, GX_LINESTRIP, GX_POINTS};
}

static void topology_matches_legacy() {
  for (const auto prim : primitives()) {
    for (u32 count = 0; count <= 512; ++count) {
      for (const u32 start : {u32{0}, u32{65535} - count}) {
        const auto expected = legacy_indices(prim, static_cast<u16>(start), static_cast<u16>(count));
        std::vector<u16> storage(expected.size() + 4, 0xdead);
        const auto got = write_idx_buffer(storage.data() + 2, prim, static_cast<u16>(start), static_cast<u16>(count));
        assert(got == expected.size());
        assert(index_count(prim, static_cast<u16>(count)) == expected.size());
        assert(std::equal(expected.begin(), expected.end(), storage.begin() + 2));
        assert(storage[0] == 0xdead && storage[1] == 0xdead);
        assert(storage[expected.size() + 2] == 0xdead && storage[expected.size() + 3] == 0xdead);
      }
    }
    for (u32 count = 65520; count <= 65535; ++count) {
      const auto expected = legacy_indices(prim, 0, static_cast<u16>(count));
      std::vector<u16> storage(expected.size() + 4, 0xbeef);
      const auto got = write_idx_buffer(storage.data() + 2, prim, 0, static_cast<u16>(count));
      assert(got == expected.size());
      assert(std::equal(expected.begin(), expected.end(), storage.begin() + 2));
      assert(storage[0] == 0xbeef && storage[1] == 0xbeef);
      assert(storage[expected.size() + 2] == 0xbeef && storage[expected.size() + 3] == 0xbeef);
    }
  }
}

static void reservation_guards() {
  reset();
  const u8 prefixBytes[] = {0x11, 0x22, 0x33};
  gfx::frame.indices.append(prefixBytes, sizeof(prefixBytes));
  const auto reserved = gfx::reserve_indices(17 * sizeof(u16), 4);
  assert(reserved.range.offset == 4);
  assert(reserved.range.size == 34);
  assert(gfx::frame.indices.size() == 38);
  assert(gfx::frame.indices.data()[3] == 0);
  std::memset(reserved.data, 0xa5, reserved.range.size);
  assert(gfx::frame.indices.data()[0] == prefixBytes[0]);
  assert(gfx::frame.indices.data()[1] == prefixBytes[1]);
  assert(gfx::frame.indices.data()[2] == prefixBytes[2]);
  for (u32 i = reserved.range.offset; i < reserved.range.offset + reserved.range.size; ++i) {
    assert(gfx::frame.indices.data()[i] == 0xa5);
  }

  const auto before = gfx::frame.indices.size();
  gfx::recording = false;
  const auto rejected = gfx::reserve_indices(64, 4);
  assert(rejected.data == nullptr && rejected.range.size == 0);
  assert(gfx::frame.indices.size() == before);
}

static std::vector<u8> raw(70000 * 8, 0x46);
static void draw(GXPrimitive prim, u16 count, GXVtxFmt fmt = GX_VTXFMT0) {
  u32 pos = 0;
  draw_prim(prim, fmt, count, raw.data(), pos, static_cast<u32>(raw.size()));
  assert(pos == count * (fmt == GX_VTXFMT0 ? 4u : 8u));
}

static void direct_merge_and_growth() {
  reset();
  draw(GX_TRIANGLES, 6);
  draw(GX_QUADS, 4);
  draw(GX_TRIANGLESTRIP, 5);
  assert(drawCommands.size() == 1);
  assert(drawCommands[0].vtxCount == 15 && drawCommands[0].indexCount == 21);
  const auto first = legacy_indices(GX_TRIANGLES, 0, 6);
  const auto second = legacy_indices(GX_QUADS, 6, 4);
  const auto third = legacy_indices(GX_TRIANGLESTRIP, 10, 5);
  std::vector<u16> expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  expected.insert(expected.end(), third.begin(), third.end());
  assert(drawCommands[0].idxRange.offset == 0);
  assert(drawCommands[0].idxRange.size == expected.size() * sizeof(u16));
  assert(gfx::copiedIndexUploads == 0);
  assert(gfx::frame.indices.size() == expected.size() * sizeof(u16));
  assert(std::memcmp(gfx::frame.indices.data(), expected.data(), gfx::frame.indices.size()) == 0);

  reset();
  const u8 prefix[] = {0x99};
  gfx::frame.indices.append(prefix, sizeof(prefix));
  draw(GX_TRIANGLESTRIP, 65535);
  assert(gfx::copiedIndexUploads == 0);
  assert(gfx::frame.indices.data()[0] == prefix[0]);
  assert(drawCommands[0].idxRange.offset == 4);
  const auto expectedCount = legacy_indices(GX_TRIANGLESTRIP, 0, 65535).size();
  assert(drawCommands[0].idxRange.size == expectedCount * sizeof(u16));
  assert(gfx::frame.indices.size() == 4 + expectedCount * sizeof(u16));

  const u16 external[] = {7, 8, 9};
  const auto range = gfx::push_indices(reinterpret_cast<const u8*>(external), sizeof(external), 4);
  assert(range.size == sizeof(external) && gfx::copiedIndexUploads == 1);
  assert(std::memcmp(gfx::frame.indices.data() + range.offset, external, sizeof(external)) == 0);
}

int main() {
  topology_matches_legacy();
  reservation_guards();
  direct_merge_and_growth();
  std::puts("PASS: direct generated GX indices");
}
