/* MP6 native port -- cross-session savestates.
 *
 * PURPOSE. Capture the game's whole mid-session execution state to a file
 * and restore it later, in a DIFFERENT process run, so a bug can be handed
 * over as an exact reproducible state instead of a description plus a
 * multi-minute scripted drive. This is a debugging/QA tool, not a
 * player-facing feature -- and it is a completely different thing from the
 * GCI save system (platform/os/save_endian.c), which persists player
 * progress in the on-disc memory-card format. The two share only the
 * English word "save."
 *
 * WHY THIS IS TRACTABLE HERE (and is not, in general, for a native binary).
 * Four properties of this port, each verified rather than assumed:
 *   1. Nearly all mutable game state is in ONE contiguous, fixed-base
 *      reservation -- the 256 MB game arena (platform/os/arena.c) -- plus
 *      the executable's own writable sections. The image is linked at a
 *      fixed base with ASLR off (--image-base/--no-dynamicbase), so those
 *      sections land at the same addresses every run.
 *   2. The arena and the coroutine-stack pool are reserved at explicit,
 *      non-ASLR candidate addresses, and were measured stable across
 *      repeated windowed boots. This is what
 *      lets raw host pointers inside captured memory stay valid in the
 *      restoring process -- and it is a MEASURED property, not a
 *      guarantee, which is exactly why the loader below re-checks every
 *      base address and refuses to load on any mismatch.
 *   3. There is a real frame boundary -- VIWaitForRetrace(), once per tick,
 *      after HuPrcCall(1) has already returned -- at which no HuPrc process
 *      is ever mid-instruction. Capture and restore both happen only there.
 *   4. The OS main thread's own native stack never needs capturing: capture
 *      and restore both re-enter through that same frame-boundary function
 *      and then return normally. Only the separate HuPrc coroutine stacks
 *      hold resumable execution state, and they live in one pool.
 *
 * WHAT IS NOT CAPTURED, AND WHY. Everything owned by the host rather than
 * by the game: the SDL window, the Dawn/WebGPU device and its swapchain,
 * the SDL audio device, host-malloc'd decoded PCM, and OS handles. These
 * are either (a) rebuilt by the restoring process's own normal boot before
 * the state is ever loaded, or (b) a pure cache over game-owned bytes that
 * self-heals -- Aurora's texture cache is content-addressed (it hashes the
 * actual texel bytes), so re-issuing the game's ordinary per-frame draw
 * calls against restored arena data re-uploads anything stale.
 *
 * THE CARVE-OUT (this is the subtle part). A savestate restores the
 * executable's writable sections wholesale. That is correct for game state
 * -- including state held by PLATFORM modules, which is why the split here
 * is NOT "decomp globals vs port globals": platform/os/process_native.c's
 * own scheduler table is port code holding load-bearing game state, and
 * platform/host/coro_arena.c's wrapper array is what that table's entries
 * point AT. Both must be restored, and both restore correctly because their
 * addresses are pinned.
 *
 * What must NOT be restored is state owned by a thread other than the game
 * thread, or handles minted by the OS/driver (different in every process):
 *   - the SDL audio callback thread's mixer statics -- restoring these
 *     races a thread that is running RIGHT NOW, and would also install
 *     stale host-malloc'd PCM pointers;
 *   - content_import.cpp's live std::thread/std::mutex objects -- memcpy
 *     over those is undefined behavior, full stop;
 *   - Aurora/SDL/Dawn handles.
 * Those TUs put their statics in a dedicated section (MP6_HOST_STATE_
 * SECTION) via a force-included pragma header, so the carve-out is a
 * SECTION NAME rather than a hand-maintained list of variables that would
 * silently rot as the code changes. Audio is instead re-synced from
 * restored game state (stop everything, replay what the game says should
 * be playing), which costs a brief audible seam at the cue point.
 */
#ifndef MP6_SAVESTATE_H
#define MP6_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PE sections holding host-owned (non-game-thread / OS-handle) statics.
 * Excluded from both capture and restore. TWO names because zero-initialized
 * and initialized data have different section types and cannot share one
 * (see shim/include/mp6_host_section.h, which is the only thing that should
 * ever place variables here). PE section names cap at 8 bytes; both of these
 * exactly fill that. */
#define MP6_HOST_STATE_SECTION_BSS  ".mp6hbss"
#define MP6_HOST_STATE_SECTION_DATA ".mp6hdat"

/* Bump on ANY layout change to the on-disk format below. The loader
 * refuses a mismatch rather than guessing. */
#define MP6_SAVESTATE_VERSION 3u

#define MP6_SAVESTATE_MAGIC   0x36505336u /* "6PS6" */

/* Result codes. Negative = failure; every failure path leaves the running
 * game untouched (the loader validates the whole header, and decompresses
 * into scratch, before it writes a single byte of live memory). */
