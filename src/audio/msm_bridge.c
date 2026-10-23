/* MP6 native port -- real streamed-music and sound-effect playback,
 * replacing tools/gen_shims.py's generated logging no-op for the msm
 * STREAM + SE (sound effect) families + msmSysInit/msmSysRegularProc +
 * the AI family. This header comment covers the scope/design decisions
 * actually load-bearing for the code below.
 *
 * SCOPE -- what this file takes over (see tools/gen_shims.py's
 * MANUAL_SYMBOLS, which excludes exactly these from generation):
 *   msmSysInit, msmSysCheckInit, msmSysRegularProc, msmStreamPlay, msmStreamStop,
 *   msmStreamPauseAll, msmStreamPause, msmStreamSetParam,
 *   msmStreamGetStatus, msmStreamStopAll, msmStreamSetMasterVolume,
 *   msmSePlay, msmSeStop, msmSeStopAll, msmSeGetStatus, msmSePauseAll,
 *   msmSeSetParam, msmSeSetMasterVolume, msmSysLoadGroup{,Base},
 *   msmSysDelGroup{All,Base}, msmSysGetSampSize, msmSysSetGroupLoadMode,
 *   AIGetDMAStartAddr, AIInitDMA, AIRegisterDMACallback,
 *   AISetStreamPlayState, AISetStreamVolLeft, AISetStreamVolRight, AIStartDMA.
 *
 * What this file deliberately does NOT take over (left exactly as the
 * auto-generated logging no-op in src/null/shims_{generated,
 * generated_aurora}.c): msmSysSetOutputMode/msmSysSetAux
 * -- aux effect buses and output-mode are out of scope; dry stereo only.
 *
 * FORMAT -- how sound/MP6_Str.pdt is laid out (ground-truth verified
 * against the real disc file):
 *
 *   offset 0x00: a 0x20-byte header -- s16 version; s16 streamMax;
 *     s32 chanMax; s32 sampleFrq; s32 maxBufs; u32 streamPackListOfs;
 *     u32 adpcmParamOfs; u32 streamPackOfs; u32 sampleOfs (all big-endian,
 *     matching include/game/msm_data.h's MSM_STREAM_HEADER). Real file:
 *     version=1 streamMax=110 chanMax=4 sampleFrq=32000 maxBufs=2.
 *   offset streamPackListOfs: streamMax * u32 ABSOLUTE file offsets (0 =
 *     "no pack at this stream id") into the streamPack region below.
 *   offset adpcmParamOfs: back-to-back 32-byte SND_ADPCMSTREAM_INFO
 *     tables (8 (coef1,coef2) s16 pairs each) -- musyx/stream.h's real
 *     layout, standard GC DSP-ADPCM.
 *   offset streamPackOfs: back-to-back 32-byte MSM_STREAM_PACK entries
 *     (s8 flag,vol,pan,span,auxA,auxB; u16 frq; u32 loopOfsEnd;
 *     u32 loopOfsStart; then 2x MSM_STREAM {s32 sampleOfs; s16
 *     adpcmParamIdx; u16 pad} for the L/R sub-streams) -- addressed via
 *     the ABSOLUTE offset from streamPackList[id], not a 0-based index.
 *   offset sampleOfs onward: raw ADPCM sample bytes for every stream's
 *     every sub-stream, each sub-stream's own `sampleOfs` field being
 *     itself an absolute file byte offset (msmStreamSlotInit passes it
 *     straight through to DVDReadAsyncPrio's own offset param with
 *     nothing else added).
 *   loopOfsEnd/loopOfsStart are GC "nibble addresses" (the standard
 *     scheme where a whole 8-byte/16-nibble frame -- including its own
 *     non-sample header nibble -- counts toward the address, which is why
 *     a plain `>>1` converts one straight to a byte offset with no
 *     frame-aware math needed); `& ~0x1F`/`& ~7` round down to a whole
 *     ADPCM frame, matching src/msm/msmstream.c's own masking exactly.
 *
 * FLAG BIT GROUND TRUTH -- include/game/msm_data.h names bit0
 * `MSM_STREAM_FLAG_STEREO` and bit1 `MSM_STREAM_FLAG_LOOP`, but
 * src/msm/msmstream.c's own `slot->stereoF = (pack->flag >> 1) & 1` reads
 * bit1 for something it calls "stereo" too -- genuinely ambiguous from
 * the recovered C alone. Every one of the real file's 110 defined packs
 * has bit0 SET (13 with flag==0x01, 97 with flag==0x03 -- bit1 is the
 * only one that ever varies), and the real disassembly settles which bit
 * actually drives stereo dispatch (build/GP6E01/asm/msm/msmstream.s,
 * msmStreamPlay @ 0x80144260):
 *
 *     lbz r0, 0x0(r31)      ; r0 = pack->flag
 *     clrlwi. r0, r0, 31    ; r0 &= 0x1 (bit0 ONLY), sets cr0
 *     beq  .L_801442F4      ; bit0 CLEAR -> msmStreamPackStartMono
 *     ...                   ; bit0 SET   -> msmStreamPackStartStereo
 *
 * -- bit0 (0x1) is the live STEREO-dispatch test at the exact call site
 * that matters, exactly as include/game/msm_data.h names it, and (with
 * real data always setting it) every one of this game's 110 streams
 * genuinely plays as a true, always-2-channel stereo pair: sub-stream 0
 * hard-panned full left, sub-stream 1 hard-panned full right
 * (msmStreamPackStartStereo's own `streamParam.pan = 0` / `= 127`,
 * unconditional -- not gated on any caller-supplied pan at all). Bit1 is
 * the independently authored LOOP flag named by msm_data.h: the 97 packs
 * carrying it wrap to loopOfsStart, while the 13 clear-bit cinematic/
 * jingle packs deactivate at loopOfsEnd so msmStreamGetStatus reports
 * DONE. The decompiled `stereoF` name is misleading bookkeeping for the
 * real double-buffered refill path; it does not change bit0's proven
 * stereo dispatch or bit1's authored lifetime semantic in this full-decode
 * mixer. MP6_PACK_FLAG_STEREO is therefore bit0 and MP6_PACK_FLAG_LOOP is
 * bit1, matching the format header and the on-disc flag census.
 *
 * DESIGN SIMPLIFICATIONS (deliberate):
 *   - Real hardware streams incrementally via double-buffered ASYNC DVD
 *     reads (msmStreamData/msmStreamDvdCallback's whole ping-pong dance)
 *     because a real DVD read takes real milliseconds and audio can't
 *     wait on it. This port's own DVDReadPrio (src/dvd/dvd_files.c)
 *     already completes synchronously against a local, individually-
 *     extracted file -- so msmStreamPlay here just decodes a whole
 *     stream's ADPCM to PCM up front, once, into a host-malloc'd buffer
 *     (a few MB to a few tens of MB per track -- trivial for a native
 *     PC's RAM budget, unlike the original 24MB-GameCube-constrained
 *     design this format was built for). This trades a one-time decode
 *     hitch at Play time (microseconds to low milliseconds per track) for
 *     a MUCH simpler, race-free playback path with zero streaming-buffer-
 *     underrun surface at all.
 *   - Looping is a hard jump back to loopStartFrame with fresh (zeroed)
 *     ADPCM decode history rather than the original's carried-forward
 *     history across the seam -- a real, if minor, discontinuity click is
 *     possible exactly at the loop point.
 *   - Fade envelopes: `msmStream{Stop,Pause,PauseAll}`'s own `speed`
 *     parameter (and the SFX equivalents, `msmSeStop`/`msmSePauseAll`)
 *     drive a real linear gain ramp in the shared mixer (`mp6_msm_render`'s
 *     own per-frame loop, section below) -- `speed<=0` applies the change
 *     immediately, `speed>0` fades smoothly over `speed` milliseconds
 *     before the channel actually stops/settles into pause, or fades back
 *     in over `speed` ms when un-pausing. Unit choice (`speed` ==
 *     milliseconds) is a DOCUMENTED ASSUMPTION, not derived from
 *     decompiled source (MuSyX's own real fade-envelope implementation is
 *     closed-source middleware, never decompiled in this tree) -- chosen
 *     because the two real call-site constants observed
 *     (`HuAudSStreamAllFadeOut`'s `1000`, `HuAudSStreamPause`'s hardcoded
 *     `5`) read naturally as "1 full second" and "near-instant, click-
 *     avoiding" respectively under that reading. `include/game/msm.h`'s own
 *     `MSM_STREAM_PAUSEIN`(3)/`PAUSEOUT`(4) constants (distinct from the
 *     settled `STOP`(1)/`PLAY`(2)) independently confirm the real engine
 *     models pause as a genuine fade TRANSITION, not an instant toggle --
 *     `msmStreamGetStatus` reports these while a fade is actually in
 *     flight, matching that real distinction, and falls back to the exact
 *     done/paused/play logic once any fade completes or when none is in
 *     progress. `msmStreamPlay`'s own `fadeSpeed` field (fade-in ON PLAY, a
 *     different feature -- crossfading into a freshly started track) is
 *     unchanged, still logged-only -- see this function's own call site.
 *     This file's own volume model (baseVol from the pack * per-play vol *
 *     master vol, all /127) is unchanged; the fade multiplier is an
 *     ADDITIONAL, independent factor.
 *   - span/auxA/auxB (surround width + MuSyX aux-bus effect sends, e.g.
 *     reverb) are read from the pack but never applied -- dry stereo
 *     output only.
 *   - No thread-level lock-free design: a single mutex (g_mixerLock, an
 *     Mp6Mutex via the host seam) guards all channel-state reads/writes
 *     since mp6_msm_render() runs on SDL's own audio callback thread in
 *     the Aurora build while msmStreamPlay/Stop/... run on the game's own
 *     thread. Decode (the slow part) always happens OUTSIDE the lock;
 *     only the final "publish into g_chan[chan]" step holds it, so lock
 *     contention/hold time is minimal.
 */

#include "msm_internal.h"
#include "mp6_dvd_files.h" /* mp6_dvd_prefetch_range: a track start reads both channels */

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own includes is
 * load-bearing, not stylistic: it is a #pragma clang section that redirects
 * every file-scope definition FOLLOWING it. As a -include (before all headers)
 * it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES -- all four msm_*.c files are. */
#include "mp6_host_section.h"


/* ---- SFX EVENT TIMELINE (MP6_AUDIO_TIMELINE=1) -------------------------
 * Opt-in, diagnostic-only. Every line is tagged with the GAME tick, so a
 * single run answers "did the game retrigger this seId, or did ONE voice
 * keep looping in the mixer?" without any guesswork:
 *   [SETL] play  -- one per accepted msmSePlay, carries the returned seNo
 *   [SETL] stop  -- one per msmSeStop, says whether the handle still existed
 *   [SETL] wrap  -- first mixer loop-wrap of a voice, then periodic census
 *   [SETL] end   -- voice left the active set (natural one-shot end or stop)
 * It is env-gated because the wrap/census counts depend on audio-callback
 * timing, which the savestate replay oracle's byte-exact stdout compare
 * must not see. */
static int mp6_se_timeline_on(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("MP6_AUDIO_TIMELINE");
        s_on = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    /* env latches the initial value; the dev console may override it live
     * (`set audiotimeline 1`), which matters here more than for most levers --
     * a voice that keeps looping is exactly the kind of thing you only think
     * to instrument once you can already hear it. */
    return mp6_console_cvar_get(MP6_CVAR_AUDIO_TIMELINE, s_on);
}

static char g_pdtPath[512];
int g_pdtReady;
int g_streamMax;
int g_chanMax = 4;

static uint32_t *g_packListOfs;   /* [g_streamMax], absolute file offsets (0 = none) */

static uint8_t *g_packBlob;       /* raw bytes [streamPackOfs, sampleOfs) */
static uint32_t g_packBlobBase;   /* == header.streamPackOfs */
static uint32_t g_packBlobSize;
static uint32_t g_pdtFileSize;
static MP6AdpcmCoefTable *g_coef; /* [g_numCoef] */
static int g_numCoef;
int g_masterVol = 127;
MsmChan g_chan[MP6_MSM_MAX_CHAN];
int g_seMasterVol = 127;
MsmSeVoice g_sfxVoice[MP6_MSM_MAX_SFX_VOICES];
int g_seNoCounter = 1;

/* Host configuration, excluded from game-state snapshots. Changes retire
 * removed slots under g_mixerLock without renumbering surviving handles.
 * Atomic because a few range checks read the limit outside the mixer lock;
 * table access still uses the existing mixer lock. */
static _Atomic int g_sfxVoiceCap;

int mp6_sfx_voice_cap(void)
{
    int cap = g_sfxVoiceCap;
    if (cap == 0) {
        cap = mp6_msm_voice_cap_clamp(mp6_enh_sfx_voices());
        g_sfxVoiceCap = cap;
    }
    return cap;
}

/* Running key-group totals for the pull-side audio snapshot
 * (include/mp6_diag_probe.h). Key-group releases are the one SFX event
 * with no standing surface at all: they appear only as individual [SETL] kgrel
 * lines under MP6_AUDIO_TIMELINE, so "is something silently killing this
 * group's voices" cannot be answered without turning that whole stream on.
 * Bumped by mp6_se_keygroup_release() under g_mixerLock, like every other
 * field it touches, and read back under the same lock. */
static unsigned long g_kgReleaseEvents;

static unsigned long g_kgVoicesReleased;
Mp6Mutex g_mixerLock;
int g_mixerLockInit;
int g_savestateMixerMuted;

void mp6_lock(void) { mp6_host_mutex_lock(&g_mixerLock); }

void mp6_unlock(void) { mp6_host_mutex_unlock(&g_mixerLock); }

void mp6_pcm_detach(MsmPcmRetirement *retired, int16_t **pcm)
{
    if (*pcm) {
        if (retired->count >= sizeof(retired->pcm) / sizeof(retired->pcm[0])) abort();
        retired->pcm[retired->count++] = *pcm;
        *pcm = NULL;
    }
}

void mp6_pcm_release(MsmPcmRetirement *retired)
{
    for (size_t i = 0; i < retired->count; ++i) free(retired->pcm[i]);
    retired->count = 0;
}

/* Game-thread setting change. Slot identities below the new limit do not
 * move; retire upper slots under the same lock as mixing and allocation. */
void mp6_msm_apply_voice_limit(void)
{
    MsmPcmRetirement pcmRetired = {0};
    const int cap = mp6_msm_voice_cap_clamp(mp6_enh_sfx_voices());
    int i, oldCap, retired = 0;
    if (!g_mixerLockInit) { g_sfxVoiceCap = cap; return; }
    mp6_lock();
    oldCap = g_sfxVoiceCap;
    for (i = cap; i < MP6_MSM_MAX_SFX_VOICES; ++i) {
        retired += g_sfxVoice[i].active != 0;
        mp6_pcm_detach(&pcmRetired, &g_sfxVoice[i].pcm);
        memset(&g_sfxVoice[i], 0, sizeof(g_sfxVoice[i]));
    }
    g_sfxVoiceCap = cap;
    mp6_unlock();
    mp6_pcm_release(&pcmRetired);
    if (oldCap != cap) printf("[AUDIO] live voice limit: %d -> %d (%d extra voices retired)\n",
                              oldCap, cap, retired);
    fflush(stdout);
}

/* =======================================================================
 * WAV-dump verification path (MP6_AUDIO_WAV_DUMP=<path>) -- see this
 * file's own header comment. Tapped from inside mp6_msm_render() itself
 * so it passively captures whichever backend is actually driving playback
 * (the --headless tick pump below, or Aurora's SDL callback in
 * audio_out_sdl.c) with no risk of double-rendering.
 *
 * The 30-second CAPTURE window only starts counting once g_wavArmed goes
 * true (msmStreamPlay sets it on the very first successful Play) rather
 * than from process start: the real boot flow spends ~3640 ticks (~60
 * real seconds' worth of simulated audio time) sitting at the warning
 * screen's own bounded input-wait loop before anything ever calls
 * msmStreamPlay at all (src/REL/bootDll/boot.c's MAX_INPUT_WAIT_FRAMES =
 * 0xDB6) -- capturing from tick 0 unconditionally would spend the entire
 * 30-second cap on pre-boot silence and never reach a single note of
 * actual music. */
static int16_t *g_wavAccum;
static uint32_t g_wavAccumFrames;
static int g_wavDone;
static int g_wavArmed;
static int g_wavEnvChecked;
static char g_wavPath[512];

