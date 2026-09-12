/* MP6 native port -- PULL-SIDE snapshot accessors over diagnostics that
 * already exist.
 *
 * Every number reachable through this header was already being maintained,
 * unconditionally, by the subsystem that owns it: the five HuMem heaps'
 * used/block totals, the OS arena's bump cursors, the coroutine pool's slot
 * occupancy, the 16 SFX voices and 8 stream channels, the W01 board's tick and
 * draw counters, the event bus's per-key latest values, and (when present)
 * board integration-seam call counts. What did NOT exist was a way to READ them
 * without also printing them -- every one of these was reachable only through
 * an env-gated printf, so a live view had to either turn a log stream on or
 * duplicate the counting.
 *
 * So this header adds no measurement. It adds accessors, and the rules that
 * make them safe to call from a frame boundary:
 *
 *   - COPY UNDER THE LOCK, FORMAT AFTER. mp6_diag_audio_snapshot() takes the
 *     mixer lock, memcpy-shaped-copies the voice/channel state, and releases
 *     it before anything is formatted. src/audio/msm_bridge.c states the
 *     rule plainly at its own census: printf must never run inside the lock
 *     the SDL audio callback needs.
 *   - NO ALLOCATION. Every accessor fills caller storage or returns a pointer
 *     into a fixed table.
 *   - NO SIDE EFFECTS. Reading a probe must never arm, reset or throttle the
 *     thing it reports.
 *
 * Plain C with no decomp types on purpose (int/unsigned/const char *), so the
 * console TUs can consume it under either flag set.
 */
#ifndef MP6_DIAG_PROBE_H
#define MP6_DIAG_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * HuMem heaps -- src/os/malloc_direct.c.
 * The same HeapTbl loop [ALLOC-CENSUS] already walks, plus the capacity
 * from HeapSizeTbl and the largest free block from HuMemMaxMemorySizeGet.
 * ------------------------------------------------------------------- */
#define MP6_DIAG_HEAP_MAX 5

typedef struct {
    const char *name;         /* HEAP_HEAP, HEAP_SOUND, ... (never NULL) */
    int         initialized;  /* 0 = the heap was never created (HEAP_SPACE) */
    unsigned    capacity;     /* HeapSizeTbl bytes */
    int         used;         /* HuMemUsedMemorySizeGet */
    int         blocks;       /* HuMemUsedMemoryBlockGet */
    int         largestFree;  /* HuMemMaxMemorySizeGet */
} Mp6DiagHeap;

int mp6_diag_heap_count(void);
int mp6_diag_heap(int index, Mp6DiagHeap *out); /* 1 = filled */

/* ---------------------------------------------------------------------
 * OS arena heaps -- src/os/arena.c's bump table.
 * ------------------------------------------------------------------- */
typedef struct {
    int                valid;
    const void        *cur;
    const void        *end;
    unsigned long long remaining;
} Mp6DiagArenaHeap;

int mp6_diag_arena_count(void);
int mp6_diag_arena_current(void); /* __OSCurrHeap, or -1 */
int mp6_diag_arena(int index, Mp6DiagArenaHeap *out);

/* ---------------------------------------------------------------------
 * Audio -- src/audio/msm_bridge.c. One snapshot call, taken under the
 * mixer lock; the caller formats after it returns.
 * ------------------------------------------------------------------- */
/* Storage bound only -- it must cover the LARGEST voice-table size the mixer
 * can be configured with (msm_safe.h's MP6_MSM_SFX_VOICES_EXTENDED), because
 * the snapshot is a fixed struct. How many entries are actually populated in
 * a given run is Mp6DiagAudio.voiceCount below, which carries the run's live
 * cap; a 16-slot run fills 16 and leaves the rest zeroed. */
#define MP6_DIAG_SFX_VOICE_MAX 32
#define MP6_DIAG_CHAN_MAX       8

typedef struct {
    int      active;
    int      no;        /* runtime handle */
    int      seId;
    int      loop;
    int      keyGroup;
    unsigned wraps;     /* mixer loop-wraps: still sounding with no new play */
    unsigned long long startTick;
} Mp6DiagSfxVoice;

typedef struct {
    int   active;
    int   paused;
    int   loop;
    int   streamId;
    int   fadeAction;   /* 0 none, 1 to-pause, 2 to-stop, 3 to-play */
    int   vol;
    float fadeMul;
} Mp6DiagChan;

typedef struct {
    int voiceCount;      /* the run's LIVE mixer voice-slot count (16 or 32),
                          * clamped to MP6_DIAG_SFX_VOICE_MAX; the number of
                          * `voices[]` entries below that carry meaning */
    int voicesActive;
    int chanCount;       /* MP6_DIAG_CHAN_MAX */
    int chanConfigured;  /* g_chanMax -- how many the .pdt actually declares */
    int chansActive;
    int masterVol;
    int seMasterVol;
    unsigned long keygroupEvents;   /* key-group release CALLS that freed >=1 voice */
    unsigned long keygroupVoices;   /* voices freed by them, cumulative */
    Mp6DiagSfxVoice voices[MP6_DIAG_SFX_VOICE_MAX];
    Mp6DiagChan     chans[MP6_DIAG_CHAN_MAX];
} Mp6DiagAudio;