typedef enum {
    MP6_SAVESTATE_OK = 0,
    MP6_SAVESTATE_ERR_IO = -1,             /* file could not be read/written */
    MP6_SAVESTATE_ERR_FORMAT = -2,         /* bad magic / truncated / version mismatch */
    MP6_SAVESTATE_ERR_BINARY_MISMATCH = -3,/* different build than the one that captured */
    MP6_SAVESTATE_ERR_LAYOUT_MISMATCH = -4,/* arena/coro/image landed at a different address */
    MP6_SAVESTATE_ERR_UNSUPPORTED = -5,    /* no capturable coroutine pool (fiber backend) */
    MP6_SAVESTATE_ERR_NOMEM = -6
} Mp6SavestateResult;

/* Capture the current state to `path`. MUST be called only from the frame
 * boundary (VIWaitForRetrace, after HuPrcCall has returned) -- calling it
 * anywhere else captures a coroutine mid-instruction and is not supported.
 * Returns MP6_SAVESTATE_OK or a negative Mp6SavestateResult. */
int mp6_savestate_capture(const char *path);

/* Restore state from `path` over the running process. Same frame-boundary
 * requirement. Validates the entire header first and fails closed, leaving
 * the running game untouched, on any mismatch. */
int mp6_savestate_restore(const char *path);

/* Nonzero once a savestate has been restored in THIS process. Consumers
 * use it to skip work that would run over third-party state the restore
 * may have staled -- see mp6_savestate_guarded_exit() below. */
int mp6_savestate_was_restored(void);

/* C15 (review): process exit for EVERY graceful path a restored process can
 * take, not just the windowed tick-budget one. A restore leaves third-party
 * teardown state (static destructors, atexit tables walking restored
 * pointers) untrustworthy; the original guard lived only inside
 * mp6_clean_shutdown_exit, so an OSPanic (PPCHalt -> exit(1)) or the RSS
 * watchdog's exit(1) in a restored session died 0xC0000005 in teardown
 * instead of reporting its own exit code. This helper is the single seam:
 * flush stdio, then _exit(code) when a restore happened (skipping atexit +
 * static destructors), plain exit(code) otherwise. Callers that need extra
 * teardown of their OWN first (aurora_shutdown) do it before calling. */
void mp6_savestate_guarded_exit(int code);

/* Human-readable text for a result code (never NULL). */
const char *mp6_savestate_strerror(int result);

/* ------------------------------------------------------------------
 * Post-restore rehydrate hooks
 * ------------------------------------------------------------------
 * A restore memcpy's captured bytes over live memory and returns. Any
 * subsystem whose HOST-side half was not captured (because it is carved
 * out, or lives on the C runtime heap) is then out of step with the game
 * state that WAS restored, and needs a chance to re-derive it. These are
 * that chance, called in a fixed order from mp6_savestate_restore()
 * immediately after the commit loop.
 *
 * A FIXED CALL LIST, deliberately, not a registration table: a registry
 * would itself be mutable state living in a restored section -- a fresh
 * instance of the exact bug class this whole mechanism exists to fix. A
 * literal list in savestate.c (which is carved out) cannot rot silently,
 * makes the ordering dependency explicit, and is greppable.
 *
 * EVERY ONE OF THESE MUST BE SAFE TO CALL WHEN ITS SUBSYSTEM WAS NEVER
 * INITIALIZED -- the restoring process may not have booted that far. */

/* Drops any DVD file handle the RESTORING process holds: restored game
 * state no longer refers to those files. Safe before the DVD layer has
 * resolved any path. */
void mp6_dvd_savestate_rehydrate(void);

/* Re-points the fake-ARAM base at THIS process's buffer (the .bss restore
 * just installed the capturing process's pointer over it). */
void mp6_aram_savestate_rehydrate(void *liveBuf);

/* C4 (review): the widescreen registries' host-heap "native geometry"
 * snapshots are GAME state that must travel in the file -- the restored
 * arena holds already-EXTRUDED vertices, so a post-restore re-snapshot
 * would record extruded-as-native and double-extrude, while nulling alone
 * left the registries dead until scene re-entry (the registrars' liveness
 * checks all pass on restored values and never re-snapshot). See
 * mp6_widescreen_extrude.c's own comment for the full mechanism.
 * blob_size/blob_write serialize at capture; prerestore frees the LIVE
 * process's snapshots before the image memcpy overwrites the pointers
 * (else every load leaks them); apply_natives rebuilds each entry from the
 * blob afterwards, failing soft per entry (frozen, never corrupt). */
size_t mp6_widescreen_savestate_blob_size(void);
void mp6_widescreen_savestate_blob_write(void *buf);
void mp6_widescreen_savestate_prerestore(void);
void mp6_widescreen_savestate_apply_natives(const void *blob, size_t blobSize);

