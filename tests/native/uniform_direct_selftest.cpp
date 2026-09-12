// The runner extracts the actual ByteBuffer, GX types, helpers, and both full
// serializers. Only device-independent texture/frame fixtures are substituted.
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include <aurora/math.hpp>
#include <dolphin/gx/GXEnum.h>
#include "selftest_assert.h"

static unsigned testIteration = 0;
static const char* testPhase = "startup";
[[noreturn]] static void bounded_abort(const char* file, int line) { std::fprintf(stderr, "bounded writer rejected invalid operation at %s:%d (iteration %u, %s)\n", file, line, testIteration, testPhase); std::exit(73); }
#define abort() bounded_abort(__FILE__, __LINE__)
#define ZoneScoped
#define DEFAULT_FATAL(...) default: abort();
using u8 = uint8_t; using s8 = int8_t; using u16 = uint16_t; using u32 = uint32_t; using f32 = float;
namespace GX { constexpr size_t MaxLights = 8; using LightMask = std::bitset<8>; }
namespace aurora {
template <class T> struct ArrayRef { const T* data; size_t size; };
#include "buffer.inc"
namespace gfx {
struct Range { uint32_t offset = 0, size = 0; bool operator==(const Range&) const = default; };
struct TextureRef { bool hasArbitraryMips = false; struct { u32 width = 1; } size; };
struct TextureBind {
  struct TexObj {
    u32 w = 1, h = 1; float bias = 0;
    u32 width() const { return w; } u32 height() const { return h; }
    float lod_bias() const { return bias; }
  } texObj;
  const TextureRef* ref = nullptr;
};
}
}
#include "lib/gfx/uniform_writer.hpp"
namespace aurora::gfx {
static ByteBuffer* frame = nullptr;
static u32 alignment = 256;
[[gnu::noinline]] UniformWriter begin_uniform(uint32_t capacity) {
  if (!frame) return {};
  return UniformWriter(*frame, capacity, alignment);
}
}
namespace aurora::gx {
constexpr bool UseReversedZ = TEST_REVERSED_Z;
constexpr u32 MaxPnMtx = 10, MaxTexMtx = 10, MaxPTTexMtx = 20, MaxTexCoord = 8;
constexpr u32 MaxTextures = 8, MaxKColors = 4, MaxColorChannels = 4, MaxTevRegs = 4;
constexpr u32 MaxVtxAttr = GX_VA_MAX_ATTR, MaxIndStages = 4, MaxIndTexMtxs = 3, MaxIndexAttr = 12;
#include "gx_types.inc"
struct State {
  bool stateDirty = true;
  u32 currentPnMtx = 0;
  struct Viewport { float width = 1920, height = 1080; } renderViewport, logicalViewport;
  u8 pointSize = 6, lineWidth = 6;
  GXTexOffset pointTexOffset = GX_TO_ZERO, lineTexOffset = GX_TO_ZERO;
  bool lineHalfAspect = false;
  Mat4x4<float> proj;
  std::array<PnMtx, MaxPnMtx> pnMtx;
  std::array<Mat3x4<float>, MaxTexMtx> texMtxs;
  std::array<Mat3x4<float>, MaxPTTexMtx> ptTexMtxs;
  std::array<Vec4<float>, 4> colorRegs, kcolors;
  std::array<Light, 8> lights;
  std::array<ColorChannelConfig, 4> colorChannelConfig;
  std::array<ColorChannelState, 4> colorChannelState;
  FogState fog;
  std::array<TexCoordScale, 8> texCoordScales;
  std::array<IndTexMtxInfo, 3> indTexMtxs;
} g_gxState;
bool enableLodBias = true;
static std::array<gfx::TextureBind, 8> textures;
static std::array<gfx::TextureRef, 8> textureRefs;
static const gfx::TextureBind& get_texture(GXTexMapID index) { return textures[static_cast<size_t>(index)]; }
#include "helpers.inc"
static ByteBuffer* referenceFrame = nullptr;
static uint64_t copiedBytes = 0;
[[gnu::noinline]] static gfx::Range reference_push(const uint8_t* bytes, size_t length) {
  if (!referenceFrame) return {};
  auto& frame = *referenceFrame;
  const size_t padding = gfx::alignment ? (0 - frame.size()) & (gfx::alignment - 1) : 0;
  assert(frame.size() + padding + length <= 65536);
  if (padding) frame.append_zeroes(padding);
  const u32 start = frame.size();
  frame.append(bytes, length);
  copiedBytes += length;
  return {start, static_cast<u32>(length)};
}
namespace reference {
#include "reference.inc"
}
namespace subject {
#include "subject.inc"
}
}

