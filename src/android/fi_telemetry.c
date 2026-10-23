/* MP6 native port -- Unlocked FPS test telemetry (Android test builds only).
 *
 * Compiled to nothing unless MP6_FI_TELEMETRY is defined, which only the
 * local test-APK wrapper does; release builds never carry it.
 *
 * frame_interp.c records every presented frame (real tick frames and
 * replays): its kind, tick, overlay, the tick's seal time, when its work
 * started and finished, and the alpha its motion was evaluated at. Records
 * are buffered and appended to fi-telemetry.txt in the app's external
 * files directory (Android/data/com.mp6.game/files), and a one-line summary
 * goes to logcat (tag MP6-FI) every 300 ticks. A PC reads both over adb; nothing is sent anywhere by the app.
 */
#include <stdint.h>

#ifdef MP6_FI_TELEMETRY
#include <android/log.h>
#include <aurora/aurora.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "mp6_host_section.h" /* statics are host state (tools/build.py carve-out) */

#ifdef MP6_FI_TELEMETRY

typedef struct {
    int64_t sealNs, t0Ns, endNs;
    uint64_t seq; /* aurora_end_frame_seq() of this frame (build/0009) */
    float alpha;
    int32_t tick;
    int16_t ovl;
    uint8_t kind; /* 0 tick frame, 1 FIFO replay, 2 decoded replay, 3 reused replay, 4 last decline */
    uint8_t interp;
} TelRec;

#define TEL_BATCH 4096
static TelRec s_batch[TEL_BATCH];
static unsigned s_count;
static FILE *s_file;
static int s_openTried;
static FILE *s_presentFile;

static uint64_t s_seq;
static long s_windowTick = -1;
static unsigned s_windowPresents, s_windowKinds[4];
const char *SDL_GetAndroidExternalStoragePath(void); /* SDL3: the app's external files dir */
extern int omcurovl; /* game/objmain.c (OMOVL, an int-sized enum) */
extern void mp6_render_res_get(int *winW, int *winH, int *renderW, int *renderH); /* aurora_bridge.c */

/* build/0009: when the render worker actually presented each end_frame seq. */
static void tel_present_drain(void)
{
    static uint64_t seqs[1024];
    static int64_t ns[1024];
    int n, i;
    if (s_presentFile == NULL) {
        const char *dir = SDL_GetAndroidExternalStoragePath();
        char path[512];
        if (dir == NULL) return;
        snprintf(path, sizeof(path), "%s/fi-present.txt", dir);
        s_presentFile = fopen(path, "w");
        if (s_presentFile == NULL) return;
        fprintf(s_presentFile, "# seq presentNs\n");
    }
    while ((n = aurora_present_log_drain(seqs, ns, 1024)) > 0) {
        for (i = 0; i < n; ++i)
            fprintf(s_presentFile, "%llu %lld\n", (unsigned long long)seqs[i], (long long)ns[i]);
        if (n < 1024) break;
    }
    fflush(s_presentFile);
}

/* GPU breakdown + render-scale sweep. aurora's per-pass GPU timestamps are
 * read every 300 ticks into fi-gpu.txt; the render scale cycles through
 * kSweep every 1200 ticks (20 s) so the same scenes are measured at several
 * resolutions. Windows right after a switch contain both scales and are
 * marked so the analysis can drop them. */
/* 40025 measured no FPS change from 100% to 50% on a Galaxy S22+, so the
 * sweep is off; list more scales here to measure again. */
static const float kSweep[] = {1.0f};
static FILE *s_gpuFile;
static long s_gpuTick = -1, s_sweepTick = -1;
static int s_sweepIdx, s_gpuMixed;
static unsigned s_gpuPresents;