static int16_t mp6_clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void mp6_wav_capture(const int16_t *out, uint32_t frames, int armed)
{
    uint32_t room, take;

    if (!g_wavEnvChecked) {
        const char *e;
        g_wavEnvChecked = 1;
        e = getenv("MP6_AUDIO_WAV_DUMP");
        if (e && e[0]) {
            size_t n = strlen(e);
            if (n < sizeof(g_wavPath)) {
                memcpy(g_wavPath, e, n + 1);
            } else {
                fprintf(stderr, "[AUDIO] MP6_AUDIO_WAV_DUMP path too long (%zu bytes), ignoring\n", n);
            }
        }
        if (g_wavPath[0]) {
            g_wavAccum = (int16_t *)malloc((size_t)MP6_WAV_CAP_FRAMES * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));
            if (!g_wavAccum) {
                fprintf(stderr, "[AUDIO] MP6_AUDIO_WAV_DUMP: out of memory for the %d-second capture "
                        "buffer -- disabling\n", MP6_WAV_CAP_SECONDS);
                g_wavPath[0] = '\0';
            } else {
                printf("[AUDIO] MP6_AUDIO_WAV_DUMP active -- capturing the first %d seconds of rendered "
                       "audio to \"%s\"\n", MP6_WAV_CAP_SECONDS, g_wavPath);
            }
        }
    }

    if (!g_wavPath[0] || g_wavDone || !g_wavAccum || !armed) return;

    room = MP6_WAV_CAP_FRAMES - g_wavAccumFrames;
    take = frames < room ? frames : room;
    if (take > 0) {
        memcpy(g_wavAccum + (size_t)g_wavAccumFrames * MP6_MSM_OUT_CHANNELS, out,
               (size_t)take * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));
        g_wavAccumFrames += take;
    }

    if (g_wavAccumFrames >= MP6_WAV_CAP_FRAMES) {
        uint32_t i;
        uint32_t n = MP6_WAV_CAP_FRAMES * MP6_MSM_OUT_CHANNELS;
        int64_t sumSq = 0;
        int32_t peak = 0;
        double rms;

        for (i = 0; i < n; i++) {
            int32_t s = g_wavAccum[i];
            sumSq += (int64_t)s * (int64_t)s;
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        rms = sqrt((double)sumSq / (double)n);

        if (mp6_wav_write(g_wavPath, g_wavAccum, MP6_WAV_CAP_FRAMES, MP6_MSM_OUT_RATE, MP6_MSM_OUT_CHANNELS) == 0) {
            printf("[AUDIO] MP6_AUDIO_WAV_DUMP: wrote %d seconds (%u frames @ %uHz) to \"%s\" -- "
                   "peak=%d (%.1f%% FS) rms=%.1f (%.1f%% FS)\n",
                   MP6_WAV_CAP_SECONDS, (unsigned)MP6_WAV_CAP_FRAMES, (unsigned)MP6_MSM_OUT_RATE, g_wavPath,
                   (int)peak, 100.0 * peak / 32768.0, rms, 100.0 * rms / 32768.0);
        } else {
            fprintf(stderr, "[AUDIO] MP6_AUDIO_WAV_DUMP: failed to write \"%s\"\n", g_wavPath);
        }
        g_wavDone = 1;
        free(g_wavAccum);
        g_wavAccum = NULL;
    }
}

/* =======================================================================
 * --headless build ONLY: real, non-weak definitions of the two hooks
 * src/audio/audio_out_sdl.c provides for the default (aurora) build
 * instead -- see mp6_audio_out.h's own header comment for the split.
 *
 * mp6_headless_drain_wav_dump exists because msmSysRegularProc() further
 * below (the REAL per-frame pump src/game/pad.c's PadReadVSync calls) is
 * UNREACHABLE dead code in this build mode: PadReadVSync only ever runs
 * via the VI post-retrace callback game/pad.c's HuPadInit registers
 * through VISetPostRetraceCallback -- and this build mode's own VI shims
 * (src/null/shims_generated.c's AUTO-GENERATED VISetPostRetraceCallback
 * -- a bare `MP6_LOG_ONCE(...); return 0;`, never storing the callback at
 * all -- and src/null/shims_manual.c's hand-written VIWaitForRetrace,
 * which never invokes one) never actually fire it. The REGISTRATION does
 * happen (the boot log's own "[SDK] VI.VISetPostRetraceCallback(...)"
 * line proves that much) -- it's purely the invocation side that's
 * missing. This is a real, pre-existing, cross-cutting src/null
 * VI-callback gap outside src/audio/'s own scope -- reported, not
 * patched here.
 *
 * Rather than reach into src/null/*.c to fix that at the root, this
 * file drives its OWN mixer pump instead, entirely within src/audio/:
 * a bounded, synchronous drain, run once here. mp6_audio_out_init() is
 * called from msmSysInit AFTER its own MP6_AUDIO_SELFTEST_STREAMS block
 * (above) has already run every requested msmStreamPlay, so by the time
 * this runs, whatever that hook started is already active and ready to
 * render. This renders straight up to the WAV-dump capture's own frame
 * cap via mp6_msm_render() -- the exact same mixer entry point
 * audio_out_sdl.c's real SDL callback and msmSysRegularProc's own
 * per-tick pump below both already call -- with NO real-time pacing at
 * all: headless has no live device to keep fed in real time, so there is
 * no reason to spread this over 30 wall-clock seconds; a bounded, fast,
 * one-shot drain is simpler and just as correct for a verification-only
 * path. No-op (returns immediately) unless MP6_AUDIO_WAV_DUMP is actually
 * set -- with it unset there's nothing downstream of mp6_msm_render() to
 * drive at all in this build mode.
 *
 * KNOWN LIMITATION: only captures whatever is ALREADY playing by the time
 * msmSysInit returns (i.e. exactly the MP6_AUDIO_SELFTEST_STREAMS
 * scenario). A msmStreamPlay() call arriving on some LATER real game tick
 * -- unreachable today, but exactly what would start happening the moment
 * anyone fixes the VI-callback gap above -- would arrive after this drain
 * has already run to completion (g_wavDone already 1) and so would NOT be
 * captured; that same future fix should also revisit whether this
 * one-shot drain is still needed at all.
 * ======================================================================= */
#ifdef MP6_HEADLESS_BUILD
static void mp6_headless_drain_wav_dump(void)
{
    const char *e = getenv("MP6_AUDIO_WAV_DUMP");
    uint32_t framesLeft;

    if (!e || !e[0]) return; /* not requested -- nothing downstream of mp6_msm_render()
                                 to drive at all in this build mode */

    printf("[AUDIO] --headless build: draining the WAV-dump capture window synchronously now "
           "(msmSysRegularProc's own per-tick pump is unreachable here -- see this file's own "
           "comment just above)\n");

    /* +MP6_RENDER_SCRATCH_FRAMES is just cheap insurance against an off-by-
     * one in this loop's own accounting -- mp6_wav_capture's internal
     * room/take clamp is exact regardless of how this loop chunks frames,
     * so overshooting the cap here is always harmless (the `!g_wavDone`
     * check simply stops the very next iteration once the real cap is
     * reached inside mp6_wav_capture itself). */
    framesLeft = MP6_WAV_CAP_FRAMES + MP6_RENDER_SCRATCH_FRAMES;
    while (!g_wavDone && framesLeft > 0) {
        int16_t scratch[MP6_RENDER_SCRATCH_FRAMES * MP6_MSM_OUT_CHANNELS];
        uint32_t chunk = framesLeft > MP6_RENDER_SCRATCH_FRAMES ? MP6_RENDER_SCRATCH_FRAMES : framesLeft;
        mp6_msm_render(scratch, chunk);
        framesLeft -= chunk;
    }
}

void mp6_audio_out_init(void)
{
    printf("[AUDIO] --headless build: no live audio device -- MP6_AUDIO_WAV_DUMP is this mode's own "
           "verification path instead (see docs/DEBUGGING.md)\n");
    mp6_headless_drain_wav_dump();
}
void mp6_audio_out_shutdown(void)
{
}
#endif

/* =======================================================================
 * .pdt file access -- plain synchronous reads via the SDK's own DVD API
 * (src/os/dll_bridge.c + src/dvd/dvd_files.c already back these
 * for real, over the extracted disc tree -- reused as-is rather than
 * reaching around them, exactly matching how the real src/msm/msmfio.c
 * itself only ever calls DVDOpen/DVDReadPrio/DVDClose). Every read here
 * completes fully before returning (this port's own DVDReadPrio always
 * does), so no callback/async plumbing is needed at all.
 * ======================================================================= */
BOOL msm_audio_file_size(const char *path, uint32_t *sizeOut)
{
    DVDFileInfo fi;
    if (path == NULL || sizeOut == NULL || !DVDOpen((char *)path, &fi)) {
        return FALSE;
    }
    *sizeOut = fi.length;
    DVDClose(&fi);
    return TRUE;
}

static BOOL pdt_read_range(uint32_t offset, uint32_t length, void *dst)
{
    DVDFileInfo fi;
    BOOL ok;

    if (length == 0) return TRUE;
    if (!DVDOpen(g_pdtPath, &fi)) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDOpen(\"%s\") failed\n", g_pdtPath);
        return FALSE;
    }
    if (dst == NULL || length > (uint32_t)INT_MAX ||
        !mp6_msm_span_valid_u32(fi.length, offset, length)) {
        fprintf(stderr, "[AUDIO] msm_bridge: invalid .pdt read span "
                "(offset=%u length=%u file=%u)\n",
                (unsigned)offset, (unsigned)length, (unsigned)fi.length);
        DVDClose(&fi);
        return FALSE;
    }
    ok = DVDReadPrio(&fi, dst, (s32)length, (s32)offset, 2);
    DVDClose(&fi);
    if (!ok) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDReadPrio failed (offset=%u length=%u)\n",
                (unsigned)offset, (unsigned)length);
    }
    return ok;
}

BOOL msm_pdt_get_pack(int streamId, PdtPack *out)
{
    uint32_t absOfs, rel;
    const uint8_t *p;

    if (!g_pdtReady) return FALSE;
    if (streamId < 0 || streamId >= g_streamMax) return FALSE;
    absOfs = g_packListOfs[streamId];
    if (absOfs == 0) return FALSE; /* MSM_ERR_REMOVEDID -- no pack defined for this id */
    if (absOfs < g_packBlobBase) return FALSE;
    rel = absOfs - g_packBlobBase;
    if ((uint64_t)rel + 32 > g_packBlobSize) return FALSE; /* defensive: corrupt/out-of-range offset */
    p = g_packBlob + rel;

    out->flag = p[0];
    out->vol  = (int8_t)p[1];
    out->pan  = (int8_t)p[2];
    out->span = (int8_t)p[3];
    out->auxA = (int8_t)p[4];
    out->auxB = (int8_t)p[5];
    out->frq  = (uint16_t)be16(p + 6);
    {
        uint32_t loopOfsEnd = be32(p + 8);
        uint32_t loopOfsStart = be32(p + 12);
        out->loopEndByte = (loopOfsEnd >> 1) & ~0x1Fu;
        out->loopStartByte = (loopOfsStart >> 1) & ~0x7u;
    }
    out->subSampleOfs[0] = be32(p + 16);
    out->subCoefIdx[0]   = (int16_t)be16(p + 20);
    out->subSampleOfs[1] = be32(p + 24);
    out->subCoefIdx[1]   = (int16_t)be16(p + 28);
    return TRUE;
}

/* NOTE the two distinct units in play here: an 8-byte ADPCM "frame"
 * decodes to 14 PCM samples, so "how many ADPCM frames" and "how many PCM
 * samples" differ by a factor of 14. msm_pdt_pack_frame_count() returns the
 * FORMER (what decode_substream's
 * own `numFrames` parameter -- and mp6_dspadpcm_decode's, matching it --
 * both mean); callers that need a PCM sample count (buffer sizing,
 * totalFrames/loopStartFrame bookkeeping) must multiply by 14 themselves,
 * explicitly, at the call site -- never pass a sample count in here. */
uint32_t msm_pdt_pack_frame_count(const PdtPack *pack)
{
    return pack->loopEndByte / 8;
}

/* =======================================================================
 * Streamed music decode.
 *
 * msmStreamPlay used to decode a whole track (a few MB of PCM) on the game
 * thread before returning: 11-17 ms per track start on PC (MP6_LOAD_TRACE),
 * several times that on phones. It now decodes the first
 * MP6_STREAM_HEAD_SECONDS up front and the rest in MP6_STREAM_STEP_SECONDS
 * slices from msmSysRegularProc, one slice per game tick, so decoding runs
 * about 60x ahead of playback. The PCM is bit-identical: the same ADPCM
 * bytes go through the same decoder, with the decode history carried from
 * one slice to the next. The mixer only reads frames below the channel's
 * decodedFrames, which is published under g_mixerLock; a read past it (only
 * possible if the game thread stalls for longer than the lead) plays
 * silence and is counted and reported. Headless builds, savestate replays
 * and MP6_AUDIO_SYNC_DECODE=1 still decode the whole track up front.
 * Every field below belongs to the game thread.
 * ======================================================================= */
#define MP6_STREAM_HEAD_SECONDS 2u
#define MP6_STREAM_STEP_SECONDS 1u

typedef struct {
    int16_t *pcm;               /* the channel buffer being filled; NULL = idle */
    uint32_t nextFrame;         /* next ADPCM frame to decode (14 PCM frames each) */
    uint32_t prefetchedFrame;   /* ADPCM frames asked of the disc layer ahead (msm_stream_job_read_ahead) */
    uint32_t adpcmFrames;
    uint32_t framesPerSecond;   /* ADPCM frames per second of audio */
    uint32_t sampleOfs[2];
    int16_t coefIdx[2];
    int stereo;
    MP6AdpcmState state[2];
} MsmStreamJob;

static MsmStreamJob g_streamJobs[MP6_MSM_MAX_CHAN];
static unsigned long g_streamUnderruns;         /* mixer-written, under g_mixerLock */
static unsigned long g_streamUnderrunsReported; /* game thread */

