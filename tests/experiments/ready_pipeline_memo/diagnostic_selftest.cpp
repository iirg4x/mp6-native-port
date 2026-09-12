#include "diagnostic.hpp"
#include <cassert>
#include <cstdio>
using namespace aurora::gfx;
int main() {
  ReadyDiagWindow w;
  int completed = 0;
  for (uint32_t i = 0; i < 2400; ++i) {
    assert(ready_diag_enabled(i) == (i >= 600 && i < 1800));
    if (w.observe({i, 100, ready_diag_enabled(i) ? 90u : 0u, 10000},
                  (int64_t(i) + 1) * 5000000)) {
      ++completed;
      assert(w.valid && w.frames == 540 && w.calls == 54000);
      assert(w.lastNs - w.firstNs == 539 * 5000000LL);
    }
  }
  assert(completed == 4);
  for (int fault = 0; fault < 5; ++fault) {
    w = {};
    for (uint32_t i = fault == 4 ? 70 : 0; i < 600; ++i) {
      if (fault == 0 && i == 100) continue;
      int64_t stamp = (int64_t(i) + 1) * 5000000;
      if (fault == 1 && i == 100) stamp = 0; // failed present
      if (fault == 2 && i >= 100) stamp += 1000000000; // pause
      if (fault == 3 && i == 100) stamp -= 5000000; // duplicate time
      if (w.observe({i,100,90,10000}, stamp)) assert(!w.valid);
    }
  }
  std::puts("PASS: ABBA phase ownership; actual-present intervals; missing/failed/paused/partial windows rejected");
}