static void tel_gpu(long tick)
{
    AuroraGpuStats st;
    int winW = 0, winH = 0, rW = 0, rH = 0, i;
    if (s_gpuTick < 0) {
        const char *dir = SDL_GetAndroidExternalStoragePath();
        char path[512];
        aurora_gpu_stats_set_enabled(true);
        s_gpuTick = s_sweepTick = tick;
        if (dir != NULL) {
            snprintf(path, sizeof(path), "%s/fi-gpu.txt", dir);
            s_gpuFile = fopen(path, "w");
        }
        return;
    }
    if (tick - s_sweepTick >= 1200) {
        s_sweepIdx = (s_sweepIdx + 1) % (int)(sizeof(kSweep) / sizeof(kSweep[0]));
        aurora_set_render_scale(kSweep[s_sweepIdx]);
        s_sweepTick = tick;
        s_gpuMixed = 1;
    }
    if (tick - s_gpuTick < 300) return;
    memset(&st, 0, sizeof(st));
    aurora_gpu_stats_read(&st);
    mp6_render_res_get(&winW, &winH, &rW, &rH);
    __android_log_print(ANDROID_LOG_INFO, "MP6-FI",
                        "gpu tick=%ld ovl=%d scale=%.2f render=%dx%d presents/tick=%.2f span=%.3fms (max %.3f) status=%u n=%u%s",
                        tick, omcurovl, kSweep[s_sweepIdx], rW, rH,
                        (double)s_gpuPresents / (double)(tick - s_gpuTick), st.spanAverageMs, st.spanMaxMs,
                        st.status, st.sampleCount, s_gpuMixed ? " mixed" : "");
    if (s_gpuFile != NULL) {
        fprintf(s_gpuFile, "W tick=%ld ovl=%d scale=%.2f win=%dx%d render=%dx%d presents=%u ticks=%ld status=%u "
                           "samples=%u dropped=%u span_avg=%.4f span_max=%.4f between_avg=%.4f between_max=%.4f mixed=%d\n",
                tick, omcurovl, kSweep[s_sweepIdx], winW, winH, rW, rH, s_gpuPresents, tick - s_gpuTick,
                st.status, st.sampleCount, st.droppedFrames, st.spanAverageMs, st.spanMaxMs,
                st.betweenAverageMs, st.betweenMaxMs, s_gpuMixed);
        for (i = 0; i < (int)st.rowCount && i < AURORA_GPU_STAT_ROWS; ++i)
            fprintf(s_gpuFile, "P %s|%.4f|%.4f\n", st.rows[i].name, st.rows[i].averageMs, st.rows[i].maxMs);
        fflush(s_gpuFile);
    }
    s_gpuTick = tick;
    s_gpuPresents = 0;
    s_gpuMixed = 0;
}

static void tel_flush(void)
{
    unsigned i;
    if (!s_openTried) {
        const char *dir = SDL_GetAndroidExternalStoragePath();
        char path[512];
        s_openTried = 1;
        if (dir != NULL) {
            snprintf(path, sizeof(path), "%s/fi-telemetry.txt", dir);
            s_file = fopen(path, "w");
            if (s_file) fprintf(s_file, "# seq kind tick ovl sealNs t0Ns endNs alpha interp frameSeq\n");
            __android_log_print(ANDROID_LOG_INFO, "MP6-FI", "telemetry file %s %s", path,
                                s_file ? "open" : "FAILED");
        }
    }
    if (s_file != NULL) {
        for (i = 0; i < s_count; ++i) {
            const TelRec *r = &s_batch[i];
            fprintf(s_file, "%llu %u %d %d %lld %lld %lld %.5f %u %llu\n", (unsigned long long)(s_seq + i),
                    r->kind, r->tick, r->ovl, (long long)r->sealNs, (long long)r->t0Ns,
                    (long long)r->endNs, r->alpha, r->interp, (unsigned long long)r->seq);
        }
        fflush(s_file);
    }
    s_seq += s_count;
    s_count = 0;
}

void mp6_fi_tel_record(int kind, long tick, int64_t sealNs, int64_t t0Ns, int64_t endNs,
                       double alpha, int interp)
{
    TelRec *r = &s_batch[s_count++];
    r->kind = (uint8_t)kind;
    r->tick = (int32_t)tick;
    r->ovl = (int16_t)omcurovl;
    r->sealNs = sealNs;
    r->t0Ns = t0Ns;
    r->endNs = endNs;
    r->alpha = (float)alpha;
    r->interp = (uint8_t)interp;
    r->seq = aurora_end_frame_seq();
    if (kind == 4) { /* decline record, not a present */
        if (s_count == TEL_BATCH) tel_flush();
        return;
    }
    s_windowPresents++;
    s_gpuPresents++;
    s_windowKinds[kind & 3]++;
    if (kind == 0) {
        tel_gpu(tick);
        if (s_windowTick < 0) s_windowTick = tick;
        if ((tick & 31) == 0) tel_present_drain();
        if (tick - s_windowTick >= 300) {
            __android_log_print(ANDROID_LOG_INFO, "MP6-FI",
                                "tick=%ld ovl=%d presents=%u (tick %u fifo %u decoded %u reused %u)",
                                tick, omcurovl, s_windowPresents, s_windowKinds[0], s_windowKinds[1],
                                s_windowKinds[2], s_windowKinds[3]);
            s_windowTick = tick;
            s_windowPresents = 0;
            memset(s_windowKinds, 0, sizeof(s_windowKinds));
        }
        /* Write on tick frames only, never inside the replay window. */
        if (s_count >= TEL_BATCH - 1024) tel_flush();
    } else if (s_count == TEL_BATCH) {
        tel_flush();
    }
}
#else
typedef int mp6_fi_telemetry_disabled;
#endif