static int mp6_stream_sync_decode(void)
{
#ifdef MP6_HEADLESS_BUILD
    return 1; /* the headless WAV drain renders faster than real time */
#else
    static int s_sync = -1;
    if (s_sync < 0) {
        const char *e = getenv("MP6_AUDIO_SYNC_DECODE");
        s_sync = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_sync;
#endif
}

/* Validates one sub-stream's whole ADPCM span before any of it is decoded. */
static s32 msm_substream_check(uint32_t sampleOfs, int16_t coefIdx, uint32_t numFrames)
{
    if (numFrames == 0 ||
        numFrames > UINT32_MAX / 8 ||
        !mp6_msm_decode_budget_valid((uint64_t)numFrames * 14u, 4u) ||
        !mp6_msm_span_valid_u32(g_pdtFileSize, sampleOfs, numFrames * 8)) {
        fprintf(stderr, "[AUDIO] msm_bridge: invalid ADPCM sample span "
                "(offset=%u frames=%u file=%u)\n",
                (unsigned)sampleOfs, (unsigned)numFrames, (unsigned)g_pdtFileSize);
        return MSM_ERR_INVALIDFILE;
    }
    if (coefIdx < 0 || coefIdx >= g_numCoef) {
        fprintf(stderr, "[AUDIO] msm_bridge: adpcmParamIdx %d out of range "
                "(have %d coefficient tables)\n", (int)coefIdx, g_numCoef);
        return MSM_ERR_INVALIDFILE;
    }
    return 0;
}

/* Decodes up to `count` more ADPCM frames of `job` into its interleaved
 * stereo buffer `pcm` (a mono stream fills both sides). */
static s32 msm_stream_job_decode(MsmStreamJob *job, int16_t *pcm, uint32_t count)
{
    uint32_t n = job->adpcmFrames - job->nextFrame;
    uint8_t *raw;
    int16_t *mono[2] = {NULL, NULL};
    int16_t *dst;
    uint32_t f;
    int s;
    s32 result = 0;

    if (count < n) n = count;
    if (n == 0) return 0;
    raw = (uint8_t *)malloc((size_t)n * 8);
    mono[0] = (int16_t *)malloc((size_t)n * 14 * sizeof(int16_t));
    if (job->stereo) mono[1] = (int16_t *)malloc((size_t)n * 14 * sizeof(int16_t));
    if (!raw || !mono[0] || (job->stereo && !mono[1])) {
        fprintf(stderr, "[AUDIO] msm_bridge: out of memory decoding %u ADPCM frames\n", (unsigned)n);
        result = MSM_ERR_OUTOFMEM;
        goto done;
    }
    for (s = 0; s <= job->stereo; s++) {
        if (!pdt_read_range(job->sampleOfs[s] + job->nextFrame * 8, n * 8, raw)) {
            result = MSM_ERR_READFAIL;
            goto done;
        }
        mp6_dspadpcm_decode(raw, n, &g_coef[job->coefIdx[s]], &job->state[s], mono[s]);
    }
    dst = pcm + (size_t)job->nextFrame * 14 * MP6_MSM_OUT_CHANNELS;
    for (f = 0; f < n * 14; f++) {
        dst[f * 2 + 0] = mono[0][f];
        dst[f * 2 + 1] = job->stereo ? mono[1][f] : mono[0][f];
    }
    job->nextFrame += n;
done:
    free(raw);
    free(mono[0]);
    free(mono[1]);
    return result;
}

/* MP6_AUDIO_STREAM_VERIFY=1: once a track finishes streaming, decode it again
 * in one piece and require the two buffers to match exactly. */
static void mp6_stream_verify(const MsmStreamJob *done, int ch)
{
    static int s_verify = -1;
    MsmStreamJob fresh;
    int16_t *whole;
    size_t bytes;
    if (s_verify < 0) {
        const char *e = getenv("MP6_AUDIO_STREAM_VERIFY");
        s_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!s_verify) return;
    fresh = *done;
    fresh.nextFrame = 0;
    memset(fresh.state, 0, sizeof(fresh.state));
    bytes = (size_t)done->adpcmFrames * 14 * MP6_MSM_OUT_CHANNELS * sizeof(int16_t);
    whole = (int16_t *)malloc(bytes);
    if (!whole) return;
    if (msm_stream_job_decode(&fresh, whole, fresh.adpcmFrames) == 0) {
        int same = memcmp(whole, done->pcm, bytes) == 0;
        printf("[AUDIO] stream verify chan=%d frames=%u: %s\n", ch,
               (unsigned)(done->adpcmFrames * 14), same ? "identical" : "MISMATCH");
        fflush(stdout);
    }
    free(whole);
}

/* A track's slices read new parts of MP6_Str.pdt every second, a fresh
 * disc read each time the track has not played lately (4-6 ms on a PC's
 * disk). Keep the next MP6_STREAM_READ_AHEAD_SECONDS of both channels
 * fetched ahead of the decoder. */
#define MP6_STREAM_READ_AHEAD_SECONDS 4u

static void msm_stream_job_read_ahead(MsmStreamJob *job)
{
    uint32_t want = job->nextFrame + job->framesPerSecond * MP6_STREAM_READ_AHEAD_SECONDS;
    uint32_t from = job->prefetchedFrame > job->nextFrame ? job->prefetchedFrame : job->nextFrame;
    int s;
    if (want > job->adpcmFrames) want = job->adpcmFrames;
    if (from >= want || want - from < job->framesPerSecond) return; /* a second at a time */
    for (s = 0; s <= job->stereo; s++) {
        mp6_dvd_prefetch_range(g_pdtPath, job->sampleOfs[s] + from * 8, (want - from) * 8);
    }
    job->prefetchedFrame = want;
}

/* Game thread, once per tick: decode the next slice of every playing track. */
void mp6_msm_stream_decode_ahead(void)
{
    int ch;
    unsigned long underruns;
    for (ch = 0; ch < g_chanMax && ch < MP6_MSM_MAX_CHAN; ch++) {
        MsmStreamJob *job = &g_streamJobs[ch];
        s32 result;
        uint64_t traceStart;
        if (!job->pcm) continue;
        if (g_chan[ch].pcm != job->pcm) { /* stopped or replaced: the buffer is not ours */
            job->pcm = NULL;
            continue;
        }
        traceStart = mp6_load_trace_begin();
        result = msm_stream_job_decode(job, job->pcm, job->framesPerSecond * MP6_STREAM_STEP_SECONDS);
        mp6_load_trace_end(traceStart, MP6_LOAD_STREAM, 0);
        if (result == 0) msm_stream_job_read_ahead(job);
        if (result != 0) {
            fprintf(stderr, "[AUDIO] msm_bridge: chan %d stream decode stopped at ADPCM frame %u "
                    "(result %d); the rest of the track stays silent\n",
                    ch, (unsigned)job->nextFrame, (int)result);
        }
        mp6_lock();
        g_chan[ch].decodedFrames = job->nextFrame * 14;
        mp6_unlock();
        if (result == 0 && job->nextFrame >= job->adpcmFrames) mp6_stream_verify(job, ch);
        if (result != 0 || job->nextFrame >= job->adpcmFrames) job->pcm = NULL;
    }
    mp6_lock();
    underruns = g_streamUnderruns;
    mp6_unlock();
    if (underruns != g_streamUnderrunsReported) {
        fprintf(stderr, "[AUDIO] msm_bridge: music played %lu frames ahead of its decode "
                "(the game thread stalled longer than the decode lead)\n",
                underruns - g_streamUnderrunsReported);
        g_streamUnderrunsReported = underruns;
    }
}

/* Decode the rest of `ch`'s track now (savestate replay seeks into it). */
void mp6_msm_stream_finish_decode(int ch)
{
    MsmStreamJob *job;
    if (ch < 0 || ch >= MP6_MSM_MAX_CHAN) return;
    job = &g_streamJobs[ch];
    if (!job->pcm || g_chan[ch].pcm != job->pcm) {
        job->pcm = NULL;
        return;
    }
    (void)msm_stream_job_decode(job, job->pcm, job->adpcmFrames);
    mp6_lock();
    g_chan[ch].decodedFrames = job->nextFrame * 14;
    mp6_unlock();
    job->pcm = NULL;
}

/* =======================================================================
 * msmSysInit -- parses the real .pdt directory (see this file's header
 * comment for the format) and brings up the live audio backend, then
 * parses the SEPARATE .msm SE/Mus group bank (init->msmPath, see
 * msm_se_bank_init in msm_bank.c).
 *
 * ALWAYS returns >=0 (success), even when the .pdt can't be read: real
 * hardware would presumably report the read failure, but game/audio.c's
 * own HuAudInit hard-hangs (`while(1);`) on any negative result with no
 * diagnostic recovery at all, and "menus navigable, no music" is a far
 * more useful degraded state for this port than a silent, unrecoverable
 * boot hang over an optional feature. The failure is still loud (stderr +
 * stdout), never silent.
 * ======================================================================= */
s32 msmSysCheckInit(void)
{
    /* msmSysInit intentionally succeeds in silent/degraded mode, but once it
     * has established the mixer locks the subsystem is installed and reset
     * callers must mute it.  This flag is monotonic after single-threaded
     * boot, so no lock is needed for the reset-time read. */
    return g_mixerLockInit ? TRUE : FALSE;
}

s32 msmSysInit(MSM_INIT *init, MSM_ARAM *aram)
{
    uint8_t hdrBuf[32];
    uint32_t version, chanMaxU, sampleFrq, maxBufs;
    uint32_t packListOfs, adpcmOfs, packOfs, sampOfs;
    uint32_t packListBytes, coefBytes, packBytes;

    (void)aram;

    if (!g_mixerLockInit) {
        mp6_host_mutex_init(&g_mixerLock);
        mp6_host_mutex_init(&g_grpLock); /* see g_grpLock's own comment */
        g_mixerLockInit = 1;
    }
    /* Initialize the active slot limit before the first audio callback.
     * Later settings changes take the mixer lock.
     *
     * Logged ONLY when the enhancement is on. Retail stays byte-silent on
     * purpose: docs/TESTING.md's headless 600-tick gate byte-compares every
     * game-flow line (including [AUDIO] ones) against docs/ua1/
     * win_headless_600.log and against the Android device log, so an
     * unconditional line would fail both gates for a run in which nothing
     * about the audio actually changed. Same "no log noise when the mod is
     * off" rule mp6_shadow_quality_scale() follows. */
    if (mp6_sfx_voice_cap() != MP6_MSM_SFX_VOICES_RETAIL) {
        printf("[AUDIO] extended SFX voice table: %d of %d slots active\n",
               mp6_sfx_voice_cap(), MP6_MSM_MAX_SFX_VOICES);
    }

    printf("[AUDIO] msmSysInit(msmPath=\"%s\", pdtPath=\"%s\", heapSize=%u)\n",
           init->msmPath ? init->msmPath : "(null)",
           init->pdtPath ? init->pdtPath : "(null)",
           (unsigned)init->heapSize);

    g_pdtReady = 0;
    g_pdtFileSize = 0;
    memset(g_chan, 0, sizeof(g_chan));
    g_masterVol = 127;

    if (!init->pdtPath) {
        printf("[AUDIO] msmSysInit: no pdtPath given -- streaming stays silent\n");
        return 0;
    }
    {
        size_t n = strlen(init->pdtPath);
        if (n >= sizeof(g_pdtPath)) {
            fprintf(stderr, "[AUDIO] msmSysInit: pdtPath too long (%zu bytes) -- streaming stays silent\n", n);
            return 0;
        }
        memcpy(g_pdtPath, init->pdtPath, n + 1);
    }

    if (!msm_audio_file_size(g_pdtPath, &g_pdtFileSize) || g_pdtFileSize < sizeof(hdrBuf)) {
        fprintf(stderr, "[AUDIO] msmSysInit: .pdt is missing or shorter than its header -- "
                "streaming stays silent\n");
        return 0;
    }

    if (!pdt_read_range(0, sizeof(hdrBuf), hdrBuf)) {
        printf("[AUDIO] msmSysInit: couldn't read the .pdt header -- streaming stays silent\n");
        return 0;
    }
    version   = be16(hdrBuf + 0);
    g_streamMax = (int)be16(hdrBuf + 2);
    chanMaxU  = be32(hdrBuf + 4);
    sampleFrq = be32(hdrBuf + 8);
    maxBufs   = be32(hdrBuf + 12);
    packListOfs = be32(hdrBuf + 16);
    adpcmOfs    = be32(hdrBuf + 20);
    packOfs     = be32(hdrBuf + 24);
    sampOfs     = be32(hdrBuf + 28);

    printf("[AUDIO] msmSysInit: .pdt header version=%u streamMax=%d chanMax=%u sampleFrq=%u maxBufs=%u\n",
           (unsigned)version, g_streamMax, (unsigned)chanMaxU, (unsigned)sampleFrq, (unsigned)maxBufs);

    if (version != MSM_PDT_FILE_VERSION_LOCAL) {
        fprintf(stderr, "[AUDIO] msmSysInit: unexpected .pdt version %u (expected %u) -- streaming "
                "stays silent\n", (unsigned)version, (unsigned)MSM_PDT_FILE_VERSION_LOCAL);
        return 0;
    }
    if (g_streamMax <= 0 || packListOfs < sizeof(hdrBuf) ||
        packOfs < adpcmOfs || sampOfs < packOfs || adpcmOfs < packListOfs ||
        sampOfs > g_pdtFileSize) {
        fprintf(stderr, "[AUDIO] msmSysInit: implausible .pdt header offsets -- streaming stays silent\n");
        return 0;
    }

    g_chanMax = (int)chanMaxU;
    if (g_chanMax <= 0) g_chanMax = 1;
    if (g_chanMax > MP6_MSM_MAX_CHAN) g_chanMax = MP6_MSM_MAX_CHAN;

    packListBytes = adpcmOfs - packListOfs;
    coefBytes = packOfs - adpcmOfs;
    packBytes = sampOfs - packOfs;
    if ((uint64_t)g_streamMax * 4u > packListBytes ||
        coefBytes == 0 || (coefBytes % 32) != 0 || coefBytes > (uint32_t)INT_MAX ||
        packBytes < 32 || packBytes > (uint32_t)INT_MAX ||
        !mp6_msm_span_valid_u32(g_pdtFileSize, packListOfs, packListBytes) ||
        !mp6_msm_span_valid_u32(g_pdtFileSize, adpcmOfs, coefBytes) ||
        !mp6_msm_span_valid_u32(g_pdtFileSize, packOfs, packBytes)) {
        fprintf(stderr, "[AUDIO] msmSysInit: malformed .pdt table spans "
                "(file=%u list=%#x+%#x streams=%d coef=%#x+%#x pack=%#x+%#x) -- "
                "streaming stays silent\n", (unsigned)g_pdtFileSize,
                (unsigned)packListOfs, (unsigned)packListBytes, g_streamMax,
                (unsigned)adpcmOfs, (unsigned)coefBytes,
                (unsigned)packOfs, (unsigned)packBytes);
        return 0;
    }

    free(g_packListOfs); g_packListOfs = NULL;
    free(g_packBlob); g_packBlob = NULL;
    free(g_coef); g_coef = NULL;
    g_numCoef = (int)(coefBytes / 32);

    g_packListOfs = (uint32_t *)malloc((size_t)g_streamMax * sizeof(uint32_t));
    g_packBlob = (uint8_t *)malloc(packBytes);
    g_coef = (MP6AdpcmCoefTable *)malloc((size_t)g_numCoef * sizeof(MP6AdpcmCoefTable));
    g_packBlobBase = packOfs;
    g_packBlobSize = packBytes;

    if (!g_packListOfs || !g_packBlob || (g_numCoef > 0 && !g_coef)) {
        fprintf(stderr, "[AUDIO] msmSysInit: out of memory reading .pdt tables -- streaming stays silent\n");
        free(g_packListOfs); g_packListOfs = NULL;
        free(g_packBlob); g_packBlob = NULL;
        free(g_coef); g_coef = NULL;
        return 0;
    }

    {
        uint32_t listReadBytes = (uint32_t)g_streamMax * 4u;
        uint8_t *rawList = (uint8_t *)malloc(listReadBytes);
        uint8_t *rawCoef = (uint8_t *)malloc((size_t)g_numCoef * 32);
        BOOL ok = (rawList != NULL) && (rawCoef != NULL || g_numCoef == 0);

        if (ok) ok = pdt_read_range(packListOfs, listReadBytes, rawList);
        if (ok && g_numCoef > 0) ok = pdt_read_range(adpcmOfs, (uint32_t)g_numCoef * 32, rawCoef);
        if (ok) ok = pdt_read_range(packOfs, packBytes, g_packBlob);

        if (ok) {
            int i;
            for (i = 0; i < g_streamMax; i++) {
                g_packListOfs[i] = be32(rawList + (size_t)i * 4);
            }
            for (i = 0; i < g_numCoef; i++) {
                int p;
                for (p = 0; p < 8; p++) {
                    g_coef[i].coef[p][0] = (int16_t)be16(rawCoef + (size_t)i * 32 + (size_t)p * 4 + 0);
                    g_coef[i].coef[p][1] = (int16_t)be16(rawCoef + (size_t)i * 32 + (size_t)p * 4 + 2);
                }
            }
            g_pdtReady = 1; /* enables msm_pdt_get_pack for the validation pass below */
            for (i = 0; i < g_streamMax; i++) {
                PdtPack check;
                uint32_t bytes;
                int channels, ch;
                if (g_packListOfs[i] == 0) continue; /* authored removed ID */
                if (!msm_pdt_get_pack(i, &check)) {
                    fprintf(stderr, "[AUDIO] msmSysInit: stream %d has an invalid pack offset\n", i);
                    g_pdtReady = 0;
                    break;
                }
                bytes = check.loopEndByte;
                channels = (check.flag & MP6_PACK_FLAG_STEREO) ? 2 : 1;
                if (bytes == 0 || ((check.flag & MP6_PACK_FLAG_LOOP) &&
                                   check.loopStartByte >= check.loopEndByte)) {
                    fprintf(stderr, "[AUDIO] msmSysInit: stream %d has invalid loop bounds\n", i);
                    g_pdtReady = 0;
                    break;
                }
                for (ch = 0; ch < channels; ch++) {
                    if (check.subCoefIdx[ch] < 0 || check.subCoefIdx[ch] >= g_numCoef ||
                        check.subSampleOfs[ch] < sampOfs ||
                        !mp6_msm_span_valid_u32(g_pdtFileSize, check.subSampleOfs[ch], bytes)) {
                        fprintf(stderr, "[AUDIO] msmSysInit: stream %d channel %d has an "
                                "invalid sample/coefficient span\n", i, ch);
                        g_pdtReady = 0;
                        break;
                    }
                }
                if (!g_pdtReady) break;
            }
            if (g_pdtReady) {
                printf("[AUDIO] msmSysInit: .pdt tables loaded (%d streams, %d ADPCM coef tables, chanMax=%d) "
                       "-- real streamed-music playback is ACTIVE\n", g_streamMax, g_numCoef, g_chanMax);
            } else {
                free(g_packListOfs); g_packListOfs = NULL;
                free(g_packBlob); g_packBlob = NULL;
                free(g_coef); g_coef = NULL;
                g_numCoef = 0;
            }
        } else {
            fprintf(stderr, "[AUDIO] msmSysInit: failed reading .pdt tables -- streaming stays silent\n");
        }
        free(rawList);
        free(rawCoef);
    }

    /* Test-only diagnostic aid, opt-in via MP6_AUDIO_SELFTEST_STREAMS (a
     * comma-separated list of real MSM_STREAM_* ids, e.g. "0,1" for the
     * opening cutscene + title-screen tracks) -- NOT part of normal
     * playback (the organic game flow's own opening.c/boot.c call sites
     * are the real, unmodified callers this stands in for), same "opt-in,
     * clearly test-only" spirit as src/gx/aurora_bridge.c's existing
     * MP6_AUTO_START_TICKS/--input-script mechanisms. Exists because
     * --headless's own boot flow never reaches ANY real msmStreamPlay
     * call site at all: src/game/pad.c's PadReadVSync (the ONLY call site
     * anywhere in the decomp for msmSysRegularProc, and the function that
     * actually populates HuPadBtnDown[] the warning screen's own
     * MAX_INPUT_WAIT_FRAMES=3510 wait loop reads) only ever runs via the
     * VI post-retrace callback game/pad.c's HuPadInit registers -- and
     * --headless's own VI shims (src/null/shims_generated.c's
     * auto-generated VISetPostRetraceCallback; src/null/
     * shims_manual.c's hand-written VIWaitForRetrace) never store or
     * invoke that callback at all. A real, pre-existing, cross-cutting
     * src/null VI-callback gap, outside src/audio/'s own scope
     * -- reported, not patched. This hook is what makes it possible to
     * still verify the REAL .pdt/decode/mix/loop/WAV-dump pipeline
     * end-to-end against real disc streams in --headless despite that
     * external blocker.
     *
     * The DEFAULT (aurora/windowed) build does NOT have this problem --
     * src/gx/aurora_bridge.c's own VISetPostRetraceCallback/
     * VIWaitForRetrace really do store and fire the callback every tick
     * (a windowed run using aurora_bridge.c's own --input-script to inject
     * a real PAD_BUTTON_START organically drives the real, unmodified
     * boot->opening->title->file-select flow all the way through, hitting
     * msmStreamPlay(0)/(1)/(2) from their REAL game-code call sites --
     * OpeningExec, BootTitleExec, filesel.c -- with zero SDL underruns).
     * This selftest hook is therefore a --headless-only substitute for a
     * gap that genuinely doesn't exist in the windowed build. */
    if (g_pdtReady) {
        const char *selftest = getenv("MP6_AUDIO_SELFTEST_STREAMS");
        if (selftest && selftest[0]) {
            char buf[256];
            char *tok;
            char *next = NULL;
            size_t n = strlen(selftest);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, selftest, n);
            buf[n] = '\0';
            printf("[AUDIO] MP6_AUDIO_SELFTEST_STREAMS=\"%s\" -- test-only: directly exercising "
                   "msmStreamPlay for these ids (see this file's msmSysInit comment for why)\n", selftest);
            /* Plain strtok is fine here -- msmSysInit runs once, single-
             * threaded, before mp6_audio_out_init() could possibly start a
             * second (SDL audio) thread. */
            tok = strtok(buf, ",");
            while (tok) {
                int id = atoi(tok);
                next = strtok(NULL, ","); /* fetch before the call -- msmStreamPlay's own
                                              logging/decode has no reason to touch strtok's
                                              static state, but grabbing it first costs nothing
                                              and removes any doubt */
                msmStreamPlay(id, NULL);
                tok = next;
            }
        }

        /* MP6_AUDIO_SELFTEST_FADE="streamId,speedMs" -- same "opt-in,
         * clearly test-only, --headless substitute for a real
         * VI-callback-driven organic call" spirit as
         * MP6_AUDIO_SELFTEST_STREAMS just above, specifically for
         * verifying the fade-envelope ramp in isolation (the organic
         * title->file-select transition calls msmStreamStopAll(1000)
         * immediately followed by msmStreamStop(chan, 0) on the SAME
         * channel, which cancels the fade before a single frame renders,
         * so that real call site can't be used to verify this feature
         * audibly; this hook plays a track then immediately arms a real,
         * uncancelled fade-out on it, which mp6_msm_render then plays out
         * over real rendered frames exactly as any organic caller's fade
         * would). */
        {
            const char *fadeTest = getenv("MP6_AUDIO_SELFTEST_FADE");
            if (fadeTest && fadeTest[0]) {
                char fbuf[64];
                char *comma;
                int streamId, speedMs, chan;
                size_t fn = strlen(fadeTest);
                if (fn >= sizeof(fbuf)) fn = sizeof(fbuf) - 1;
                memcpy(fbuf, fadeTest, fn);
                fbuf[fn] = '\0';
                comma = strchr(fbuf, ',');
                streamId = atoi(fbuf);
                speedMs = comma ? atoi(comma + 1) : 1000;
                printf("[AUDIO] MP6_AUDIO_SELFTEST_FADE=\"%s\" -- test-only: play streamId=%d then "
                       "immediately arm msmStreamStop(chan, %d) (a real, uncancelled fade)\n",
                       fadeTest, streamId, speedMs);
                chan = msmStreamPlay(streamId, NULL);
                if (chan >= 0) {
                    msmStreamStop(chan, speedMs);
                }
            }
        }
    }

    /* Parse the SEPARATE MP6_SND.msm SFX bank (see this file's own big
     * SFX header comment above msm_se_bank_init). Never blocks boot --
     * see that function's own header comment. */
    msm_se_bank_init(init->msmPath);

    /* Test-only diagnostic aid, opt-in via MP6_AUDIO_SELFTEST_SE (a comma-
     * separated list of MSM_SE_* ids, e.g. "2,24" for the choice-confirm +
     * message-advance common SFX) -- same "opt-in, clearly test-only"
     * spirit as MP6_AUDIO_SELFTEST_STREAMS just above, for exactly the same
     * reason (--headless's own boot flow never organically reaches a real
     * msmSePlay call site either -- the VI-callback gap documented at
     * length in this file's own msmSysRegularProc/mp6_audio_out_init
     * comments applies identically here). The DEFAULT (aurora/windowed)
     * build reaches real msmSePlay call sites organically instead
     * (game/window.c's HuWinKeyWait/choiceEndSe). */
    if (g_msmReady) {
        /* MP6_AUDIO_SELFTEST_GROUPS="<grpIdx>,..." -- test-only, processed
         * BEFORE the SE selftest below so a previously -122 (not-in-a-
         * base-group) SE id can be exercised in ISOLATION: load its group
         * here exactly as the game's own HuAudSndGrpSetSet would, then let
         * SELFTEST_SE play it. Same "opt-in, clearly test-only,
         * --headless substitute for an organic call" spirit as every
         * other selftest hook here. */
        const char *grpSelftest = getenv("MP6_AUDIO_SELFTEST_GROUPS");
        if (grpSelftest && grpSelftest[0]) {
            static char s_selftestStaging[16]; /* non-NULL staging only -- never dereferenced */
            char gbuf[128];
            char *gtok, *gnext = NULL;
            size_t gn = strlen(grpSelftest);
            if (gn >= sizeof(gbuf)) gn = sizeof(gbuf) - 1;
            memcpy(gbuf, grpSelftest, gn);
            gbuf[gn] = '\0';
            printf("[AUDIO] MP6_AUDIO_SELFTEST_GROUPS=\"%s\" -- test-only: msmSysLoadGroup for these "
                   "grpInfo indices (stands in for the game's own HuAudSndGrpSetSet)\n", grpSelftest);
            gtok = strtok(gbuf, ",");
            while (gtok) {
                int gid = atoi(gtok);
                gnext = strtok(NULL, ",");
                msmSysLoadGroup(gid, s_selftestStaging, FALSE);
                gtok = gnext;
            }
        }
    }

    if (g_msmReady) {
        const char *selftest = getenv("MP6_AUDIO_SELFTEST_SE");
        if (selftest && selftest[0]) {
            char buf[256];
            char *tok, *next = NULL;
            size_t n = strlen(selftest);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, selftest, n);
            buf[n] = '\0';
            printf("[AUDIO] MP6_AUDIO_SELFTEST_SE=\"%s\" -- test-only: directly exercising msmSePlay "
                   "for these SE ids\n", selftest);
            tok = strtok(buf, ",");
            while (tok) {
                int id = atoi(tok);
                next = strtok(NULL, ",");
                msmSePlay(id, NULL);
                tok = next;
            }
        }
    }

    mp6_leaktest_maybe_start();
    mp6_se_leaktest_maybe_start();
    mp6_grpswap_leaktest_maybe_start(); /* see its own comment */
    mp6_audio_out_init();

    return 0;
}

