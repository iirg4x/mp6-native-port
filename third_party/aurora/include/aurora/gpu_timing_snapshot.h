#pragma once

#include <aurora/aurora.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Request-time statistics from one worker-owned history ring (up to 120 frames).
// stats retains AuroraGpuStats status/count/dropped semantics: a clean complete
// window requires status == 2, sampleCount == 120 and droppedFrames == 0.
// droppedFrames is cumulative for this enabled generation, not reset by requests.
// span is first-to-last GPU timestamps, not submission/presentation wall time.
// AO sums timestamped passes named exactly "GTAO" or prefixed "AO "/"XeGTAO ".
// It excludes integrated EFB composite, generic Depth snapshot, copies and the
// between-pass residual; it is NOT complete AO cost. Percentiles use this sum,
// never the sum of per-pass percentiles. p95 uses the nearest-rank convention.
typedef struct AuroraGpuTimingSnapshot {
  uint64_t requestId, generation;
  AuroraGpuStats stats;
  double spanMedianMs, spanP95Ms, betweenMedianMs, betweenP95Ms;
  double aoAverageMs, aoMaxMs, aoMedianMs, aoP95Ms;
} AuroraGpuTimingSnapshot;

// Thread-safe, asynchronous, latest request wins. A nonzero monotonically
// increasing ID queues CPU aggregation at a frame boundary; 0 means unavailable
// (unsupported, disabled, Tracy, or exhausted IDs). Never waits for GPU readback.
// Generation changes, shutdown, failed readback and collapsed pass inventories
// invalidate results. Callers must bound polling; a cancelled ID never completes.
uint64_t aurora_gpu_timing_snapshot_request(void);

// True only for a completed latest ID in the current enabled generation. On
// false, *out is zeroed if nonnull; false also covers pending/cancelled requests.
bool aurora_gpu_timing_snapshot_read(uint64_t requestId, AuroraGpuTimingSnapshot* out);

#ifdef __cplusplus
}
#endif
