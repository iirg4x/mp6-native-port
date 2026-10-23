/* MP6 native port -- loading trace (MP6_LOAD_TRACE=1).
 *
 * Times the loading work that runs on the game thread (disc reads,
 * decompression, HSF model conversion, sound-group and music decode) and
 * prints, for every tick that spent at least MP6_LOAD_TRACE_TICK_MS on it,
 * one "[MP6-LOAD]" line with the per-kind totals. Portable (it works in
 * Android logcat and the web console, where no sampling profiler exists),
 * and costs one cached flag test per call when unset.
 *
 *     uint64_t t0 = mp6_load_trace_begin();
 *     ... work ...
 *     mp6_load_trace_end(t0, MP6_LOAD_DVD, bytes);
 */
#ifndef MP6_LOAD_TRACE_H
#define MP6_LOAD_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP6_LOAD_DVD,     /* disc file reads */
    MP6_LOAD_DECODE,  /* HuDecodeData decompression */
    MP6_LOAD_HSF,     /* native HSF model conversion */
    MP6_LOAD_SEGROUP, /* sound-effect group load */
    MP6_LOAD_STREAM,  /* music track decode at play time */
    MP6_LOAD_KIND_COUNT
} Mp6LoadKind;

#define MP6_LOAD_TRACE_TICK_MS 8

/* 0 when tracing is off; otherwise the start time to pass to _end. */
uint64_t mp6_load_trace_begin(void);
void mp6_load_trace_end(uint64_t start, Mp6LoadKind kind, uint64_t bytes);
/* Called once per game tick: prints and resets this tick's totals. */
void mp6_load_trace_tick(long tick);

/* src/hsf/hsf_load_native.c, MP6_HSF_PROFILE=1: print and clear the model
 * loader's per-stage times (called after each [MP6-LOAD] line). */
void mp6_hsf_profile_flush(void);

/* MP6_MEM_TRACE=1: print process and C-heap memory with a label. */
void mp6_mem_trace(const char *label, long tick);

#ifdef __cplusplus
}
#endif

#endif /* MP6_LOAD_TRACE_H */
