#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
float aurora_get_fps();

void aurora_enable_vsync(bool enabled);

// Pipeline (shader) build telemetry since the previous call. A stall is a draw
// that waited for its pipeline to be built (on its own thread or a worker).
typedef struct {
  uint32_t stalls;
  double stallMs;
  double stallMaxMs;
  uint32_t built;     // pipelines created on any thread
  double buildMs;     // total creation time across threads
  uint32_t prewarmed; // of those, built ahead of use from a scene's recorded set
  uint32_t uberDraws; // draws rendered by the ubershader while their pipeline built
  uint32_t queued;  // waiting to be built now
  uint32_t resident;
  uint32_t skippedDraws; // draws left out because their pipeline wasn't built (async web builds)
} AuroraPipelineStats;
void aurora_pipeline_stats_take(AuroraPipelineStats* out);

// Scene pipeline sets: the game names the scene it is loading (any stable id,
// such as its overlay number). Pipelines used while a scene is current are
// recorded against it; when that scene loads again its recorded set (or the
// bundled seed's) is built in the background right away, before its first
// frames ask for it. On memory-tight platforms pipelines the new scene doesn't
// use are released.
void aurora_set_pipeline_scene(uint32_t scene);

// The bundled pipeline seed: 1 when it is open with scene sets. The status is
// its seed id, or why it is missing (for telemetry).
int aurora_pipeline_seed_ok(void);
const char* aurora_pipeline_seed_status(void);

// GX ubershader mode: 1 draws through it while a pipeline builds (default),
// 0 waits for specialized pipelines, 2 draws everything through it; -1 goes
// back to the MP6_GX_UBER environment setting.
void aurora_gx_set_uber_mode(int mode);

// First-launch pipeline build. needed: 1 on the first launch on this GPU and
// driver, or with a new seed or ubershader. begin queues every pipeline any
// scene is known to use plus the ubershader states, on extra compile threads
// (natively) or asynchronously (web). Call poll once a frame until it returns
// 0 (done: remembered for the next launch), drawing the progress with draw
// between aurora_begin_frame and aurora_end_frame.
int aurora_pipeline_warmup_needed(void);
void aurora_pipeline_warmup_begin(void);
int aurora_pipeline_warmup_poll(uint32_t* done, uint32_t* total);
void aurora_pipeline_warmup_draw(uint32_t done, uint32_t total);

#ifdef __cplusplus
}
#endif

#endif