static uint32_t randomBits = 0x58a1c967;
static u32 next() { randomBits ^= randomBits << 13; randomBits ^= randomBits >> 17; return randomBits ^= randomBits << 5; }
static float value() {
  static constexpr u32 special[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00001, 0x7fe13579, 1, 0x807fffff};
  u32 bits = next();
  return std::bit_cast<float>((bits & 7) == 0 ? special[(bits >> 8) & 7] : bits);
}
static void floats(aurora::Vec2<float>& v) { v.x = value(); v.y = value(); }
static void floats(aurora::Vec4<float>& v) { for (int i = 0; i < 4; ++i) v[i] = value(); }
static void floats(aurora::Mat3x2<float>& v) { floats(v.m0); floats(v.m1); floats(v.m2); }
static void floats(aurora::Mat3x4<float>& v) { floats(v.m0); floats(v.m1); floats(v.m2); }
static void floats(aurora::Mat4x4<float>& v) { floats(v.m0); floats(v.m1); floats(v.m2); floats(v.m3); }
static void floats(aurora::gx::PnMtx& v) { floats(v.pos); floats(v.nrm); }
static void floats(aurora::gx::Light& v) { floats(v.pos); floats(v.dir); floats(v.color); floats(v.cosAtt); floats(v.distAtt); }
template <typename T, size_t N> static void floats(std::array<T, N>& v) { for (auto& entry : v) floats(entry); }

static aurora::gx::ShaderInfo configure(u32 iteration) {
  using namespace aurora; using namespace aurora::gx;
  auto& s = g_gxState;
  s.currentPnMtx = iteration % 13;
  s.pointSize = next(); s.lineWidth = next(); s.lineHalfAspect = next() & 1;
  s.pointTexOffset = static_cast<GXTexOffset>(iteration % 6);
  s.lineTexOffset = static_cast<GXTexOffset>((iteration / 6) % 6);
  floats(s.pnMtx); floats(s.texMtxs); floats(s.ptTexMtxs); floats(s.lights);
  floats(s.colorRegs); floats(s.kcolors); floats(s.proj); floats(s.fog.color);
  s.fog.a = value(); s.fog.b = value(); s.fog.c = value();
  s.renderViewport = {static_cast<float>(1 + next() % 4096), static_cast<float>(1 + next() % 4096)};
  s.logicalViewport = {static_cast<float>(next() % 1024), static_cast<float>(next() % 1024)};
  for (auto& c : s.colorChannelConfig) { c.lightingEnabled = next() & 1; c.ambSrc = static_cast<GXColorSrc>(next() & 1); c.matSrc = static_cast<GXColorSrc>(next() & 1); }
  for (auto& c : s.colorChannelState) { floats(c.ambColor); floats(c.matColor); c.lightMask = next(); }
  for (auto& m : s.indTexMtxs) { floats(m.mtx); m.scaleExp = next(); }
  for (auto& scale : s.texCoordScales) { scale.scaleS = next(); scale.scaleT = next(); scale.lineOffset = next() & 1; scale.pointOffset = next() & 1; }
  enableLodBias = iteration & 1;
  for (size_t i = 0; i < textures.size(); ++i) {
    textures[i].texObj = {next() % 2048, next() % 2048, value()};
    textureRefs[i].hasArbitraryMips = next() & 1; textureRefs[i].size.width = next() % 8192;
    textures[i].ref = (next() & 1) ? &textureRefs[i] : nullptr;
  }
  ShaderInfo info;
  info.uniformSize = 8192; // First pass measures actual raw length; then tighten.
  info.compactMatrices = iteration & 1; info.lightingEnabled = iteration & 2;
  info.usesFog = iteration & 4; info.lineMode = (iteration >> 3) & 3;
  info.loadsTevReg = next(); info.sampledColorChannels = next(); info.sampledKColors = next();
  info.usesPTTexMtx = next(); info.usesTexMtx = next(); info.usedIndTexMtxs = next(); info.sampledTextures = next();
  if (iteration % 29 == 0) { info.usesTexMtx.reset(); info.usesPTTexMtx.reset(); info.usedIndTexMtxs.reset(); info.sampledTextures.reset(); }
  if (iteration % 31 == 0) { info.usesTexMtx.set(); info.usesPTTexMtx.set(); info.usedIndTexMtxs.set(); info.sampledTextures.set(); }
  return info;
}

