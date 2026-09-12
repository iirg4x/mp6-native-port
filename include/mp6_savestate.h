/* MP6 native port -- cross-session savestates.
 *
 * PURPOSE. Capture the game's whole mid-session execution state to a file
 * and restore it later, in a DIFFERENT process run, so a bug can be handed
 * over as an exact reproducible state instead of a description plus a
 * multi-minute scripted drive. This is a debugging/QA tool, not a
 * player-facing feature -- and it is a completely different thing from the
 * GCI save system (src/os/save_endian.c), which persists player
 * progress in the on-disc memory-card format. The two share only the
 * English word "save."
 *
 * WHY THIS IS TRACTABLE HERE (and is not, in general, for a native binary).
 * Four properties of this port, each verified rather than assumed:
 *   1. Nearly all mutable game state is in ONE contiguous, fixed-base
 *      reservation -- the 256 MB game arena (src/os/arena.c) -- plus
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
 * is NOT "decomp globals vs port globals": src/os/process_native.c's
 * own scheduler table is port code holding load-bearing game state, and
 * src/host/coro_arena.c's wrapper array is what that table's entries
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
 * silently rot as the code changes. Audio is instead rebuilt from a bounded
 * mixer shadow: callback output is gated silent while streams/groups/SFX are
 * decoded, then every captured slot, handle, position, volume, and fade is
 * published before the callback resumes.
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
 * (see include/mp6_host_section.h, which is the only thing that should
 * ever place variables here). PE section names cap at 8 bytes; both of these
 * exactly fill that. */
#define MP6_HOST_STATE_SECTION_BSS  ".mp6hbss"
#define MP6_HOST_STATE_SECTION_DATA ".mp6hdat"

/* Bump on ANY layout change to the on-disk format below. The loader
 * refuses a mismatch rather than guessing.
 *
 * v7: the audio shadow's SFX voice table went from 16 to 32 fixed entries
 * and gained an explicit capture-time capacity (Mp6SsAudioShadow.voiceCap).
 * Both are header-layout changes, so v6 files are refused as before -- the
 * compatibility this version buys is between two RUNS of a v7 build with
 * DIFFERENT voice-table sizes, not with older files. See Mp6SsAudioShadow. */
#define MP6_SAVESTATE_VERSION 7u

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

/* The emulated RTC is wall-clock-at-process-start plus a host monotonic
 * delta. A captured base cannot be reused after a reboot because the host
 * monotonic epoch resets. Store the logical tick value in the file and
 * rebuild the base against THIS boot's monotonic counter after restore. */
int64_t mp6_os_time_savestate_capture(void);
/* Compute the restoring process's RTC base before the memory commit.  The
 * checked subtraction can reject a corrupt logical tick value without ever
 * invoking signed overflow.  Apply the already-proven base after commit so
 * the live-memory phase has no fallible arithmetic left to perform. */
int mp6_os_time_savestate_rebase(int64_t logicalTicks, int64_t *rtcBaseOut);
void mp6_os_time_savestate_apply_rebase(int64_t rtcBaseTicks);

/* Windowed host-input collectors live in the host-state carve-out, then
 * explicitly discard any deltas/fingers sampled before a restore.  The touch
 * hook exists only in the Android windowed build; savestate.c guards the call
 * with the same platform condition. */
void mp6_freecam_input_savestate_reset(void);
void mp6_touch_pad_savestate_reset(void);
void mp6_aurora_input_reset_transients(void);

/* Current-invocation environment-derived state must survive the image memcpy:
 * loading a capture from a process with different automation/diagnostic env
 * settings must not import that process's parsed caches. */
#define MP6_SS_AUTO_START_MAX 32
typedef struct Mp6SsShimHostConfig {
    int autoStartTicks[MP6_SS_AUTO_START_MAX];
    int autoStartCount;
} Mp6SsShimHostConfig;
typedef struct Mp6SsAllocDiagHostConfig {
    long censusStartTick;
    int censusParsed;
    int censusCallsLogged;
} Mp6SsAllocDiagHostConfig;
void mp6_shims_savestate_capture_host_config(Mp6SsShimHostConfig *out);
void mp6_shims_savestate_apply_host_config(const Mp6SsShimHostConfig *in);
void mp6_malloc_savestate_capture_host_config(Mp6SsAllocDiagHostConfig *out);
void mp6_malloc_savestate_apply_host_config(const Mp6SsAllocDiagHostConfig *in);

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
/* Side-effect-free structural preflight: exact consumption, bounded registry
 * IDs/slots/count, and no duplicate entries. */