/* =======================================================================
 * msmSysRegularProc -- the real per-frame pump (game/pad.c's PadReadVSync
 * calls this once per tick, itself only reachable via the VI post-retrace
 * callback -- see mp6_audio_out_init()'s own header comment above for why
 * that chain is CURRENTLY DEAD in --headless specifically). Intent,
 * if/when that gap is ever fixed elsewhere: --headless has no live audio
 * device to drive in
 * real time, so it would pump the SAME shared mixer (msm_mixer.h) itself
 * here, purely so channel playback position advances realistically and
 * MP6_AUDIO_WAV_DUMP can capture real audio without a display OR a live
 * device. Default (aurora) build: a pure no-op here -- real-time playback
 * is driven by src/audio/audio_out_sdl.c's own SDL callback instead
 * (see msm_mixer.h's header comment for why NOT both at once).
 *
 * NOTE for whoever fixes the VI gap: once this actually starts firing,
 * double-check this doesn't double-render against
 * mp6_headless_drain_wav_dump()'s own one-shot drain (mp6_audio_out_init,
 * above) for the specific window where both could theoretically overlap
 * -- in practice g_wavDone already gates both paths (mp6_wav_capture's
 * own early-return), so the only real interaction is channel playback
 * POSITION having already been advanced by this file's own one-shot drain
 * before this per-tick pump ever gets its first real call; see that
 * drain's own "KNOWN LIMITATION" note.
 * ======================================================================= */
/* Pull-side audio snapshot (include/mp6_diag_probe.h).
 *
 * Same discipline as mp6_se_timeline_census() (msm_diag.c), and for the same
 * stated reason: the SDL audio callback needs g_mixerLock, so this COPIES the
 * voice and channel tables under the lock and does no formatting, no printf
 * and no allocation while holding it. The caller formats from its own copy.
 *
 * Every field here was already maintained unconditionally -- only the [SETL]
 * PRINTING is gated by mp6_se_timeline_on(). So a live audio panel costs one
 * bounded memcpy-shaped copy per refresh and arms nothing. */
void mp6_diag_audio_snapshot(Mp6DiagAudio *out)
{
    int i;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    /* The RUNTIME cap, not the struct capacity: voiceCount is what the audio
     * panel prints as "of N mixer slots" and what it LOOPS OVER, so reporting
     * 32 in a 16-slot run would claim 16 slots that provably cannot exist.
     * No runtime clamp is needed -- the #error next to MP6_MSM_MAX_SFX_VOICES
     * makes MP6_DIAG_SFX_VOICE_MAX >= every possible cap a build-time fact. */
    out->voiceCount = mp6_sfx_voice_cap();
    out->chanCount = MP6_DIAG_CHAN_MAX;

    mp6_lock();
    out->chanConfigured = g_chanMax;
    out->masterVol = g_masterVol;
    out->seMasterVol = g_seMasterVol;
    out->keygroupEvents = g_kgReleaseEvents;
    out->keygroupVoices = g_kgVoicesReleased;
    for (i = 0; i < out->voiceCount; i++) {
        const MsmSeVoice *v = &g_sfxVoice[i];
        out->voices[i].active = v->active;
        out->voices[i].no = v->no;
        out->voices[i].seId = v->seId;
        out->voices[i].loop = v->loop;
        out->voices[i].keyGroup = v->keyGroup;
        out->voices[i].wraps = v->tlWraps;
        out->voices[i].startTick = v->tlStartTick;
        if (v->active) out->voicesActive++;
    }
    for (i = 0; i < MP6_DIAG_CHAN_MAX && i < MP6_MSM_MAX_CHAN; i++) {
        const MsmChan *c = &g_chan[i];
        out->chans[i].active = c->active;
        out->chans[i].paused = c->paused;
        out->chans[i].loop = c->loop;
        out->chans[i].streamId = c->streamId;
        out->chans[i].fadeAction = c->fadeAction;
        out->chans[i].vol = c->vol;
        out->chans[i].fadeMul = c->fadeMul;
        if (c->active) out->chansActive++;
    }
    mp6_unlock();
}

static _Atomic int g_pcmRetired;

void mp6_msm_collect_finished(void)
{
    MsmPcmRetirement retired = {0};
    int i;
    if (!g_mixerLockInit || !atomic_exchange(&g_pcmRetired, 0)) return;
    mp6_lock();
    for (i = 0; i < g_chanMax; ++i) {
        if (!g_chan[i].active && g_chan[i].pcm) {
            mp6_pcm_detach(&retired, &g_chan[i].pcm);
        }
    }
    for (i = 0; i < MP6_MSM_MAX_SFX_VOICES; ++i) {
        if (!g_sfxVoice[i].active && g_sfxVoice[i].pcm) {
            mp6_pcm_detach(&retired, &g_sfxVoice[i].pcm);
        }
    }
    mp6_unlock();
    mp6_pcm_release(&retired);
}

void msmSysRegularProc(void)
{
    mp6_msm_collect_finished();
    mp6_msm_stream_decode_ahead();
    msm_se_pool_fill();
    msm_se_decode_ahead();
    if (mp6_se_timeline_on()) mp6_se_timeline_census();
#ifdef MP6_HEADLESS_BUILD
    static uint64_t s_ticks;
    static uint64_t s_framesEmitted;
    static int s_everCalled;
    uint64_t wantFrames;
    uint32_t frames;

    if (!s_everCalled) {
        s_everCalled = 1;
        printf("[AUDIO-DIAG] msmSysRegularProc CALLED for the first time (g_pdtReady=%d)\n", g_pdtReady);
        fflush(stdout);
    }
    if (!g_pdtReady) return;

    s_ticks++;
    if (s_ticks == 1 || (mp6_tick_count > 0 && (mp6_tick_count % 500) == 0)) {
        /* Both the CONTENT and the PACING of this line key off the GAME's
         * tick counter, not s_ticks: this TU is carved out of the savestate,
         * so s_ticks does not rewind with a restore -- and this line goes to
         * stdout, where the savestate gate's byte-exact replay oracle reads
         * it. mp6_tick_count is restored (same cadence -- one regular-proc
         * call per game tick), so after a rewind the line reappears at the
         * same game ticks with the same numbers the straight-through
         * baseline has. Pacing on s_ticks instead would fire at the wrong
         * game ticks after a rewind and permanently desync the oracle. */
        int wavArmed;
        mp6_lock();
        wavArmed = g_wavArmed;
        mp6_unlock();
        printf("[AUDIO-DIAG] msmSysRegularProc tick #%llu, g_wavArmed=%d\n",
               (unsigned long long)mp6_tick_count, wavArmed);
        fflush(stdout);
    }
    /* Exact (no fixed-point drift) 60Hz-tick -> MP6_MSM_OUT_RATE-Hz-audio
     * rate conversion -- classic Bresenham-style "how many whole frames
     * are owed by now" accounting. */
    wantFrames = (s_ticks * MP6_MSM_OUT_RATE) / 60u;
    frames = (uint32_t)(wantFrames - s_framesEmitted);
    s_framesEmitted = wantFrames;

    while (frames > 0) {
        int16_t scratch[MP6_RENDER_SCRATCH_FRAMES * MP6_MSM_OUT_CHANNELS];
        uint32_t chunk = frames > MP6_RENDER_SCRATCH_FRAMES ? MP6_RENDER_SCRATCH_FRAMES : frames;
        mp6_msm_render(scratch, chunk);
        frames -= chunk;
    }
#endif
}