static void oracle(bool negative) {
  using namespace aurora; using namespace aurora::gx;
  std::array<u8, 65536 + 2> referenceMemory{}, subjectMemory{};
  // Deliberately unaligned borrowed storage, like an arbitrary mapped subspan.
  ByteBuffer ref(referenceMemory.data() + 1, 65536), out(subjectMemory.data() + 1, 65536);
  referenceFrame = &ref; gfx::frame = &out;
  BindGroupRanges ranges;
  std::array<gfx::Range, 8> previous{};
  uint64_t checkedBytes = 0;
  for (u32 i = 0; i < 50000; ++i) {
    testIteration = i;
    if (i % 8 == 0) { ref.clear(); out.clear(); referenceMemory.fill(0xa7); subjectMemory.fill(0xd3); }
    gfx::alignment = std::array<u32, 6>{0, 1, 16, 64, 256, 512}[(i / 8) % 6];
    auto info = configure(i);
    for (auto& r : ranges.vaRanges) r = {next(), next()};
    const u32 vertex = next();
    g_gxState.stateDirty = true;
    testPhase = "reference";
    auto a = reference::build_uniform(info, vertex, ranges);
    assert(!g_gxState.stateDirty);
    // This catches mistakes caused by rounded ShaderInfo capacity vs raw size.
    info.uniformSize = gfx::alignment ? (a.size + gfx::alignment - 1) & ~(gfx::alignment - 1) : a.size;
    g_gxState.stateDirty = true;
    testPhase = "subject";
    auto b = subject::build_uniform(info, vertex, ranges);
    assert(!g_gxState.stateDirty);
    assert(a == b && ref.size() == out.size());
    if (negative && i == 3) out.data()[b.offset + 80] ^= 1;
    assert(std::memcmp(ref.data() + a.offset, out.data() + b.offset, a.size) == 0);
    previous[i & 7] = a;
    // Compare all prior payloads after appending; alignment gaps aren't shader data.
    for (u32 j = 0; j <= (i & 7); ++j) {
      const auto r = previous[j];
      assert(std::memcmp(ref.data() + r.offset, out.data() + r.offset, r.size) == 0);
    }
    assert(referenceMemory.front() == 0xa7 && referenceMemory.back() == 0xa7);
    assert(subjectMemory.front() == 0xd3 && subjectMemory.back() == 0xd3);
    checkedBytes += a.size;
  }
  gfx::frame = nullptr; referenceFrame = nullptr;
  auto info = configure(0);
  g_gxState.stateDirty = true;
  assert(subject::build_uniform(info, 0, ranges) == gfx::Range{});
  assert(!g_gxState.stateDirty);
  // Owned buffers may grow: committed contents must survive reallocations.
  ByteBuffer owned;
  testPhase = "owned growth";
  for (u32 i = 0; i < 10000; ++i) { gfx::UniformWriter w(owned, 32, 16); w.append(i); auto r = w.finish(); assert(r.offset == i * 16 && r.size == 4); }
  for (u32 i = 0; i < 10000; ++i) { u32 v; std::memcpy(&v, owned.data() + i * 16, 4); assert(v == i); }
  printf("50,000 full serializers, %llu payload bytes identical; 10,000 growth/lifetime checks\n", static_cast<unsigned long long>(checkedBytes));
}

static void benchmark(u32 workload) {
  using namespace aurora; using namespace aurora::gx;
  std::array<u8, 65536> memory{};
  ByteBuffer frame(memory.data(), memory.size()); referenceFrame = &frame; gfx::frame = &frame;
  auto info = configure(23); BindGroupRanges ranges;
  if (workload < 2) {
    info.usesTexMtx.reset(); info.usesPTTexMtx.reset(); info.usedIndTexMtxs.reset();
    info.loadsTevReg.reset(); info.sampledKColors.reset(); info.sampledColorChannels.reset();
    info.sampledTextures = 1; info.usesFog = false; info.lineMode = 0;
    info.lightingEnabled = workload != 0;
  }
  // Finite inputs keep math-library exceptional paths out of the timing sample.
  for (auto& t : textures) { t.ref = nullptr; t.texObj.bias = 0; }
  for (auto& m : g_gxState.indTexMtxs) m.scaleExp = 0;
  auto first = reference::build_uniform(info, 0, ranges);
  info.uniformSize = (first.size + 255) & ~255;
  volatile u32 consume = 0;
  for (int pair = 0; pair < 12; ++pair) {
    for (int phase = 0; phase < 2; ++phase) {
      bool direct = (phase ^ (pair & 1)) != 0;
      auto start = std::chrono::steady_clock::now();
      for (u32 i = 0; i < 100000; ++i) {
        if ((i & 7) == 0) frame.clear();
        auto r = direct ? subject::build_uniform(info, i, ranges) : reference::build_uniform(info, i, ranges);
        u32 v; std::memcpy(&v, frame.data() + r.offset, 4); consume = v;
      }
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
      printf("workload=%u pair=%d direct=%d ns_per_uniform=%.3f raw_bytes=%u\n", workload, pair, direct, ns / 100000.0, first.size);
    }
  }
  assert(consume == 99999);
}

int main(int argc, char** argv) {
  using namespace aurora;
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "bench") { for (u32 w = 0; w < 3; ++w) benchmark(w); return 0; }
  if (mode.empty() || mode == "negative") { oracle(mode == "negative"); return 0; }
  std::array<u8, 64> memory{}; ByteBuffer target(memory.data(), memory.size());
  if (mode == "invalid") { gfx::UniformWriter w; w.finish(); }
  if (mode == "alignment") { gfx::UniformWriter w(target, 16, 3); }
  if (mode == "borrowed-capacity") { gfx::UniformWriter w(target, 65, 16); }
  gfx::UniformWriter w(target, 16, 16);
  if (mode == "overflow") w.append_zeroes(17);
  if (mode == "interleaved") { target.append<u32>(1); w.finish(); }
  if (mode == "double-finish") { w.finish(); w.finish(); }
  if (mode == "after-finish") { w.append<u32>(1); w.finish(); w.append<u32>(2); }
  assert(false);
}