/* ------------------------------------------------------------------
 * Audio shadow (W3)
 * ------------------------------------------------------------------
 * The mixer's state is carved out (it is owned by the SDL audio callback
 * thread), so a restore leaves it playing whatever the LIVE process was
 * playing while the restored game state believes something else. It cannot
 * be recovered from restored game state either: the playing BGM stream id
 * exists in exactly ONE place in this port, msm_bridge.c's own
 * g_chan[].streamId, and game/audio.c's HuAudSStreamChanPlay passes the id
 * straight through and records only the channel. So it must be captured
 * explicitly -- that is what this shadow is.
 *
 * It is captured from the MIXER's own view rather than from game globals,
 * which is what lets the whole feature avoid patching the decomp: on
 * restore we simply re-establish the mixer state that existed at capture,
 * which by construction agrees with the game state being restored
 * alongside it. (The plan's alternative -- reading game/audio.c's
 * file-static sndGroupBak -- would have needed a new decomp patch AND a
 * way to defeat that function's own equality guard.)
 *
 * Lives in the savestate header, not a region: it is small, fixed-size, and
 * has no address of its own to restore to. */
#define MP6_SS_AUDIO_MAX_CHAN   8
#define MP6_SS_AUDIO_MAX_GROUPS 16

typedef struct {
    int32_t  active;
    int32_t  paused;
    int32_t  streamId;
    uint64_t posFrac;   /* Q16.16 playback position -- restores mid-track, not from 0 */
    int32_t  vol;
    float    fadeMul;
    float    fadeStep;  /* C14 (review): without this, a channel captured mid-fade
                         * restores with fadeAction set but fadeStep 0 -- the fade
                         * never completes, the track plays forever at partial
                         * volume, and status polls waiting on completion stall. */
    int32_t  fadeAction;
} Mp6SsAudioChan;

typedef struct {
    int32_t        chanCount;
    Mp6SsAudioChan chan[MP6_SS_AUDIO_MAX_CHAN];
    int32_t        groupCount;
    int32_t        groupIdx[MP6_SS_AUDIO_MAX_GROUPS]; /* grpInfo indices of the loaded NON-base groups */
    int32_t        seNoCounter;
    int32_t        masterVol;
    int32_t        seMasterVol;
} Mp6SsAudioShadow;

/* Snapshot the mixer into `out` (safe/zeroing if audio never initialized). */
void mp6_msm_savestate_capture(Mp6SsAudioShadow *out);

/* Re-establish the captured mixer state: stop everything, reload the SE
 * groups, replay the streams at their captured positions, and bias the SE
 * handle counter past anything restored game state might still hold. */
void mp6_msm_savestate_apply(const Mp6SsAudioShadow *in);

/* Per-tick hook, called from the frame boundary in BOTH build modes.
 * Handles the interactive hotkeys and the scripted env levers
 * (MP6_SAVESTATE_SAVE_AT_TICK / MP6_SAVESTATE_LOAD_AT_TICK, used by the
 * regression gate). Cheap and silent when the feature is unused. */
void mp6_savestate_tick(void);

/* Requests queued by the windowed hotkey handler, serviced by
 * mp6_savestate_tick() at the safe point. */
void mp6_savestate_request_save(void);
void mp6_savestate_request_load(void);

/* ------------------------------------------------------------------
 * Slot-file UI seam (in-game Save States page, platform/gx/ui/)
 * ------------------------------------------------------------------
 * Same single-pending request queue as above, but the request carries a
 * TARGET PATH (copied into carved-out storage; NULL/empty = the default
 * slot path, i.e. exactly the plain request functions above). The
 * capture/restore core is untouched -- these only parameterize which file
 * the frame-boundary service call passes it. */
void mp6_savestate_request_save_path(const char *path);
void mp6_savestate_request_load_path(const char *path);

/* Numbered slot files, derived from the default slot path (the UI's five
 * slots live NEXT TO it): "mp6_savestate.mp6state" ->
 * "mp6_savestate_slot<N>.mp6state" (the ".mp6state" suffix is re-appended
 * if present, else "_slot<N>" is appended). Honors MP6_SAVESTATE_PATH. */
void mp6_savestate_slot_file(int slot, char *buf, size_t n);

/* Cheap header-only compatibility probe for a slot file (no decompress, no
 * layout walk): MP6_SAVESTATE_OK = readable and captured by THIS build;
 * ERR_IO = no/unreadable file; ERR_FORMAT = not a savestate / other
 * version; ERR_BINARY_MISMATCH = a different build's state (the UI shows
 * "incompatible (other build)" instead of a raw console error). */
int mp6_savestate_probe(const char *path);

/* One-shot pickup of the most recent serviced request's outcome, for UI
 * feedback (toast). Returns 0 if nothing new; else 1 and fills *wasSave
 * and *result (MP6_SAVESTATE_OK or the error). Cleared by the call. */
int mp6_savestate_take_last_result(int *wasSave, int *result);

/* Bumped once per serviced save/load request -- the Save States page polls
 * it to refresh slot timestamps only when something actually happened. */
unsigned int mp6_savestate_ui_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_SAVESTATE_H */