/* =======================================================================
 * The shared mixer (msm_mixer.h) -- see this file's header comment for
 * the full design. Called either by the --headless pump above or by
 * audio_out_sdl.c's real SDL callback; never both in the same build.
 * ======================================================================= */
void mp6_msm_render(int16_t *out, uint32_t frames)
{
    int i;
    int wavArmed;
    int voiceCap;
    uint32_t f;

    memset(out, 0, (size_t)frames * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));

    /* The SDL callback is normally started only after msmSysInit, but keep
     * this public mixer seam fail-safe if a host backend probes it earlier.
     * Mp6Mutex zero storage is explicitly not an initialized mutex. */
    if (!g_mixerLockInit) {
        return;
    }

    mp6_lock();
    if (g_savestateMixerMuted) {
        mp6_unlock();
        return;
    }
    for (i = 0; i < g_chanMax; i++) {
        MsmChan *c = &g_chan[i];
        float gain;

        /* A channel mid-fade is NEVER `paused` (see msmStreamPause/
         * PauseAll below -- `paused` only ever flips to 1 once a
         * MP6_FADE_TO_PAUSE ramp actually completes, and flips to 0 the
         * instant a MP6_FADE_TO_PLAY ramp starts, not once IT completes)
         * -- so this skip condition needs no fade-awareness of its own,
         * it already does the right thing either way. */
        if (!c->active || c->paused || !c->pcm || c->totalFrames == 0) continue;
        gain = (c->baseVol / 127.0f) * (c->vol / 127.0f) * (g_masterVol / 127.0f);

        for (f = 0; f < frames; f++) {
            uint32_t idx = (uint32_t)(c->posFrac >> 16);
            int32_t l, r;

            if (idx >= c->totalFrames) {
                if (!mp6_msm_resolve_end_q16(&c->posFrac, c->loop,
                                             c->loopStartFrame, c->totalFrames)) {
                    c->active = 0;
                    g_pcmRetired = 1;
                    c->fadeAction = MP6_FADE_NONE;
                    c->fadeStep = 0.0f;
                    break;
                }
                idx = (uint32_t)(c->posFrac >> 16);
            }
            if (idx < c->decodedFrames) {
                l = c->pcm[idx * MP6_MSM_OUT_CHANNELS + 0];
                r = c->pcm[idx * MP6_MSM_OUT_CHANNELS + 1];
            } else { /* not decoded yet: see "Streamed music decode" */
                l = r = 0;
                g_streamUnderruns++;
            }
            out[f * MP6_MSM_OUT_CHANNELS + 0] = mp6_clamp16(out[f * MP6_MSM_OUT_CHANNELS + 0] + (int32_t)(l * gain * c->fadeMul));
            out[f * MP6_MSM_OUT_CHANNELS + 1] = mp6_clamp16(out[f * MP6_MSM_OUT_CHANNELS + 1] + (int32_t)(r * gain * c->fadeMul));
            c->posFrac += c->stepFrac;

            /* Fade-envelope ramp -- one linear step per OUTPUT frame,
             * sample-accurate (not a per-callback-chunk step), matching
             * this file's own header comment ("apply them in the mixer
             * ramp"). See the MP6_FADE_* enum's own comment for what each
             * action does on completion. */
            if (c->fadeAction != MP6_FADE_NONE) {
                c->fadeMul += c->fadeStep;
                if (c->fadeStep < 0.0f && c->fadeMul <= 0.0f) {
                    c->fadeMul = 0.0f;
                    if (c->fadeAction == MP6_FADE_TO_STOP) {
                        c->active = 0;
                        g_pcmRetired = 1;
                        c->fadeAction = MP6_FADE_NONE;
                        c->fadeStep = 0.0f;
                        break; /* inactive PCM is reclaimed on the game thread */
                    }
                    /* MP6_FADE_TO_PAUSE completed. */
                    c->paused = 1;
                    c->fadeAction = MP6_FADE_NONE;
                    c->fadeStep = 0.0f;
                } else if (c->fadeStep > 0.0f && c->fadeMul >= 1.0f) {
                    c->fadeMul = 1.0f;
                    c->fadeAction = MP6_FADE_NONE; /* MP6_FADE_TO_PLAY completed */
                    c->fadeStep = 0.0f;
                }
            }
        }
    }

    /* SFX voices -- same shape as the BGM channel loop above, with a MONO
     * source dual-panned into stereo via gainL/gainR. Authored loop bounds
     * wrap in Q16.16; one-shot samples deactivate at their exact length.
     *
     * The bound is read ONCE into a local: this is the audio callback's own
     * inner loop. The mixer lock keeps the cap stable for this callback.
     * At 16 this walks exactly the retail slots. */
    voiceCap = mp6_sfx_voice_cap();
    for (i = 0; i < voiceCap; i++) {
        MsmSeVoice *v = &g_sfxVoice[i];
        float gain;

        if (!v->active || v->paused || !v->pcm || v->totalFrames == 0) continue;
        gain = (v->baseVol / 127.0f) * (v->vol / 127.0f) * (g_seMasterVol / 127.0f);

        for (f = 0; f < frames; f++) {
            uint32_t idx = (uint32_t)(v->posFrac >> 16);
            int32_t s, l, r;
            float fadedGain;

            if (idx >= (v->loop ? v->loopEndFrame : v->totalFrames)) {
                if (!mp6_msm_resolve_end_q16(&v->posFrac, v->loop,
                                             v->loopStartFrame, v->loopEndFrame)) {
                    v->active = 0;
                    g_pcmRetired = 1;
                    v->fadeAction = MP6_FADE_NONE;
                    v->fadeStep = 0.0f;
                    break;
                }
                v->tlWraps++; /* diagnostic only -- see MP6_AUDIO_TIMELINE */
                idx = (uint32_t)(v->posFrac >> 16);
            }
            fadedGain = gain * v->fadeMul;
            /* THE MACRO'S AUTHORED VOLUME ENVELOPE. Guarded on segCount, not
             * folded in unconditionally: a voice with no modellable envelope
             * must not even acquire an extra multiply, so its samples stay
             * bit-identical to the pre-envelope build. `idx` is this voice's
             * position in its own native-rate frames, which is exactly the
             * clock mp6_msm_env_plan resolved the program onto. */
            if (v->envPlan.segCount != 0) {
                fadedGain *= mp6_msm_env_mul(&v->envPlan, (uint64_t)idx);
            }
            s = v->pcm[idx];
            l = (int32_t)(s * fadedGain * v->gainL);
            r = (int32_t)(s * fadedGain * v->gainR);
            out[f * MP6_MSM_OUT_CHANNELS + 0] = mp6_clamp16(out[f * MP6_MSM_OUT_CHANNELS + 0] + l);
            out[f * MP6_MSM_OUT_CHANNELS + 1] = mp6_clamp16(out[f * MP6_MSM_OUT_CHANNELS + 1] + r);
            v->posFrac += v->stepFrac;

            /* Fade envelope -- same shape as the BGM loop above, see that
             * loop's own comment and the MP6_FADE_* enum. */
            if (v->fadeAction != MP6_FADE_NONE) {
                v->fadeMul += v->fadeStep;
                if (v->fadeStep < 0.0f && v->fadeMul <= 0.0f) {
                    v->fadeMul = 0.0f;
                    if (v->fadeAction == MP6_FADE_TO_STOP) {
                        v->active = 0;
                        g_pcmRetired = 1;
                        v->fadeAction = MP6_FADE_NONE;
                        v->fadeStep = 0.0f;
                        break;
                    }
                    v->paused = 1;
                    v->fadeAction = MP6_FADE_NONE;
                    v->fadeStep = 0.0f;
                } else if (v->fadeStep > 0.0f && v->fadeMul >= 1.0f) {
                    v->fadeMul = 1.0f;
                    v->fadeAction = MP6_FADE_NONE;
                    v->fadeStep = 0.0f;
                }
            }
        }
    }
    wavArmed = g_wavArmed;
    mp6_unlock();

    mp6_wav_capture(out, frames, wavArmed);
}

/* =======================================================================
 * msmStreamPlay / Stop / Pause / PauseAll / SetParam / GetStatus / StopAll /
 * SetMasterVolume -- the game-facing stream control API (game/audio.c's
 * HuAudSStream*, HuAudBGM*, HuAudJingle* wrappers).
 * ======================================================================= */
int msmStreamPlay(int streamId, MSM_STREAMPARAM *streamParam)
{
    MsmPcmRetirement retired = {0};
    PdtPack pack;
    int chan;
    int stereo;
    uint32_t frames;
    int16_t *interleaved;
    MsmStreamJob job;
    int vol;
    int startPaused;
    int firstWav = 0;
    uint64_t traceStart = 0;
    s32 decodeResult;
    uint32_t loopStartFrame;

    if (streamParam) {
        printf("[AUDIO] msmStreamPlay(streamId=%d, flag=%#x vol=%d pan=%d span=%d auxA=%d auxB=%d "
               "chan=%d fadeSpeed=%u)\n", streamId, (unsigned)streamParam->flag, streamParam->vol,
               streamParam->pan, streamParam->span, streamParam->auxA, streamParam->auxB,
               streamParam->chan, (unsigned)streamParam->fadeSpeed);
    } else {
        printf("[AUDIO] msmStreamPlay(streamId=%d, streamParam=NULL)\n", streamId);
    }

    if (!g_pdtReady) {
        printf("[AUDIO] msmStreamPlay: no .pdt loaded on this checkout -- returning MSM_ERR_OPENFAIL\n");
        return MSM_ERR_OPENFAIL;
    }
    if (streamId < 0 || streamId >= g_streamMax) return MSM_ERR_INVALIDID;
    if (!msm_pdt_get_pack(streamId, &pack)) return MSM_ERR_REMOVEDID;

    if (streamParam && (streamParam->flag & MSM_STREAMPARAM_CHAN)) {
        chan = streamParam->chan;
        if (chan < 0 || chan >= g_chanMax) return MP6_MSM_ERR_CHANLIMIT;
    } else {
        chan = -1; /* selected atomically under g_mixerLock after decode */
    }

    stereo = (pack.flag & MP6_PACK_FLAG_STEREO) ? 1 : 0;
    {
        uint32_t adpcmFrames = msm_pdt_pack_frame_count(&pack);
        if (adpcmFrames == 0 || adpcmFrames > (UINT32_MAX / 14)) { /* the /14 guard blocks the
                                                                        frames*14 multiply below
                                                                        from silently wrapping */
            printf("[AUDIO] msmStreamPlay: streamId=%d has zero-length or implausible audio -- "
                   "nothing to play\n", streamId);
            return MSM_ERR_INVALIDFILE;
        }
        frames = adpcmFrames * 14; /* PCM sample count -- see msm_pdt_pack_frame_count's own
                                       comment for why this multiply must happen HERE, once,
                                       rather than being baked into that helper */

        if (!mp6_msm_decode_budget_valid(frames, 8u)) {
            fprintf(stderr,
                    "[AUDIO] msmStreamPlay: streamId=%d decode budget rejected %u PCM frames\n",
                    streamId, (unsigned)frames);
            return MSM_ERR_INVALIDFILE;
        }

        if ((uint64_t)frames * MP6_MSM_OUT_CHANNELS * sizeof(int16_t) > SIZE_MAX ||
            (uint64_t)frames * sizeof(int16_t) > SIZE_MAX) {
            return MSM_ERR_OUTOFMEM;
        }
        interleaved = (int16_t *)malloc((size_t)frames * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));
        if (!interleaved) {
            fprintf(stderr, "[AUDIO] msmStreamPlay: out of memory allocating %u frames\n", (unsigned)frames);
            return MSM_ERR_OUTOFMEM;
        }

        memset(&job, 0, sizeof(job));
        job.adpcmFrames = adpcmFrames;
        job.framesPerSecond = ((pack.frq ? pack.frq : MP6_MSM_OUT_RATE) + 13u) / 14u;
        job.stereo = stereo;
        job.sampleOfs[0] = pack.subSampleOfs[0];
        job.coefIdx[0] = pack.subCoefIdx[0];
        job.sampleOfs[1] = pack.subSampleOfs[1];
        job.coefIdx[1] = pack.subCoefIdx[1];
        decodeResult = msm_substream_check(job.sampleOfs[0], job.coefIdx[0], adpcmFrames);
        if (decodeResult == 0 && stereo) {
            decodeResult = msm_substream_check(job.sampleOfs[1], job.coefIdx[1], adpcmFrames);
        }
        if (decodeResult == 0) {
            /* Only the head now; mp6_msm_stream_decode_ahead fills the rest. */
            uint32_t head = mp6_stream_sync_decode() ? adpcmFrames
                                                     : job.framesPerSecond * MP6_STREAM_HEAD_SECONDS;
            if (head > adpcmFrames) head = adpcmFrames;
            /* Both channels' heads are read one after the other below; on
             * the web each is a round trip to the page, so start both now. */
            if (stereo) {
                mp6_dvd_prefetch_range(g_pdtPath, job.sampleOfs[0], head * 8);
                mp6_dvd_prefetch_range(g_pdtPath, job.sampleOfs[1], head * 8);
            }
            traceStart = mp6_load_trace_begin();
            decodeResult = msm_stream_job_decode(&job, interleaved, head);
            mp6_load_trace_end(traceStart, MP6_LOAD_STREAM,
                               (uint64_t)job.nextFrame * 14 * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));
        }
        if (decodeResult != 0) {
            free(interleaved);
            return decodeResult;
        }
    }

    vol = (streamParam && (streamParam->flag & MSM_STREAMPARAM_VOL)) ? streamParam->vol : MSM_VOL_MAX;
    startPaused = (streamParam && (streamParam->flag & MSM_STREAMPARAM_PAUSE)) ? 1 : 0;
    loopStartFrame = (pack.loopStartByte / 8) * 14;
    if ((pack.flag & MP6_PACK_FLAG_LOOP) && loopStartFrame >= frames) {
        free(interleaved);
        return MSM_ERR_INVALIDFILE;
    }

    mp6_lock();
    if (chan < 0) {
        for (chan = 0; chan < g_chanMax; chan++) {
            if (!g_chan[chan].active) break;
        }
        if (chan == g_chanMax) {
            mp6_unlock();
            free(interleaved);
            return MP6_MSM_ERR_CHANLIMIT;
        }
    }
    mp6_pcm_detach(&retired, &g_chan[chan].pcm);
    g_chan[chan].active = 1;
    g_chan[chan].paused = startPaused;
    g_chan[chan].loop = (pack.flag & MP6_PACK_FLAG_LOOP) ? 1 : 0;
    g_chan[chan].streamId = streamId;
    g_chan[chan].pcm = interleaved;
    g_chan[chan].totalFrames = frames;
    g_chan[chan].decodedFrames = job.nextFrame * 14;
    g_chan[chan].loopStartFrame = loopStartFrame;
    g_chan[chan].posFrac = 0;
    {
        uint32_t nativeFrq = pack.frq ? pack.frq : MP6_MSM_OUT_RATE;
        g_chan[chan].stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    }
    g_chan[chan].baseVol = pack.vol;
    g_chan[chan].vol = vol;
    /* A freshly (re)started channel never inherits a stale fade from
     * whatever this slot was doing before -- detaching the old PCM above
     * unconditionally discards any in-flight MP6_FADE_TO_STOP's
     * own target buffer, so wiping the fade state here (not left to the
     * mixer to notice) is both correct and avoids the mixer ever touching
     * a pointer this function just retired and replaced. */
    g_chan[chan].fadeAction = MP6_FADE_NONE;
    g_chan[chan].fadeStep = 0.0f;
    g_chan[chan].fadeMul = startPaused ? 0.0f : 1.0f;
    if (!g_wavArmed) {
        g_wavArmed = 1;
        firstWav = 1;
    }
    mp6_unlock();

    mp6_pcm_release(&retired);
    job.pcm = job.nextFrame < job.adpcmFrames ? interleaved : NULL;
    if (job.pcm != NULL) msm_stream_job_read_ahead(&job);
    g_streamJobs[chan] = job;
    if (firstWav) {
        /* Arms the WAV-dump capture window (see its own header comment) --
         * "the first ~30s of rendered audio" means the first 30 seconds
         * starting from the first real note, not from process start. */
        printf("[AUDIO] msmStreamPlay: first real playback -- MP6_AUDIO_WAV_DUMP capture window (if "
               "enabled) starts now\n");
    }

    printf("[AUDIO] msmStreamPlay: streamId=%d -> chan=%d %s frq=%uHz authoredVol=%d duration=%.1fs "
           "(%u frames) loopStart=%.1fs%s\n",
           streamId, chan, stereo ? "STEREO" : "mono", (unsigned)pack.frq, (int)pack.vol,
           (double)frames / (pack.frq ? pack.frq : MP6_MSM_OUT_RATE), (unsigned)frames,
           (double)loopStartFrame / (pack.frq ? pack.frq : MP6_MSM_OUT_RATE),
           startPaused ? " (starts PAUSED)" : "");

    return chan;
}

