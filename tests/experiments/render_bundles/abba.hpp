#pragma once
#include <cstdint>

namespace mp6_bundle_abba {
constexpr uint32_t PhaseFrames = 240, SettleFrames = 24;
constexpr uint32_t UsedFrames = PhaseFrames - SettleFrames;
inline bool enabled(uint32_t index) {
  const auto phase = (index / PhaseFrames) % 4;
  return phase == 1 || phase == 2;
}
struct Sample {
  uint32_t index = 0;
  uint64_t draws = 0, hits = 0, builds = 0, encodeNs = 0;
};
struct Window {
  uint32_t phase = UINT32_MAX, frames = 0, previousIndex = 0;
  uint64_t draws = 0, hits = 0, builds = 0, encodeNs = 0;
  int64_t firstNs = 0, lastNs = 0;
  bool valid = false;
  bool observe(const Sample& sample, int64_t presentNs) {
    const auto next = sample.index / PhaseFrames;
    const auto offset = sample.index % PhaseFrames;
    if (next != phase) { *this = {}; phase = next; valid = true; }
    if (offset < SettleFrames) {
      previousIndex = sample.index;
      lastNs = presentNs;
      return false;
    }
    if (!frames) {
      firstNs = presentNs;
      if (offset != SettleFrames || presentNs <= 0) valid = false;
      if (lastNs > 0 && previousIndex + 1 == sample.index && presentNs <= lastNs) valid = false;
    } else if (sample.index != previousIndex + 1 || presentNs <= lastNs ||
               presentNs - lastNs > 250000000) valid = false;
    if (presentNs <= 0 || sample.hits + sample.builds > sample.draws) valid = false;
    ++frames;
    previousIndex = sample.index;
    lastNs = presentNs;
    draws += sample.draws; hits += sample.hits; builds += sample.builds;
    encodeNs += sample.encodeNs;
    if (offset != PhaseFrames - 1) return false;
    valid = valid && frames == UsedFrames && firstNs > 0 && lastNs > firstNs && draws > 0;
    return true;
  }
};
} // namespace mp6_bundle_abba