void mp6_diag_audio_snapshot(Mp6DiagAudio *out);

/* ---------------------------------------------------------------------
 * Board -- src/os/board_runtime.c's live counters (today they feed one
 * one-shot [W01] line each and are otherwise unreadable).
 * ------------------------------------------------------------------- */
int      mp6_diag_board_no(void);
unsigned mp6_diag_board_ticks(void);
unsigned mp6_diag_board_draws(void);

/* ---------------------------------------------------------------------
 * Board placeholder seams -- src/os/board_placeholders.c.
 *
 * The pinned decomp revision currently implements every former board seam,
 * so this registry is empty. Keep the stable probe ABI: diagnostics consumers
 * can report zero seams without conditional compilation, and a future
 * deliberately logged seam can restore entries without changing that ABI.
 * ------------------------------------------------------------------- */
#define MP6_DIAG_SEAM_MAX 128

int mp6_diag_seam_count(void);
/* Any out pointer may be NULL. Returns 1 when index is in range. */
int mp6_diag_seam(int index, const char **symbol, const char **result,
                  unsigned long *count);

/* ---------------------------------------------------------------------
 * Event bus -- src/os/mp6_events.c.
 *
 * The slot table keeps only each key's LATEST value plus a count, so there
 * was no chronological view at all. The tail ring below is appended right
 * where the [EVENT] printf already is, so it costs one snprintf into a fixed
 * buffer per event and cannot disagree with the log.
 * ------------------------------------------------------------------- */
#define MP6_DIAG_EVENT_TAIL 64

int         mp6_diag_event_tail_count(void);
const char *mp6_diag_event_tail(int back); /* 0 = newest; NULL past the end */

int mp6_diag_event_slot_count(void);
int mp6_diag_event_slot(int index, const char **key, const char **sval,
                        long *nval, unsigned long *count, unsigned long *seq);

/* ---------------------------------------------------------------------
 * HuPrc scheduler -- src/os/process_native.c.
 *
 * processcnt is maintained by HuPrcCreate/gcTerminateProcess on every path and
 * has never had a reader; the coroutine table beside it is what actually
 * bounds how many processes can exist at once, so both are worth seeing next
 * to each other when a screen stops responding.
 * ------------------------------------------------------------------- */
int mp6_diag_process_count(void);  /* HUPROCESS nodes linked right now */
int mp6_diag_process_fibers(void); /* coroutine slots the scheduler has claimed */
int mp6_diag_process_fiber_max(void);

/* ---------------------------------------------------------------------
 * OM object walk -- compat/decomp/src/game/objmain.c.patch.
 *
 * The four verdict tallies omMain already computes (ran / objFunc-NULL /
 * gated by a stat bit / deleted-but-still-listed) are now maintained
 * unconditionally, with only the [OM-CENSUS] lines still behind
 * MP6_OM_CENSUS. Reports the LAST COMPLETE walk -- the counters are zeroed at
 * the top of every pass, so sampling them live could report an object count
 * that never existed.
 * ------------------------------------------------------------------- */
void mp6_diag_om_tallies(int *ran, int *funcNull, int *gated, int *dead);
long mp6_diag_om_passes(void);

/* ---------------------------------------------------------------------
 * Unlocked FPS -- src/gx/frame_interp.c. WINDOWED BUILD ONLY (that TU
 * is PLATFORM_AURORA_ONLY); callers must guard with MP6_HEADLESS_BUILD.
 *
 * frame_interp.c already keeps a PERMANENT decline census and cost
 * accounting: s_declCount, s_declSlack*, s_cost*, s_replayBudgetNs are
 * incremented on every path whether or not MP6_FI_DIAG prints, and
 * [MP6-FI-CENSUS] merely formats them. Anything that wants those numbers must
 * READ them here rather than count again -- a second copy could disagree with
 * the census line about why a window produced no interpolated present, which
 * is the one question the census exists to answer.
 *
 * Values are CUMULATIVE (the fields the census log resets on its own 300-seal
 * window are noted below); a caller wanting a rate takes its own differences
 * and must tolerate a reset by clamping a negative delta to zero.
 * ------------------------------------------------------------------- */
#define MP6_FI_DECL_MAX 16

typedef struct {
    int  active;            /* the capture sink is registered */
    int  declCount;         /* how many decline reasons exist */
    long decl[MP6_FI_DECL_MAX];
    /* idle room seen at the entry admission check (census-window scoped) */
    long long slackMinNs, slackMaxNs, slackSumNs;
    long      slackSamples;
    long long budgetNs;     /* the admission estimator's current value */
    /* replay cost split (census-window scoped) */
    long long costBuildSumNs, costBuildMaxNs;
    long long costPresentSumNs, costPresentMaxNs;
    long      costSamples;
    long      seals, sealWalkFail, sealNoPrev;
    /* cumulative for the process */
    long      statReplays, statSnaps, statSkipped;
    /* the latest sealed stream */
    unsigned  streamBytes, chunkCount, posCount;
    int       walkOk;
} Mp6FiStats;

void        mp6_fi_stats_get(Mp6FiStats *out);
const char *mp6_fi_decl_name(int index); /* "inactive", "dl-built", "ok", ... */

#ifdef __cplusplus
}
#endif

#endif /* MP6_DIAG_PROBE_H */