/* Converts a real `speed` parameter into a per-output-frame linear fade
 * step. See this file's own header comment ("Fade envelopes") for the
 * documented speed==milliseconds assumption. speed<=0 returns exactly
 * 0.0f -- every call site below treats that as "keep the previous
 * immediate behavior, untouched". */
static float mp6_fade_step_from_speed(s32 speed)
{
    uint32_t fadeFrames;
    fadeFrames = mp6_msm_fade_frames_from_ms(speed, MP6_MSM_OUT_RATE);
    if (fadeFrames == 0) return 0.0f;
    return 1.0f / (float)fadeFrames;
}

s32 msmStreamStop(int streamNo, s32 speed)
{
    MsmPcmRetirement retired = {0};
    float step;
    printf("[AUDIO] msmStreamStop(streamNo=%d, speed=%d)\n", streamNo, (int)speed);
    if (streamNo < 0 || streamNo >= g_chanMax) return MP6_MSM_ERR_RANGE_STREAM;
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    if (step <= 0.0f || !g_chan[streamNo].active || !g_chan[streamNo].pcm) {
        /* immediate -- exact previous behavior, byte-for-byte, whenever
         * speed<=0 (every existing call site) or there's nothing to fade. */
        g_chan[streamNo].active = 0;
        mp6_pcm_detach(&retired, &g_chan[streamNo].pcm);
        g_chan[streamNo].fadeAction = MP6_FADE_NONE;
        g_chan[streamNo].fadeStep = 0.0f;
    } else {
        /* Fade out over `speed` ms first -- mp6_msm_render's own
         * MP6_FADE_TO_STOP case deactivates and retires PCM on completion. */
        g_chan[streamNo].paused = 0; /* still audibly rendering (fading) until the ramp completes */
        g_chan[streamNo].fadeAction = MP6_FADE_TO_STOP;
        g_chan[streamNo].fadeStep = -step;
    }
    mp6_unlock();
    mp6_pcm_release(&retired);
    return 0;
}

void msmStreamStopAll(s32 speed)
{
    MsmPcmRetirement retired = {0};
    int i;
    float step;
    printf("[AUDIO] msmStreamStopAll(speed=%d)\n", (int)speed);
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < g_chanMax; i++) {
        if (step <= 0.0f || !g_chan[i].active || !g_chan[i].pcm) {
            g_chan[i].active = 0;
            mp6_pcm_detach(&retired, &g_chan[i].pcm);
            g_chan[i].fadeAction = MP6_FADE_NONE;
            g_chan[i].fadeStep = 0.0f;
        } else {
            g_chan[i].paused = 0;
            g_chan[i].fadeAction = MP6_FADE_TO_STOP;
            g_chan[i].fadeStep = -step;
        }
    }
    mp6_unlock();
    mp6_pcm_release(&retired);
}

s32 msmStreamPause(int streamNo, BOOL pause, s32 speed)
{
    float step;
    printf("[AUDIO] msmStreamPause(streamNo=%d, pause=%d, speed=%d)\n", streamNo, (int)pause, (int)speed);
    if (streamNo < 0 || streamNo >= g_chanMax) return MP6_MSM_ERR_RANGE_STREAM;
    mp6_lock();
    if (g_chan[streamNo].active) {
        step = mp6_fade_step_from_speed(speed);
        if (step <= 0.0f) {
            /* immediate -- exact previous behavior */
            g_chan[streamNo].paused = pause ? 1 : 0;
            g_chan[streamNo].fadeAction = MP6_FADE_NONE;
            g_chan[streamNo].fadeStep = 0.0f;
            g_chan[streamNo].fadeMul = pause ? 0.0f : 1.0f;
        } else {
            /* `paused` only settles once the mixer's own ramp actually
             * completes (either direction) -- see mp6_msm_render's
             * MP6_FADE_TO_PAUSE/MP6_FADE_TO_PLAY handling. Clearing it here
             * unconditionally (not just for unpause) is deliberate: a
             * channel already fully paused (fadeMul==0) that gets told to
             * pause AGAIN with a real speed just fades from 0 to 0 -- a
             * harmless one-frame no-op -- rather than being skipped by the
             * mixer's own top-of-loop `paused` check and never running its
             * fade logic at all. */
            g_chan[streamNo].paused = 0;
            g_chan[streamNo].fadeAction = pause ? MP6_FADE_TO_PAUSE : MP6_FADE_TO_PLAY;
            g_chan[streamNo].fadeStep = pause ? -step : step;
        }
    }
    mp6_unlock();
    return 0;
}

s32 msmStreamPauseAll(BOOL pause, s32 speed)
{
    int i;
    float step;
    printf("[AUDIO] msmStreamPauseAll(pause=%d, speed=%d)\n", (int)pause, (int)speed);
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < g_chanMax; i++) {
        if (!g_chan[i].active) continue;
        if (step <= 0.0f) {
            g_chan[i].paused = pause ? 1 : 0;
            g_chan[i].fadeAction = MP6_FADE_NONE;
            g_chan[i].fadeStep = 0.0f;
            g_chan[i].fadeMul = pause ? 0.0f : 1.0f;
        } else {
            g_chan[i].paused = 0;
            g_chan[i].fadeAction = pause ? MP6_FADE_TO_PAUSE : MP6_FADE_TO_PLAY;
            g_chan[i].fadeStep = pause ? -step : step;
        }
    }
    mp6_unlock();
    return 0;
}

s32 msmStreamSetParam(int streamNo, MSM_STREAMPARAM *param)
{
    printf("[AUDIO] msmStreamSetParam(streamNo=%d, flag=%#x%s)\n", streamNo,
           param ? (unsigned)param->flag : 0u, param ? "" : " (param=NULL)");
    if (streamNo < 0 || streamNo >= g_chanMax) return MP6_MSM_ERR_RANGE_STREAM;
    if (!param) return 0;
    mp6_lock();
    if (param->flag & MSM_STREAMPARAM_VOL) g_chan[streamNo].vol = param->vol;
    /* pan/span/auxA/auxB/fadeSpeed/chan: intentionally not applied to the
     * mix -- every real stream in this file plays through the ALWAYS-2-
     * channel stereo start path with HARD-CODED pan (0=full-left,
     * 127=full-right; see this file's header comment), so a pan change
     * here is a no-op in the real engine too for every track this game
     * actually uses (msmStreamSetParam's own real pan branch is gated on
     * `slot->slotL == -1`, never true for this game's actual assets).
     * span/aux (surround width + effect sends) are out of scope (dry
     * stereo only). */
    mp6_unlock();
    return 0;
}

s32 msmStreamGetStatus(int streamNo)
{
    s32 status;
    if (streamNo < 0 || streamNo >= g_chanMax) return MP6_MSM_ERR_RANGE_STREAM;
    /* Report the real transitional states while a fade is actually in
     * flight -- MSM_STREAM_PAUSEIN
     * ("fading into pause", also reused for "fading out to a full stop",
     * the closest fit since there is no distinct STOPPING constant) and
     * MSM_STREAM_PAUSEOUT ("fading out of pause, back to full volume").
     * Falls back to the exact previous done/paused/play logic once any
     * fade completes or when speed<=0 kept a transition immediate. */
    mp6_lock();
    if (!g_chan[streamNo].active) status = MSM_STREAM_DONE;
    else if (g_chan[streamNo].fadeAction == MP6_FADE_TO_PAUSE ||
             g_chan[streamNo].fadeAction == MP6_FADE_TO_STOP) status = MSM_STREAM_PAUSEIN;
    else if (g_chan[streamNo].fadeAction == MP6_FADE_TO_PLAY) status = MSM_STREAM_PAUSEOUT;
    else if (g_chan[streamNo].paused) status = MSM_STREAM_PAUSEIN;
    else status = MSM_STREAM_PLAY;
    mp6_unlock();
    return status;
}

void msmStreamSetMasterVolume(s32 arg0)
{
    printf("[AUDIO] msmStreamSetMasterVolume(%d)\n", (int)arg0);
    mp6_lock();
    g_masterVol = (int)(arg0 & 127);
    mp6_unlock();
}

/* =======================================================================
 * msmSePlay / Stop / StopAll / GetStatus / PauseAll / SetParam /
 * SetMasterVolume -- the game-facing SFX control API (game/audio.c's
 * HuSePlay/HuAudFXPlay* wrappers -- see this file's own big SFX header
 * comment above msm_se_bank_init for the full bank-format writeup).
 * ======================================================================= */
/* ---- KEY GROUPS (mcmdSetKeyGroup, synthmacros.c opcode 0x59) -----------
 *
 * THE ONE-AT-A-TIME RULE THIS PORT WAS MISSING.
 *
 * When the real engine starts an fx whose macro carries SetKeyGroup <kg>,
 * mcmdSetKeyGroup first walks EVERY synth voice and, for each one already in
 * key group kg, calls voiceKill() (the step's kill byte set) or
 * macSetExternalKeyoff() (kill byte clear). Only then does the new voice
 * adopt the group. That is what keeps a rapidly retriggered SE to a single
 * sounding voice on real hardware.
 *
 * This port had no equivalent, so every retrigger STACKED. Measured on the
 * retail Party-Mode flow: the coin sound (seId 7, key group 71, kill=1) was
 * started 8 times per tick and held 16 simultaneous voices, after which
 * msmSePlay reported "all 16 SFX voice slots busy, dropped" 65 times. Sixteen
 * overlapping copies of a 0.54 s one-shot, retriggered every ~4 ticks, is
 * continuous sound -- a one-shot that "loops".
 *
 * BOTH kill MODES STOP THE VOICE HERE, and that is faithful rather than
 * convenient: the keyoff branch ends the voice through the ADSR release, and
 * the release time comes from hwInitSamplePlayback's DEFAULT envelope
 * (rTime = 0 -- hardware.c) unless the macro set its own with SetADSR /
 * SetADSRFromCtrl / ScaleVolumeDLS. The disc census (MP6_AUDIO_SE_CENSUS)
 * reports customADSR=0 across ALL 833 key-grouped SE defs, so no key-grouped
 * macro on this disc has a release stage at all. Should that ever stop being
 * true, this is the line to revisit.
 *
 * A stop with speed 0 (the immediate path) is used rather than a fade,
 * matching voiceKill; the mixer's own per-sample volume handling is what
 * removes the click, exactly as it does for msmSeStop(seNo, 0).
 *
 * Callers must hold g_mixerLock. Returns the number of voices released. */
