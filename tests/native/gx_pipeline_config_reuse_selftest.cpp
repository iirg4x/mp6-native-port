// Native fixture template for the replayed GX PipelineConfig memo.
//
// The Python replay test injects the actual declaration/body from the pinned
// post-0081 command_processor.cpp between the markers below.  This file does
// not contain a second hand-written implementation of that helper.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using u32 = std::uint32_t;

namespace {

enum class GXPrimitive : u32 { Triangles = 0, Lines = 1, Points = 2 };
enum class GXVtxFmt : u32 { Position = 0, Color = 1, Texture = 2 };

// A small deterministic stand-in for the production value.  No production
// GPU/resource type is retained by this fixture; only helper key/value
// behavior is exercised.
struct PipelineConfig {
  u32 version = 14;
  u32 msaaSamples = 1;
  u32 stageCount = 0;
  std::array<u32, 8> stages{};
  bool alphaCompareEnabled = false;
  u32 alphaCompareRef = 0;
  GXPrimitive prim = GXPrimitive::Triangles;
  GXVtxFmt fmt = GXVtxFmt::Position;

  bool operator==(const PipelineConfig& rhs) const {
    return version == rhs.version && msaaSamples == rhs.msaaSamples &&
           stageCount == rhs.stageCount && stages == rhs.stages &&
           alphaCompareEnabled == rhs.alphaCompareEnabled &&
           alphaCompareRef == rhs.alphaCompareRef && prim == rhs.prim &&
           fmt == rhs.fmt;
  }
};

static u32 gFrame = 0;
static u32 gSamples = 1;
static u32 gPopulateCalls = 0;

namespace gfx {
u32 current_frame() { return gFrame; }
u32 get_sample_count() { return gSamples; }
} // namespace gfx

// This deterministic provider deliberately grows/shrinks the value so a miss
// must clear trailing stages and optional alpha data before rebuilding.
void populate_pipeline_config(PipelineConfig& config, GXPrimitive prim,
                              GXVtxFmt fmt) {
  ++gPopulateCalls;
  config.version = 14;
  config.msaaSamples = gSamples;
  config.prim = prim;
  config.fmt = fmt;
  config.stageCount = prim == GXPrimitive::Triangles ? 8u : 1u;
  for (u32 i = 0; i < config.stageCount; ++i) {
    config.stages[i] = 0x1000u + static_cast<u32>(prim) * 0x100u + i;
  }
  config.alphaCompareEnabled = fmt == GXVtxFmt::Color;
  config.alphaCompareRef = config.alphaCompareEnabled ? 0x5Au : 0u;
}

// BEGIN ACTUAL_PIPELINE_CONFIG_HELPER
// END ACTUAL_PIPELINE_CONFIG_HELPER

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void reset_fixture(u32 frame, u32 samples) {
  gFrame = frame;
  gSamples = samples;
  gPopulateCalls = 0;
  invalidate_pipeline_config();
  sConfigMemo = {};
}

void run_clean_hit_matrix() {
  std::size_t cases = 0;
  for (u32 frame : {0u, 1u, 17u, 4095u}) {
    for (u32 samples : {1u, 2u, 4u}) {
      for (u32 primValue = 0; primValue < 3; ++primValue) {
        for (u32 fmtValue = 0; fmtValue < 3; ++fmtValue) {
          reset_fixture(frame, samples);
          const auto prim = static_cast<GXPrimitive>(primValue);
          const auto fmt = static_cast<GXVtxFmt>(fmtValue);
          const auto first = get_pipeline_config(prim, fmt);
          const auto snapshot = first;
          require(gPopulateCalls == 1, "initial lookup did not populate once");
          const auto& hit = get_pipeline_config(prim, fmt);
          require(gPopulateCalls == 1, "clean key rebuilt instead of hitting");
          require(hit == snapshot, "clean hit changed PipelineConfig");
          ++cases;
        }
      }
    }
  }
  std::cout << "clean_hit_cases=" << cases << '\n';
}

void run_key_and_lifecycle_cases() {
  reset_fixture(10, 1);
  const auto baseline = get_pipeline_config(GXPrimitive::Triangles, GXVtxFmt::Color);
  require(gPopulateCalls == 1, "baseline did not populate");

  invalidate_pipeline_config();
  require(get_pipeline_config(GXPrimitive::Triangles, GXVtxFmt::Color) == baseline,
          "same-key invalidation changed value");
  require(gPopulateCalls == 2, "same-key invalidation did not rebuild");

  gFrame = 11;
  get_pipeline_config(GXPrimitive::Triangles, GXVtxFmt::Color);
  require(gPopulateCalls == 3, "frame change did not rebuild");
  gSamples = 2;
  const auto& sample = get_pipeline_config(GXPrimitive::Triangles, GXVtxFmt::Color);
  require(gPopulateCalls == 4 && sample.msaaSamples == 2,
          "sample change did not rebuild");
  const auto& prim = get_pipeline_config(GXPrimitive::Lines, GXVtxFmt::Color);
  require(gPopulateCalls == 5 && prim.prim == GXPrimitive::Lines,
          "primitive change did not rebuild");
  const auto& fmt = get_pipeline_config(GXPrimitive::Lines, GXVtxFmt::Texture);
  require(gPopulateCalls == 6 && fmt.fmt == GXVtxFmt::Texture,
          "format change did not rebuild");
  std::cout << "key_dimension_misses=4 lifecycle_invalidate=1\n";
}

void run_clear_on_miss() {
  reset_fixture(20, 4);
  const auto& rich = get_pipeline_config(GXPrimitive::Triangles, GXVtxFmt::Color);
  require(rich.stageCount == 8 && rich.alphaCompareEnabled,
          "rich seed did not populate");
  const auto& small = get_pipeline_config(GXPrimitive::Lines, GXVtxFmt::Texture);
  require(small.stageCount == 1 && !small.alphaCompareEnabled,
          "small value did not shrink");
  for (u32 i = 1; i < small.stages.size(); ++i) {
    require(small.stages[i] == 0, "trailing stage retained prior data");
  }
  require(small.alphaCompareRef == 0, "alpha compare retained prior data");
  std::cout << "zero_on_miss_clear=1 trailing_stages=7 alpha_clear=1\n";
}

void run_repeated_lifecycle_cases() {
  std::size_t actions = 0;
  for (u32 i = 0; i < 1000; ++i) {
    const u32 frame = i % 19;
    const u32 samples = 1 + (i % 4);
    const auto prim = static_cast<GXPrimitive>(i % 3);
    const auto fmt = static_cast<GXVtxFmt>((i / 3) % 3);
    reset_fixture(frame, samples);
    const auto first = get_pipeline_config(prim, fmt);
    const auto snapshot = first;
    const auto& hit = get_pipeline_config(prim, fmt);
    require(gPopulateCalls == 1 && hit == snapshot,
            "repeated clean hit rebuilt or changed value");
    ++actions;
    if ((i & 1u) == 0) {
      invalidate_pipeline_config();
    } else {
      ++gFrame;
    }
    const auto& rebuilt = get_pipeline_config(prim, fmt);
    require(gPopulateCalls == 2 && rebuilt == snapshot,
            "repeated lifecycle miss failed");
    ++actions;
  }
  std::cout << "lifecycle_actions=" << actions << " iterations=1000\n";
}

} // namespace

int main() {
  run_clean_hit_matrix();
  run_key_and_lifecycle_cases();
  run_clear_on_miss();
  run_repeated_lifecycle_cases();
  std::cout << "PASS GX pipeline config reuse actual helper\n";
  return 0;
}
