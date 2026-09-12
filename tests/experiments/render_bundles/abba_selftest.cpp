#include "abba.hpp"
#include <cstdio>
#include <cstdlib>
using namespace mp6_bundle_abba;
static unsigned checks = 0;
static void require(bool x) { ++checks; if (!x) std::abort(); }
static Sample sample(uint32_t i) { return {i, 100, enabled(i) ? 90u : 0u, 0, 10000}; }
int main() {
  Window window;
  for (uint32_t i = 0; i < PhaseFrames * 100; ++i) {
    const auto done = window.observe(sample(i), int64_t(i+1)*5000000);
    require(done == (i % PhaseFrames == PhaseFrames-1));
    require(enabled(i) == ((i/PhaseFrames)%4 == 1 || (i/PhaseFrames)%4 == 2));
    if (done) {
      require(window.valid && window.frames == UsedFrames);
      require(window.draws == UsedFrames*100);
      require(window.encodeNs == UsedFrames*10000);
      require(window.lastNs-window.firstNs == int64_t(UsedFrames-1)*5000000);
      require(window.hits == UsedFrames*(enabled(i) ? 90u : 0u));
    }
  }
  // Every possible failed present or omitted frame invalidates its phase.
  for (uint32_t bad = SettleFrames; bad < PhaseFrames; ++bad) {
    for (unsigned kind = 0; kind < 5; ++kind) {
      Window test;
      for (uint32_t i = 0; i < PhaseFrames; ++i) {
        if (kind == 0 && i == bad) continue;
        int64_t stamp = int64_t(i+1)*5000000;
        auto value = sample(i);
        if (i == bad) {
          if (kind == 1) stamp = 0;
          if (kind == 2) stamp += 500000000;
          if (kind == 3) stamp -= 5000000;
          if (kind == 4) value.hits = value.draws + 1;
        }
        const bool done = test.observe(value,stamp);
        require(!done || !test.valid);
      }
    }
  }
  Window partial;
  for (uint32_t i = SettleFrames+1; i < PhaseFrames; ++i)
    if (partial.observe(sample(i),int64_t(i+1)*5000000)) require(!partial.valid);
  Window empty;
  for (uint32_t i = 0; i < PhaseFrames; ++i)
    if (empty.observe({i},int64_t(i+1)*5000000)) require(!empty.valid);
  std::printf("PASS: %u bundle ABBA accounting checks\n",checks);
}
