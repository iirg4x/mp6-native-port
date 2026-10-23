#pragma once
#include <cstdlib>

namespace aurora {
// Opt-in process policy, never a saved player setting. Latch before SDL
// initialization so a run cannot acquire hardware halfway through a test.
inline bool background_qa() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("MP6_BACKGROUND_QA");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}
inline constexpr const char* background_qa_marker =
    "[BACKGROUND-QA] hidden window; scripted input only; controllers/haptics disabled";
}