static int mp6_se_keygroup_disabled(void)
{
    /* A/B switch, same shape and purpose as MP6_AUDIO_NO_MACRO_LIFE: restores
     * the pre-fix stacking behavior in the SAME binary so "before" and "after"
     * are one build and one method. */
    static int s_disabled = -1;
    if (s_disabled < 0) {
        const char *e = getenv("MP6_AUDIO_NO_KEYGROUP");
        s_disabled = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return s_disabled;
}

/* One released voice, recorded under the lock and printed after it drops --
 * same snapshot-then-print discipline mp6_se_timeline_census uses, because
 * stdio inside g_mixerLock stalls the SDL audio callback, and a coin tally
 * can drive this path 8 times a tick. */
typedef struct {
    int slot, seNo, seId, kill;
    uint64_t age;
    uint32_t wraps;
} MsmKgRelRec;

static int mp6_se_keygroup_release(int keyGroup, int kill, MsmKgRelRec *recs, MsmPcmRetirement *retired)
{
    int i, released = 0;
    if (keyGroup == 0 || mp6_se_keygroup_disabled()) return 0;
    /* A savestate restore replays every captured voice through msmSePlay in
     * turn. Those voices coexisted in the captured state, so re-applying the
     * rule during replay would have each restored voice kill the previously
     * restored members of its own group. The captured set is authoritative. */
    if (g_savestateMixerMuted) return 0;
    /* Bounded by the ACTIVE cap, which is also what sizes the caller's recs[]
     * (kgRel[] in msmSePlay is the capacity, so it can never overflow) --
     * releasing is a sweep over the same slots the allocator can hand out. */
    for (i = 0; i < mp6_sfx_voice_cap(); i++) {
        MsmSeVoice *v = &g_sfxVoice[i];
        if (!v->active || v->keyGroup != keyGroup) continue;
        if (recs) {
            recs[released].slot = i;
            recs[released].seNo = v->no;
            recs[released].seId = v->seId;
            recs[released].kill = kill;
            recs[released].age = mp6_tick_count - v->tlStartTick;
            recs[released].wraps = v->tlWraps;
        }
        v->active = 0;
        mp6_pcm_detach(retired, &v->pcm);
        v->fadeAction = MP6_FADE_NONE;
        v->fadeStep = 0.0f;
        v->keyGroup = 0;
        released++;
    }
    if (released > 0) {
        g_kgReleaseEvents++;
        g_kgVoicesReleased += (unsigned long)released;
    }
    return released;
}

/* Lock NOT held. */
static void mp6_se_keygroup_report(const MsmKgRelRec *recs, int n, int keyGroup, int bySeId)
{
    int i;
    if (n <= 0 || recs == NULL || !mp6_se_timeline_on()) return;
    for (i = 0; i < n; i++) {
        printf("[SETL] kgrel tick=%llu slot=%d seNo=%d seId=%d kg=%d %s by seId=%d "
               "age=%llu ticks wraps=%u\n",
               (unsigned long long)mp6_tick_count, recs[i].slot, recs[i].seNo,
               recs[i].seId, keyGroup, recs[i].kill ? "KILLED" : "KEYOFF", bySeId,
               (unsigned long long)recs[i].age, (unsigned)recs[i].wraps);
    }
    fflush(stdout);
}

MsmSeVoice *find_sfx_voice_by_no(int seNo)
{
    int i;
    for (i = 0; i < mp6_sfx_voice_cap(); i++) {
        if (g_sfxVoice[i].active && g_sfxVoice[i].no == seNo) return &g_sfxVoice[i];
    }
    return NULL;
}

int msmSePlay(int seId, MSM_SEPARAM *param)
{
    MsmPcmRetirement retired = {0};
    MsmSeDef *def;
    int slot, i, vol, pan, ownerGrpIdx;
    int fxIdx = -1;   /* the fx's own index in its group's fx[] -- the volume
                       * envelope side table's key, see MsmFxEnvRow */
    MsmSampleInfo info;
    uint32_t adpcmFrames, byteLen;
    uint32_t sampPoolFileOfs, sampPoolSize;
    uint32_t loopEndFrame = 0;
    uint32_t fxLifeMs = 0;
    Mp6MsmSeEnv fxEnv;
    Mp6MsmEnvPlan envPlan;
    int fxKeyGroup = 0, fxKeyGroupKill = 0, keyGroupReleased = 0;
    MsmKgRelRec kgRel[MP6_MSM_MAX_SFX_VOICES];
    int loopMode;
    uint16_t sampleId;
    uint8_t initialPS;
    uint8_t *raw;
    int16_t *mono;
    MP6AdpcmCoefTable coef;
    MP6AdpcmState state;
    int psCheckOk;
    int seNo;
    uint32_t totalFramesForLog;
    int firstWav = 0;

    /* Published as a game event (include/mp6_events.h) at the very top,
     * before any of the sample-resolution work below can reject the request:
     * what a driver needs from this seam is "the game DECIDED to play SE
     * <id>", which is the observable acknowledgement that a menu accepted
     * an input -- every MP6 menu answers a cursor move or a confirm with
     * its own SE. That is true whether or not this port can then find a
     * decodable sample for it, so the event must not be conditional on the
     * mixer succeeding. One line per SE start; menus fire these only on
     * real input, so this is not a per-frame cost.
     *
     * The id is published in the STRING field as well as the numeric one.
     * mp6_event_matches() already accepted either spelling, so every existing
     * "pressuntil:a/se.play/<id>" style wait is unaffected -- but the
     * MP6_FRAME_DUMP_TRIGGER matcher (include/mp6_frame_dump.h) only
     * ever sees the "<key>=<sval>" text, so with a NULL sval every SE in the
     * game collapsed to the single indistinguishable string "se.play=-" and
     * no frame burst could be armed on a PARTICULAR sound. Board animations
     * announce themselves through their SE and nothing else (board/dice.c's
     * dice-block spin is mbAudFXPlay(0x3ED) = 1005 and has no other
     * observable marker), so this is the difference between being able to
     * capture the frames of a named animation and not. */
    {
        char seIdText[16];
        snprintf(seIdText, sizeof(seIdText), "%d", seId);
        mp6_event_post("se.play", (long)seId, seIdText);
    }

    memset(&fxEnv, 0, sizeof(fxEnv));
    memset(&envPlan, 0, sizeof(envPlan));

    if (param) {
        printf("[AUDIO] msmSePlay(seId=%d, flag=%#x vol=%d pan=%d)\n", seId,
               (unsigned)param->flag, param->vol, param->pan);
    } else {
        printf("[AUDIO] msmSePlay(seId=%d, param=NULL)\n", seId);
    }

    if (!g_msmReady) {
        printf("[AUDIO] msmSePlay: no .msm bank loaded on this checkout -- returning MSM_ERR_OPENFAIL\n");
        return MSM_ERR_OPENFAIL;
    }
    if (seId < 0 || seId >= g_seDefCount) return MSM_ERR_INVALIDID;
    def = &g_seDefs[seId];
    if (def->gid == 0xFFFF) return MSM_ERR_REMOVEDID; /* the real msmSePlay's own removed-id
                                                          marker (msmse.c) -- none authored on
                                                          this disc, guarded anyway */

    /* GID-SCOPED resolution against the currently-loaded group table. The
     * SE def itself names the group (MSM_SE.gid); since groups load/unload
     * per scene, an unloaded gid is the -122 (MSM_ERR_REMOVEDID) silent
     * skip -- same error the real engine reports for an id whose group
     * isn't resident.
     *
     * LOCKING: everything read out of the group's blob (fx index, SDIR
     * entry, ADPCM coef table) is COPIED to locals under g_grpLock, then
     * the lock drops BEFORE the sample-pool file read + decode below -- a
     * concurrent msmSysDelGroupAll (the opt-in stress threads; the game's
     * own calls are same-thread) can free the blob the instant we unlock,
     * and must not stall behind this call's file I/O either. */
    {
        MsmSeGroup *grp;
        const MsmFxEntry *fx = NULL;

        mp6_grp_lock();
        grp = msm_se_group_find_by_gid(def->gid);
        if (!grp) {
            char loaded[256];
            msm_se_group_loaded_gids_str(loaded, sizeof(loaded));
            mp6_grp_unlock();
            printf("[AUDIO] msmSePlay: seId=%d gid=%u fxId=%d not resolved -- its GROUP is not loaded "
                   "right now (loaded gids: %s; *=base). Scenes load groups via HuAudSndGrpSetSet/"
                   "msmSysLoadGroup -- silently skipped (MSM_ERR_REMOVEDID)\n",
                   seId, (unsigned)def->gid, def->fxId, loaded);
            return MSM_ERR_REMOVEDID;
        }
        if (grp->parseError) {
            mp6_grp_unlock();
            return MSM_ERR_INVALIDFILE;
        }
        for (i = 0; i < grp->fxCount; i++) {
            if (grp->fx[i].fxId == def->fxId) { fx = &grp->fx[i]; fxIdx = i; break; }
        }
        if (!fx) {
            int grpIdx = grp->grpIdx, fxCount = grp->fxCount;
            mp6_grp_unlock();
            fprintf(stderr, "[AUDIO] msmSePlay: seId=%d fxId=%d missing from its OWN loaded group "
                    "gid=%u (grpIdx=%d, %d fx entries) -- macro without a StartSample opcode, or a "
                    "format surprise -- skipped\n", seId, def->fxId, (unsigned)def->gid, grpIdx, fxCount);
            return MSM_ERR_REMOVEDID;
        }
        sampleId = fx->sampleId;
        fxLifeMs = fx->lifeMs; /* copied out with everything else -- see MsmFxEntry */
        {
            /* The compiled volume program, if this fx has one at all: copied BY
             * VALUE like everything else here, because the group's blob AND its
             * envelope rows may be freed the instant the lock drops while the
             * voice keeps sounding. No row means segCount 0 (fxEnv was zeroed
             * above), which is the flat-gain state every refusal lands on. */
            const Mp6MsmSeEnv *env = msm_se_group_fx_env(grp, fxIdx);
            if (env != NULL) fxEnv = *env;
        }
        fxKeyGroup = fx->keyGroup;
        fxKeyGroupKill = fx->keyGroupKill;
        if (!msm_lookup_sdir(grp, sampleId, &info)) {
            int malformed = grp->parseError;
            mp6_grp_unlock();
            if (malformed) {
                fprintf(stderr, "[AUDIO] msmSePlay: rejecting malformed SDIR for gid=%u\n",
                        (unsigned)def->gid);
                return MSM_ERR_INVALIDFILE;
            }
            fprintf(stderr, "[AUDIO] msmSePlay: seId=%d fxId=%d sampleId=%d not found in its own "
                    "group's SDIR table\n", seId, def->fxId, (int)sampleId);
            return MSM_ERR_PLAYFAIL;
        }
        if (info.compType != 0) {
            mp6_grp_unlock();
            fprintf(stderr, "[AUDIO] msmSePlay: seId=%d sample compType=%d not supported "
                    "(only ADPCM==0 handled -- see this file's own scope note) -- skipped\n",
                    seId, (int)info.compType);
            return MSM_ERR_PLAYFAIL;
        }

        /* Coefficient table lives inline in the group's own metadata blob at
         * sdirOfs+coefTableRelOfs -- see this file's own SFX header
         * comment. NOT at offset 0: an 8-byte sub-header precedes the real
         * 8-pair table (numCoef(u16, always 8) initialPS(u8) loopPS(u8)
         * loopY0(s16) loopY1(s16) -- recovered from the real, un-decompiled
         * PowerPC asm for salBuildCommandList, build/GP6E01/asm/musyx/
         * runtime/hw_dspctrl.s, since no decompiled C anywhere states this
         * pool-sample-specific layout; byte-identical to the already-
         * decompiled SNDADPCMinfo in include/musyx/stream.h, just never
         * connected to this call path in C). `initialPS` exactly matches
         * the real sample data's own frame-0 header byte for every sample
         * checked -- getting this offset wrong (reading coefficients
         * starting at +0 instead of +8) silently misinterprets
         * numCoef/initialPS/loopPS/loopY0/loopY1 as the first 2
         * "coefficient pairs", producing an unstable, noisy decode. Copied
         * OUT here because the blob must not be touched after g_grpLock
         * drops. */
        {
            uint64_t coefOff = (uint64_t)grp->sdirOfs + info.coefTableRelOfs;
            const uint8_t *coefSub;
            if (!msm_group_span(grp, coefOff, 40, "ADPCM coefficient block") ||
                be16(grp->blob + (size_t)coefOff) != 8u) {
                grp->parseError = 1;
                mp6_grp_unlock();
                return MSM_ERR_INVALIDFILE;
            }
            coefSub = grp->blob + (size_t)coefOff;
            const uint8_t *coefBytes = coefSub + 8;
            int p;
            for (p = 0; p < 8; p++) {
                coef.coef[p][0] = (int16_t)be16(coefBytes + (size_t)p * 4 + 0);
                coef.coef[p][1] = (int16_t)be16(coefBytes + (size_t)p * 4 + 2);
            }
            initialPS = coefSub[2];
        }
        sampPoolFileOfs = grp->sampPoolFileOfs;
        sampPoolSize = grp->sampPoolSize;
        /* Retain the exact owner even though the decoded PCM becomes private:
         * a savestate can replay a live voice only if this group is still in
         * its captured resident set. */
        ownerGrpIdx = grp->grpIdx;
        mp6_grp_unlock();
    }

    /* info.length is a PCM SAMPLE count, NOT a byte count (see
     * MsmSampleInfo.length's own comment for the full ground truth). One
     * 8-byte ADPCM frame decodes to 14 samples, so the raw byte length is
     * ceil(length/14)*8. Reading it as a byte count instead decodes ~75%
     * foreign bytes (the NEXT samples in the pool, or for a pool-final
     * sample the next group's data) with THIS sample's coefficient table:
     * a loud, often full-scale noise burst appended to every SFX's tail. */
    adpcmFrames = (info.length + 13) / 14;
    if (adpcmFrames == 0 || adpcmFrames > (UINT32_MAX / 14)) {
        printf("[AUDIO] msmSePlay: seId=%d has zero-length or implausible audio -- nothing to play\n", seId);
        return MSM_ERR_INVALIDFILE;
    }
    if (!mp6_msm_decode_budget_valid((uint64_t)adpcmFrames * 14u, 4u)) {
        fprintf(stderr,
                "[AUDIO] msmSePlay: seId=%d decode budget rejected %u ADPCM frames\n",
                seId, (unsigned)adpcmFrames);
        return MSM_ERR_INVALIDFILE;
    }
    byteLen = adpcmFrames * 8;
    if (!mp6_msm_span_valid_u32(sampPoolSize, info.offset, byteLen) ||
        (uint64_t)sampPoolFileOfs + info.offset > UINT32_MAX ||
        !mp6_msm_span_valid_u32(g_msmFileSize,
                                (uint32_t)((uint64_t)sampPoolFileOfs + info.offset), byteLen)) {
        fprintf(stderr, "[AUDIO] msmSePlay: seId=%d sample span escapes its group pool "
                "(offset=%u bytes=%u pool=%u)\n", seId, (unsigned)info.offset,
                (unsigned)byteLen, (unsigned)sampPoolSize);
        return MSM_ERR_INVALIDFILE;
    }
    loopMode = mp6_msm_loop_bounds(info.length, info.loopStart, info.loopLength,
                                   &loopEndFrame);
    if (loopMode < 0) {
        fprintf(stderr, "[AUDIO] msmSePlay: seId=%d has invalid loop metadata "
                "(total=%u start=%u length=%u)\n", seId, (unsigned)info.length,
                (unsigned)info.loopStart, (unsigned)info.loopLength);
        return MSM_ERR_INVALIDFILE;
    }

    /* A long sample is normally decoded ahead already (msm_se_decode_ahead):
     * copy it. Otherwise decode it now. */
    {
        uint8_t firstByte = 0;
        mono = msm_copy_decoded(def->gid, ownerGrpIdx, info.sdirIndex, adpcmFrames, &firstByte);
        if (mono != NULL) {
            psCheckOk = (initialPS == firstByte); /* the same self-check as below */
        } else {
            raw = (uint8_t *)malloc(byteLen);
            if (!raw) return MSM_ERR_OUTOFMEM;
            if (!msm_read_sample(def->gid, ownerGrpIdx, sampPoolFileOfs, info.offset, byteLen, raw)) {
                free(raw);
                return MSM_ERR_READFAIL;
            }

            if ((uint64_t)adpcmFrames * 14u * sizeof(int16_t) > SIZE_MAX) {
                free(raw);
                return MSM_ERR_OUTOFMEM;
            }
            mono = (int16_t *)malloc((size_t)adpcmFrames * 14u * sizeof(int16_t));
            if (!mono) { free(raw); return MSM_ERR_OUTOFMEM; }

            /* Self-check -- see the log-site comment below. initialPS (copied
             * out of the coef sub-header under g_grpLock above) is the authored
             * copy of frame 0's PS byte; raw[0] is the real frame-0 PS byte just
             * read from the sample pool. */
            psCheckOk = (initialPS == raw[0]);
            state.hist1 = 0;
            state.hist2 = 0;
            mp6_dspadpcm_decode(raw, adpcmFrames, &coef, &state, mono);
            free(raw);
        }
    }

    /* AUTHORED NOTE LENGTH -- the macro clock this port does not otherwise
     * have. See MsmFxEntry.lifeMs for the ground truth (mcmdEndOfMacro ->
     * voiceFree). Only a loop-flagged sample can outlive its own data, so
     * only that case needs the cap; a one-shot already ends at info.length.
     *
     * Rather than adding a per-voice countdown (which a savestate would have
     * to capture to restore exactly), the loop is FLATTENED here: the decoded
     * buffer is rebuilt as the head plus however many whole/partial loop
     * repeats fit inside the authored note, and the voice is then an ordinary
     * ONE-SHOT of exactly that length. The mixer, the savestate shadow, and
     * msmSeStop all keep working unchanged, and the restore path -- which
     * replays through this very function -- reproduces the identical buffer. */
    {
        uint32_t noteFrames = mp6_se_authored_note_frames(&info, fxLifeMs, loopMode,
                                                          loopEndFrame);
        if (noteFrames != 0) {
            uint32_t decodedCap = adpcmFrames * 14u;
            if (noteFrames <= decodedCap) {
                /* The note ends inside the already-decoded data. */
                info.length = noteFrames;
                loopMode = 0;
                loopEndFrame = 0;
            } else if ((uint64_t)noteFrames * sizeof(int16_t) <= SIZE_MAX) {
                int16_t *flat = (int16_t *)malloc((size_t)noteFrames * sizeof(int16_t));
                if (flat) {
                    uint32_t loopSpan = loopEndFrame - info.loopStart;
                    uint32_t head = loopEndFrame < noteFrames ? loopEndFrame : noteFrames;
                    uint32_t pos;
                    memcpy(flat, mono, (size_t)head * sizeof(int16_t));
                    for (pos = head; pos < noteFrames; ) {
                        uint32_t chunk = noteFrames - pos;
                        if (chunk > loopSpan) chunk = loopSpan;
                        memcpy(flat + pos, mono + info.loopStart,
                               (size_t)chunk * sizeof(int16_t));
                        pos += chunk;
                    }
                    free(mono);
                    mono = flat;
                    info.length = noteFrames;
                    loopMode = 0;   /* now a plain one-shot of the authored length */
                    loopEndFrame = 0;
                }
                /* malloc failure: keep the un-flattened looping buffer. The
                 * savestate position validator assumes the flattened bound,
                 * so a capture taken in that (allocation-failure) window
                 * fails closed rather than restoring a wrong position. */
            }
        }
    }

    /* AFTER the flattening above, because the loop gate that decides whether
     * an envelope may be installed at all reads the FINAL loopMode -- see
     * mp6_se_install_env's own comment. */
    mp6_se_install_env(&fxEnv, &info, loopMode, &envPlan);

    vol = (param && (param->flag & MSM_SEPARAM_VOL)) ? param->vol : MSM_VOL_MAX;
    pan = (param && (param->flag & MSM_SEPARAM_PAN)) ? param->pan : def->pan;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;

    mp6_lock();
    /* mcmdSetKeyGroup runs BEFORE the new voice takes a slot on real hardware
     * (the macro's SetKeyGroup step precedes its StartSample), so the slot
     * search below sees the freed slots -- which is exactly why the real
     * engine never runs out of voices on a rapid retrigger. */
    keyGroupReleased = mp6_se_keygroup_release(fxKeyGroup, fxKeyGroupKill, kgRel, &retired);
    /* First-free over the ACTIVE slots, through the one shared rule in
     * msm_safe.h (mp6_msm_voice_first_free) so tools/sfx_voices_selftest.c
     * exercises the same function the runtime allocates with. At cap 16 the
     * scan cannot reach -- and never reads -- a slot >= 16, so the sequence
     * of slots a given run hands out is bit-for-bit the pre-enhancement one;
     * the 17th simultaneous start is refused exactly as it always was. At
     * cap 32 the 17th succeeds and the 33rd is refused. */
    {
        unsigned char busy[MP6_MSM_MAX_SFX_VOICES];
        int cap = mp6_sfx_voice_cap();
        for (i = 0; i < cap; i++) busy[i] = (unsigned char)(g_sfxVoice[i].active != 0);
        slot = mp6_msm_voice_first_free(busy, cap);
    }
    if (slot < 0) {
        mp6_unlock();
        mp6_pcm_release(&retired);
        mp6_se_keygroup_report(kgRel, keyGroupReleased, fxKeyGroup, seId);
        free(mono);
        printf("[AUDIO] msmSePlay: seId=%d -- all %d SFX voice slots busy, dropped\n",
               seId, mp6_sfx_voice_cap());
        return MP6_MSM_ERR_CHANLIMIT;
    }
    if (g_seNoCounter >= INT_MAX) {
        mp6_unlock();
        mp6_pcm_release(&retired);
        mp6_se_keygroup_report(kgRel, keyGroupReleased, fxKeyGroup, seId);
        free(mono);
        fprintf(stderr, "[AUDIO] msmSePlay: SE handle space exhausted -- dropped safely\n");
        return MP6_MSM_ERR_CHANLIMIT;
    }

    mp6_pcm_detach(&retired, &g_sfxVoice[slot].pcm);
    g_sfxVoice[slot].active = 1;
    g_sfxVoice[slot].paused = 0;
    g_sfxVoice[slot].loop = loopMode > 0;
    g_sfxVoice[slot].pcm = mono;
    /* Play EXACTLY the authored sample count, not the whole-frame roundup
     * (`adpcmFrames * 14` -- up to 13 extra decoded padding nibbles from
     * the final partial frame; the mono[] buffer still holds them, they
     * just never play). */
    g_sfxVoice[slot].totalFrames = info.length;
    g_sfxVoice[slot].loopStartFrame = info.loopStart;
    g_sfxVoice[slot].loopEndFrame = loopEndFrame;
    g_sfxVoice[slot].posFrac = 0;
    {
        uint32_t nativeFrq = info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE;
        g_sfxVoice[slot].stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    }
    g_sfxVoice[slot].baseVol = def->vol;
    g_sfxVoice[slot].vol = vol;
    g_sfxVoice[slot].gainL = (127 - pan) / 127.0f;
    g_sfxVoice[slot].gainR = pan / 127.0f;
    g_sfxVoice[slot].pan = pan;
    g_sfxVoice[slot].seId = seId;
    g_sfxVoice[slot].grpIdx = ownerGrpIdx;
    g_sfxVoice[slot].no = g_seNoCounter++;
    g_sfxVoice[slot].gid = def->gid; /* msmSeStopAll(checkGrp) needs it, see MsmSeVoice */
    /* Adopted only after the release pass above, mirroring mcmdSetKeyGroup's
     * own order (clear svoice->keyGroup, walk the other voices, then assign)
     * -- so a voice can never key-group-kill itself. */
    g_sfxVoice[slot].keyGroup = mp6_se_keygroup_disabled() ? 0 : fxKeyGroup;
    /* See msmStreamPlay's own identical comment on why this reset
     * belongs here, not left to the mixer. */
    g_sfxVoice[slot].fadeAction = MP6_FADE_NONE;
    g_sfxVoice[slot].fadeStep = 0.0f;
    g_sfxVoice[slot].fadeMul = 1.0f;
    g_sfxVoice[slot].envPlan = envPlan; /* neutral (segCount 0) unless the macro
                                         * authored a program this port can run */
    g_sfxVoice[slot].tlStartTick = mp6_tick_count;
    g_sfxVoice[slot].tlWraps = 0;
    g_sfxVoice[slot].tlLastCensusWraps = 0;
    g_sfxVoice[slot].tlReported = 0;
    seNo = g_sfxVoice[slot].no;
    totalFramesForLog = g_sfxVoice[slot].totalFrames;
    if (!g_wavArmed) {
        g_wavArmed = 1;
        firstWav = 1;
    }
    mp6_unlock();

    mp6_pcm_release(&retired);
    mp6_se_keygroup_report(kgRel, keyGroupReleased, fxKeyGroup, seId);

    if (firstWav) {
        printf("[AUDIO] msmSePlay: first real SFX playback -- MP6_AUDIO_WAV_DUMP capture window (if "
               "enabled) starts now\n");
    }

    /* initialPS (the coef sub-header's own copy of frame 0's PS byte) vs
     * the actual first raw byte read -- a cheap, always-on decode-path
     * self-check. A mismatch would mean the sample OFFSET or the
     * coef-table offset is being misread -- loud log, playback still
     * proceeds. */
    printf("[AUDIO] msmSePlay: seId=%d gid=%u fxId=%d -> voice slot=%d sampleId=%d frq=%uHz authoredVol=%d "
           "pan=%d duration=%.2fs (%u samples, %u adpcm frames, loopStart=%u loopLen=%u%s)\n",
           seId, (unsigned)def->gid, def->fxId, slot, (int)sampleId, (unsigned)info.sampleRateHz,
           (int)def->vol, pan, (double)totalFramesForLog /
           (info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE),
           (unsigned)totalFramesForLog, (unsigned)adpcmFrames,
           (unsigned)info.loopStart, (unsigned)info.loopLength,
           psCheckOk ? "" : " -- WARNING initialPS!=frame0-PS, offsets suspect");

    /* One line per start whose macro authored a volume program, so a bounded
     * run's log alone shows whether the envelope actually engaged on the SE
     * under investigation -- and, when the fx compiled one but the loop gate
     * refused it, says so instead of staying silent. */
    if (fxEnv.segCount != 0) {
        char progBuf[256];
        mp6_env_program_str(&fxEnv, progBuf, sizeof(progBuf));
        printf("[AUDIO] msmSePlay: seId=%d authored volume envelope %s -- segs=%d applied=%d%s\n",
               seId, progBuf, (int)fxEnv.segCount, envPlan.segCount != 0,
               envPlan.segCount != 0 ? ""
                   : (mp6_se_macro_env_disabled() ? " (MP6_AUDIO_NO_MACRO_ENV)"
                                                  : " (voice still loops -- see mp6_se_install_env)"));
    }

    if (mp6_se_timeline_on()) {
        printf("[SETL] play tick=%llu seNo=%d seId=%d gid=%u fxId=%d slot=%d samp=%d "
               "dur=%.3fs loop=%d loopFrames=%u..%u macroLifeMs=%u\n",
               (unsigned long long)mp6_tick_count, seNo, seId, (unsigned)def->gid,
               def->fxId, slot, (int)sampleId,
               (double)totalFramesForLog / (info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE),
               loopMode > 0, (unsigned)info.loopStart, (unsigned)loopEndFrame,
               (unsigned)fxLifeMs);
        printf("[SETL] kgset tick=%llu seNo=%d seId=%d kg=%d kill=%d released=%d\n",
               (unsigned long long)mp6_tick_count, seNo, seId, fxKeyGroup,
               fxKeyGroupKill, keyGroupReleased);
        fflush(stdout);
    }

    return seNo;
}

s32 msmSeStop(int seNo, s32 speed)
{
    MsmPcmRetirement retired = {0};
    MsmSeVoice *v;
    float step;
    int found, stoppedId = 0;
    uint64_t age = 0;
    uint32_t wraps = 0;
    const int timeline = mp6_se_timeline_on();
    printf("[AUDIO] msmSeStop(seNo=%d, speed=%d)\n", seNo, (int)speed);
    mp6_lock();
    v = find_sfx_voice_by_no(seNo);
    found = v != NULL;
    if (v) {
        stoppedId = v->seId;
        age = mp6_tick_count - v->tlStartTick;
        wraps = v->tlWraps;
        step = mp6_fade_step_from_speed(speed);
        if (step <= 0.0f || !v->pcm) {
            v->active = 0;
            mp6_pcm_detach(&retired, &v->pcm);
            v->fadeAction = MP6_FADE_NONE;
            v->fadeStep = 0.0f;
        } else {
            v->paused = 0;
            v->fadeAction = MP6_FADE_TO_STOP;
            v->fadeStep = -step;
        }
    }
    mp6_unlock();
    mp6_pcm_release(&retired);
    if (timeline) {
        if (found) {
            printf("[SETL] stop tick=%llu seNo=%d seId=%d HIT age=%llu ticks wraps=%u speed=%d\n",
                   (unsigned long long)mp6_tick_count, seNo, stoppedId,
                   (unsigned long long)age, (unsigned)wraps, (int)speed);
        } else {
            printf("[SETL] stop tick=%llu seNo=%d MISS (handle already gone)\n",
                   (unsigned long long)mp6_tick_count, seNo);
        }
        fflush(stdout);
    }
    return found ? 0 : MP6_MSM_ERR_INVALIDSE;
}

void msmSeStopAll(BOOL checkGrp, s32 speed)
{
    MsmPcmRetirement retired = {0};
    int i, j;
    float step;
    uint16_t baseGids[MP6_MSM_MAX_SE_GROUPS];
    int baseGidCount = 0;
    printf("[AUDIO] msmSeStopAll(checkGrp=%d, speed=%d)\n", (int)checkGrp, (int)speed);
    /* checkGrp: real semantics (msmse.c's msmSysCheckBaseGroup gate):
     * checkGrp=TRUE stops only voices whose group is NOT a resident base
     * group -- exactly what HuAudSndGrpSetSet calls right before a
     * scene's group swap, so the about-to-unload groups' voices stop while
     * common/base UI sounds keep ringing across the transition. The base-gid
     * snapshot is taken under g_grpLock FIRST, never nested with the mixer
     * lock. */
    if (checkGrp) {
        mp6_grp_lock();
        for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
            if (g_seGroups[i].inUse && g_seGroups[i].baseGrpF) {
                baseGids[baseGidCount++] = g_seGroups[i].gid;
            }
        }
        mp6_grp_unlock();
    }
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < mp6_sfx_voice_cap(); i++) {
        if (checkGrp) {
            int isBase = 0;
            for (j = 0; j < baseGidCount; j++) {
                if (g_sfxVoice[i].gid == baseGids[j]) { isBase = 1; break; }
            }
            if (isBase) continue; /* base-group voice -- keeps playing, real-engine behavior */
        }
        if (step <= 0.0f || !g_sfxVoice[i].pcm) {
            g_sfxVoice[i].active = 0;
            mp6_pcm_detach(&retired, &g_sfxVoice[i].pcm);
            g_sfxVoice[i].fadeAction = MP6_FADE_NONE;
            g_sfxVoice[i].fadeStep = 0.0f;
        } else {
            g_sfxVoice[i].paused = 0;
            g_sfxVoice[i].fadeAction = MP6_FADE_TO_STOP;
            g_sfxVoice[i].fadeStep = -step;
        }
    }
    mp6_unlock();
    mp6_pcm_release(&retired);
}