int mp6_widescreen_savestate_validate_natives(const void *blob, size_t blobSize);
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
/* The on-disk voice table is ALWAYS the mixer's full capacity, never the
 * run's active count -- one fixed shape, so a file written by a 16-slot run
 * and a file written by a 32-slot run are the same size and the same layout,
 * and neither needs the reader to know the writer's setting before it can
 * parse. Which of those entries were REACHABLE is carried separately, in
 * Mp6SsAudioShadow.voiceCap. Coupled to msm_bridge.c's
 * MP6_MSM_MAX_SFX_VOICES by a #error there. */
#define MP6_SS_AUDIO_MAX_VOICES 32

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
    int32_t  active;
    int32_t  paused;
    int32_t  seId;      /* replay identity in the immutable MSM_SE table */
    int32_t  groupIdx;  /* owning grpInfo index; must still be resident */
    int32_t  seNo;      /* exact game-visible handle */
    uint64_t posFrac;   /* Q16.16 position in the decoded mono sample */
    int32_t  vol;
    int32_t  pan;
    float    fadeMul;
    float    fadeStep;
    int32_t  fadeAction;
} Mp6SsAudioVoice;

typedef struct {
    int32_t        chanCount;
    Mp6SsAudioChan chan[MP6_SS_AUDIO_MAX_CHAN];
    int32_t        groupCount;
    /* Loaded groups beyond the immutable init-time base set. Positive values
     * are ordinary dynamic groups; negative values preserve dynamically-added
     * base groups (msmSysLoadGroupBase) across restore. */
    int32_t        groupIdx[MP6_SS_AUDIO_MAX_GROUPS];
    int32_t        seNoCounter;
    int32_t        masterVol;
    int32_t        seMasterVol;
    /* How many of the voice slots below the CAPTURING run could actually use:
     * exactly MP6_MSM_SFX_VOICES_RETAIL (16) or MP6_MSM_SFX_VOICES_EXTENDED
     * (32), per the "Extended SFX voices" enhancement. Zero only in the
     * canonical all-zero shadow that represents "audio never initialized".
     *
     * WHY IT IS IN THE FILE. The voice-table size is HOST configuration, not
     * game state: msm_bridge.c is in the carve-out, so a restore keeps the
     * RESTORING process's size. Without this field the loader could not tell
     * "slot 20 is empty" from "slot 20 did not exist", which is the whole
     * difference between an exact restore and a silent, unreported loss of a
     * voice. With it, both directions are defined and both LOAD:
     *
     *   captured 16 -> restored under 32: every captured slot restores in
     *       place; slots 16..31 were canonical zeros and stay free.
     *   captured 32 -> restored under 16: slots 0..15 restore in place; a
     *       voice captured in slot 16..31 is DROPPED and named in the log --
     *       never remapped into a lower slot (that would change mixer order
     *       and re-home a game-visible handle) and never a load failure (a
     *       dropped one-shot is indistinguishable from one that just ended,
     *       which msmSeGetStatus already answers MSM_SE_DONE). seNoCounter is
     *       still restored exactly, so a dropped handle is never re-issued.
     *
     * The preflight rejects any other value, and rejects a non-zero slot at
     * or above voiceCap -- a capture cannot claim a voice it could not hold.
     * See src/audio/msm_safe.h's mp6_msm_voice_slot_restorable(). */
    int32_t        voiceCap;
    /* Fixed runtime voice slots, including holes.  Slot identity determines
     * mixer order and future first-free allocation, so compacting this list
     * would not be an exact restore. */
    Mp6SsAudioVoice voice[MP6_SS_AUDIO_MAX_VOICES];
} Mp6SsAudioShadow;

/* Snapshot the mixer into `out` (safe/zeroing if audio never initialized).
 * Returns 0 on success. A negative result means the fixed shadow cannot
 * represent the live mixer exactly, so the outer capture must fail closed
 * instead of publishing a knowingly incomplete state. */
int mp6_msm_savestate_capture(Mp6SsAudioShadow *out);

/* Side-effect-free precommit validation against the live parsed .pdt/.msm
 * directories.  Rejects corrupt IDs, group encodings, handle/counter state,
 * volumes, and non-finite or incoherent fade envelopes before any restored
 * bytes are written to live memory. */
int mp6_msm_savestate_validate(const Mp6SsAudioShadow *in);

/* Re-establish the captured mixer state: stop everything, reload the SE
 * groups, replay streams and SFX at their captured positions/slots/handles,
 * and restore the exact next SE handle counter. */
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
 * Slot-file UI seam (in-game Save States page, src/gx/ui/)
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
/* Returns 0 only when the complete derived path fit; on failure writes an
 * empty string when possible and never returns a truncated path. */
int mp6_savestate_slot_file(int slot, char *buf, size_t n);

/* Cheap compatibility/integrity probe (no decompress, no live-layout walk):
 * reads the header/table and CRC-protected widescreen tail.
 * MP6_SAVESTATE_OK = readable, metadata/tail intact, and captured by THIS build;
 * compressed region-payload integrity is checked only by an actual load.
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
