#pragma once
#include <cstdint>

// Private experiment only. A phase contains 600 recorded frames; the first
// 60 are settling frames. ABBA ordering reduces monotonic temperature bias.
namespace aurora::gfx {
struct ReadyDiagFrame {
  uint32_t index = 0;
  uint32_t calls = 0;
  uint32_t hits = 0;
  uint64_t lookupNs = 0;
};
inline bool ready_diag_enabled(uint32_t index) {
  const auto phase = (index / 600) % 4;
  return phase == 1 || phase == 2;
}
ReadyDiagFrame ready_diag_snapshot();

struct ReadyDiagWindow {
  uint32_t phase = UINT32_MAX, frames = 0, previousIndex = 0;
  uint64_t calls = 0, hits = 0, lookupNs = 0;
  int64_t firstNs = 0, lastNs = 0;
  bool valid = false;

  // Called exactly once per completed packet, on the render worker. Timestamp
  // zero means its callback did NOT successfully present. Never count queued
  // or merely recorded frames as successful presents.
  bool observe(const ReadyDiagFrame& frame, int64_t presentNs) {
    const auto nextPhase = frame.index / 600;
    const auto offset = frame.index % 600;
    if (nextPhase != phase) {
      *this = {};
      phase = nextPhase;
      valid = true;
    }
    if (offset < 60) return false;
    if (frames == 0) {
      firstNs = presentNs;
      if (offset != 60 || presentNs <= 0) valid = false;
    } else if (frame.index != previousIndex + 1 || presentNs <= lastNs ||
               presentNs - lastNs > 250000000) {
      valid = false;
    }
    if (presentNs <= 0) valid = false;
    ++frames;
    previousIndex = frame.index;
    lastNs = presentNs;
    calls += frame.calls;
    hits += frame.hits;
    lookupNs += frame.lookupNs;
    if (offset != 599) return false;
    valid = valid && frames == 540 && lastNs > firstNs && calls > 0 && hits <= calls;
    return true;
  }
};
} // namespace aurora::gfx