s32 msmSeGetStatus(int seNo)
{
    MsmSeVoice *v;
    s32 status;
    mp6_lock();
    v = find_sfx_voice_by_no(seNo);
    /* Same "report the real transition while it's in flight" treatment
     * as msmStreamGetStatus above -- MSM_SE_PAUSEIN/PAUSEOUT are this
     * format's own equivalents. */
    if (!v) status = MSM_SE_DONE;
    else if (v->fadeAction == MP6_FADE_TO_PAUSE || v->fadeAction == MP6_FADE_TO_STOP) status = MSM_SE_PAUSEIN;
    else if (v->fadeAction == MP6_FADE_TO_PLAY) status = MSM_SE_PAUSEOUT;
    else status = v->paused ? MSM_SE_PAUSEIN : MSM_SE_PLAY;
    mp6_unlock();
    return status;
}

s32 msmSePause(int seNo, BOOL pause, s32 speed)
{
    MsmSeVoice *v;
    float step;

    printf("[AUDIO] msmSePause(seNo=%d, pause=%d, speed=%d)\n",
           seNo, (int)pause, (int)speed);
    mp6_lock();
    v = find_sfx_voice_by_no(seNo);
    if (!v) {
        mp6_unlock();
        return MP6_MSM_ERR_INVALIDSE;
    }

    step = mp6_fade_step_from_speed(speed);
    if (step <= 0.0f) {
        v->paused = pause ? 1 : 0;
        v->fadeAction = MP6_FADE_NONE;
        v->fadeStep = 0.0f;
        v->fadeMul = pause ? 0.0f : 1.0f;
    } else {
        v->paused = 0;
        v->fadeAction = pause ? MP6_FADE_TO_PAUSE : MP6_FADE_TO_PLAY;
        v->fadeStep = pause ? -step : step;
    }
    mp6_unlock();
    return 0;
}

s32 msmSePauseAll(BOOL pause, s32 speed)
{
    int i;
    float step;
    printf("[AUDIO] msmSePauseAll(pause=%d, speed=%d)\n", (int)pause, (int)speed);
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < mp6_sfx_voice_cap(); i++) {
        if (!g_sfxVoice[i].active) continue;
        if (step <= 0.0f) {
            g_sfxVoice[i].paused = pause ? 1 : 0;
            g_sfxVoice[i].fadeAction = MP6_FADE_NONE;
            g_sfxVoice[i].fadeStep = 0.0f;
            g_sfxVoice[i].fadeMul = pause ? 0.0f : 1.0f;
        } else {
            g_sfxVoice[i].paused = 0;
            g_sfxVoice[i].fadeAction = pause ? MP6_FADE_TO_PAUSE : MP6_FADE_TO_PLAY;
            g_sfxVoice[i].fadeStep = pause ? -step : step;
        }
    }
    mp6_unlock();
    return 0;
}

s32 msmSeSetParam(int seNo, MSM_SEPARAM *param)
{
    MsmSeVoice *v;
    s32 result;
    printf("[AUDIO] msmSeSetParam(seNo=%d, flag=%#x%s)\n", seNo,
           param ? (unsigned)param->flag : 0u, param ? "" : " (param=NULL)");
    if (!param) return 0;
    mp6_lock();
    v = find_sfx_voice_by_no(seNo);
    if (v) {
        if (param->flag & MSM_SEPARAM_VOL) v->vol = param->vol;
        if (param->flag & MSM_SEPARAM_PAN) {
            int pan = param->pan;
            if (pan < 0) pan = 0;
            if (pan > 127) pan = 127;
            v->gainL = (127 - pan) / 127.0f;
            v->gainR = pan / 127.0f;
            v->pan = pan;
        }
        /* pitch/span/auxA/auxB/pos: intentionally not applied -- dry stereo,
         * no pitch-shifting resampler beyond the fixed native-rate step
         * already set at Play time (see this file's own scope note). */
        result = 0;
    } else {
        result = MP6_MSM_ERR_INVALIDSE;
    }
    mp6_unlock();
    return result;
}

void msmSeSetMasterVolume(s32 vol)
{
    printf("[AUDIO] msmSeSetMasterVolume(%d)\n", (int)vol);
    /* Deliberately a SEPARATE scalar from msmStreamSetMasterVolume's own
     * g_masterVol -- the real engine's sndMasterVolume call sites differ
     * per-bus (music vs SFX), and conflating them here would mean a
     * volume change aimed at one incorrectly affects the other. */
    mp6_lock();
    g_seMasterVol = (int)(vol & 127);
    mp6_unlock();
}

/* =======================================================================
 * AI family -- see this file's own top comment for why these 7 are all
 * safe as rich-logging passthrough stubs with no functional audio effect
 * (4 are THP-hardware-DMA-only and provably unreachable behind
 * THPInit()'s existing FALSE gate; the other 3 are a separate, lower-level
 * hardware-AI streaming path that src/msm/msmstream.c itself never calls
 * through at all -- zero AI* references anywhere in that file).
 * ======================================================================= */
u32 AIGetDMAStartAddr(void)
{
    printf("[AUDIO] AIGetDMAStartAddr() -- THP path, unreachable on this port's boot-to-menu scope "
           "(see src/null/shims_manual.c's THPInit)\n");
    return 0;
}

void AIInitDMA(u32 start_addr, u32 length)
{
    printf("[AUDIO] AIInitDMA(start_addr=%#x, length=%u) -- THP path, unreachable on this port's boot-to-menu scope\n",
           (unsigned)start_addr, (unsigned)length);
}

AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
    printf("[AUDIO] AIRegisterDMACallback(%p) -- THP path, unreachable on this port's boot-to-menu scope\n", (void *)callback);
    return NULL;
}

void AISetStreamPlayState(u32 state)
{
    printf("[AUDIO] AISetStreamPlayState(%u)\n", (unsigned)state);
}

void AISetStreamVolLeft(u8 vol)
{
    printf("[AUDIO] AISetStreamVolLeft(%u)\n", (unsigned)vol);
}

void AISetStreamVolRight(u8 vol)
{
    printf("[AUDIO] AISetStreamVolRight(%u)\n", (unsigned)vol);
}

void AIStartDMA(void)
{
    printf("[AUDIO] AIStartDMA() -- THP path, unreachable on this port's boot-to-menu scope\n");
}
