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
#include "dolphin.h"
#include "msm.h"
#include "mp6_shim_log.h"
#include "be.h"
#include "dspadpcm.h"
#include "wav_writer.h"
#include "msm_mixer.h"
#include <stdatomic.h>
#include "msm_safe.h"
#include "mp6_audio_out.h"
#include "host.h" /* mp6_host_mutex_* (mixer/group locks), mp6_host_thread_start
                   * + mp6_host_sleep_ns (the opt-in MP6_AUDIO_LEAKTEST_*
                   * stress threads) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own
 * includes is load-bearing, not stylistic: it is a #pragma clang section that
 * redirects every file-scope definition FOLLOWING it. As a -include (before all
 * headers) it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES. */
#include "mp6_savestate.h" /* W3: Mp6SsAudioShadow -- the audio re-sync shadow */
#include "mp6_boot.h"      /* mp6_tick_count -- the regular-proc diag keys off the GAME tick (see its comment) */
#include "mp6_events.h"    /* se.play -- "the UI just reacted to input", see msmSePlay */
#include "mp6_diag_probe.h" /* mp6_diag_audio_snapshot -- lock-copy, format outside */
#include "mp6_console.h"    /* the MP6_AUDIO_TIMELINE runtime lever */
#include "mp6_enhancements.h" /* mp6_enh_sfx_voices -- the 16/32 voice-table size */
#include "mp6_host_section.h"


/* ---- error codes not visible via this file's own preamble (top-level
 * "msm.h", the SAME header tools/gen_shims.py's probe resolves for these
 * symbols -- see this file's own header comment on why matching that
 * exact preamble matters). include/game/msm.h has these two, transcribed
 * directly. */
#define MP6_MSM_ERR_RANGE_STREAM (-140)
#define MP6_MSM_ERR_CHANLIMIT    (-110)

/* Real header value (include/game/msm_data.h's MSM_PDT_FILE_VERSION) --
 * transcribed locally rather than pulling in game/msm_data.h (this file's
 * own preamble deliberately mirrors shims_generated.c's exactly, see the
 * header comment above). Must be defined before msmSysInit uses it below. */
#define MSM_PDT_FILE_VERSION_LOCAL 1u

/* See this file's header comment, "FLAG BIT GROUND TRUTH". */
#define MP6_PACK_FLAG_STEREO 0x1
#define MP6_PACK_FLAG_LOOP   0x2

/* Not visible via this file's own resolved "msm.h" (mirrors shims_generated.c's
 * preamble -- resolves to the TOP-LEVEL include/msm.h, not include/game/msm.h;
 * see this file's header comment's discussion of the two competing headers).
 * MSM_ERR_CHANLIMIT/RANGE_STREAM above have this exact same gap; value
 * transcribed directly from include/game/msm.h. */
#define MP6_MSM_ERR_INVALIDSE (-111)

/* Same "not visible via this file's own resolved msm.h" gap as the two
 * above; values transcribed directly from include/game/msm.h (lines
 * 51-52). MSM_ERR_64 is the real engine's own "bad group id" result
 * (msmSysLoadGroupBase's grpId<1 / >=grpMax reject); STACK_OVERFLOW is
 * its "no free group slot". */
#define MP6_MSM_ERR_64             (-100)
#define MP6_MSM_ERR_STACK_OVERFLOW (-101)

/* Forward declaration -- msmSysInit's own MP6_AUDIO_SELFTEST_STREAMS hook
 * (below) calls this before its real definition appears later in this
 * file (msmStreamPlay's implementation comes after the mixer, matching
 * this file's own top-to-bottom section order). */
int msmStreamPlay(int streamId, MSM_STREAMPARAM *streamParam);
/* Same reason -- msmSysInit's own MP6_AUDIO_SELFTEST_SE hook calls this
 * before msmSePlay's real definition (which comes after the mixer,
 * matching msmStreamPlay's own placement). */
int msmSePlay(int seId, MSM_SEPARAM *param);

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

#define MP6_MSM_MAX_CHAN 8
#define MP6_RENDER_SCRATCH_FRAMES 4096
#define MP6_WAV_CAP_SECONDS 30
#define MP6_WAV_CAP_FRAMES (MP6_MSM_OUT_RATE * MP6_WAV_CAP_SECONDS)

/* =======================================================================
 * .pdt directory state -- parsed once by msmSysInit, kept for the whole
 * process lifetime (a few KB: streamMax*4 + numCoef*32 + packBytes bytes;
 * the real file's own numbers are ~7.5KB total).
 * ======================================================================= */
static char g_pdtPath[512];
static int g_pdtReady;
static int g_streamMax;
static int g_chanMax = 4;
static uint32_t *g_packListOfs;   /* [g_streamMax], absolute file offsets (0 = none) */
static uint8_t *g_packBlob;       /* raw bytes [streamPackOfs, sampleOfs) */
static uint32_t g_packBlobBase;   /* == header.streamPackOfs */
static uint32_t g_packBlobSize;
static uint32_t g_pdtFileSize;
static MP6AdpcmCoefTable *g_coef; /* [g_numCoef] */
static int g_numCoef;
static int g_masterVol = 127;

typedef struct {
    uint8_t flag;
    int8_t vol, pan, span, auxA, auxB;
    uint16_t frq;
    uint32_t loopEndByte;
    uint32_t loopStartByte;
    uint32_t subSampleOfs[2];
    int16_t subCoefIdx[2];
} PdtPack;

/* Fade-envelope action, shared by both MsmChan (BGM) and MsmSeVoice (SFX)
 * below. MP6_FADE_NONE is the default/steady state (no fade in progress --
 * fadeMul sits at exactly 1.0 or exactly 0.0 depending on paused, never in
 * between); the other 3 values are set only while `fadeStep != 0`, i.e.
 * while mp6_msm_render's own per-frame ramp (below) is actively moving
 * fadeMul toward 0 or 1. */
enum {
    MP6_FADE_NONE = 0,
    MP6_FADE_TO_PAUSE, /* fading OUT; on completion, settle into paused=1 (matches MSM_STREAM_PAUSEIN) */
    MP6_FADE_TO_STOP,  /* fading OUT; deactivate on completion, retire PCM on the control thread */
    MP6_FADE_TO_PLAY   /* fading IN (from pause or a startPaused=0 chan); on completion, paused=0, fadeMul=1 (matches MSM_STREAM_PAUSEOUT) */
};

/* =======================================================================
 * Playback channel state -- one per logical msm "stream channel" (matches
 * the `chan`/`streamNo` handle game code already uses, e.g.
 * game/audio.c's HuAudSStreamChanPlay(streamId, chanNo)). Fully decoded
 * PCM, not an incremental ring buffer -- see this file's header comment.
 * ======================================================================= */
typedef struct {
    int active;
    int paused;
    int loop;
    int streamId;
    int16_t *pcm;             /* interleaved stereo, malloc'd, totalFrames*2 samples */
    uint32_t totalFrames;
    uint32_t loopStartFrame;
    uint64_t posFrac;          /* Q16.16 position within pcm, in the stream's OWN native-rate frames */
    uint32_t stepFrac;         /* Q16.16 per-output-sample advance = (nativeFrq<<16)/MP6_MSM_OUT_RATE */
    int baseVol;                /* 0-127, authored (pack->vol) */
    int vol;                    /* 0-127, runtime (MSM_STREAMPARAM.vol, default MSM_VOL_MAX) */
    /* Fade envelope (see the enum comment above and this file's own
     * header comment, "Fade envelopes" -- mp6_msm_render applies these). */
    float fadeMul;               /* current envelope multiplier, always in [0,1] */
    float fadeStep;              /* per-output-frame delta; 0 = no fade in progress */
    int fadeAction;              /* one of the MP6_FADE_* values above */
} MsmChan;

static MsmChan g_chan[MP6_MSM_MAX_CHAN];

/* =======================================================================
 * Sound-effects playback via the SEPARATE MP6_SND.msm bank (MuSyX "group"
 * format -- a REAL, general-purpose synthesizer bank format, not this
 * game's own container like the .pdt stream file above). Format resolved
 * against the real disc file and cross-checked against the real,
 * fully-decompiled MuSyX runtime (src/musyx/runtime/*.c) for every offset
 * below. Short version:
 *
 *   MSM_HEADER (0x60 bytes, all big-endian u32/s32, same shape as the
 *   .pdt header): magic="GSND", version=2, then a run of (ofs,size) pairs
 *   -- infoOfs/infoSize, auxParamOfs/Size, grpInfoOfs/Size, musOfs/Size,
 *   seOfs/Size, grpDataOfs/Size, sampOfs/Size, dummyMusOfs/Size,
 *   grpSetOfs/Size (field order matches include/game/msm_data.h's
 *   MSM_HEADER exactly).
 *   MSM_INFO (64 bytes @ infoOfs): baseGrpNum (u8 @ offset 40) + baseGrp[]
 *   (u8 array @ offset 41) -- INDICES into the grpInfo table (NOT gids)
 *   for the "always loaded" groups (5 on this disc). The OTHER grpInfo
 *   entries load/unload dynamically via the game's own msmSysLoadGroup/
 *   msmSysDelGroupAll calls (per-scene menu/filesel/board/minigame
 *   groups) -- see the "DYNAMIC GROUPS" section below. MSM_INFO also
 *   carries stackDepthA/B (s8 @ offsets 9/10, the real engine's
 *   dynamic-group stack caps -- 0/8 on this disc; logged at init, this
 *   port's own flat slot table is capped by MP6_MSM_MAX_SE_GROUPS
 *   instead).
 *   MSM_SE (16 bytes each @ seOfs, seSize/16 entries): gid(u16) fxId(u16)
 *   vol(s8) pan(s8) pitchBend(s16) span/reverb/chorus/emitterF/emiComp/
 *   pad -- this file only reads gid/fxId/vol/pan (matching the dry
 *   stereo, no-aux-sends design used for streams above).
 *   MSM_GRP_INFO (32 bytes each @ grpInfoOfs): gid(u16) stackNo(s8)
 *   subGrpId(s8) dataOfs(s32) dataSize(s32) sampOfs(s32) sampSize(s32) --
 *   dataOfs/sampOfs are relative to grpDataOfs/sampOfs respectively.
 *   Each base group's OWN "data" blob (read once, kept in memory forever
 *   -- ~100KB total across all 5 real base groups, trivial) starts with
 *   MSM_GRP_HEAD (poolOfs/projOfs/sdirOfs/sngOfs, s32, relative to the
 *   blob's OWN start), then:
 *     - GROUP_DATA (0x28 bytes) @ blob+projOfs: nextOff(u32, BASE-relative
 *       to blob+projOfs, i.e. NOT self-relative and NOT accumulating --
 *       src/musyx/runtime/s_data.c's sndPushGroup always computes
 *       `prj_data + g->nextOff` from the SAME prj_data base) id(u16)
 *       type(u16) macroOff/sampleOff/curveOff/keymapOff/layerOff(u32, all
 *       relative to blob+projOfs too) then a union @ offset 0x1C: type==1
 *       is an "FX group" (type==0 is a song/sequencer group per
 *       src/musyx/runtime/s_data.c's own seqPlaySong) -> a single u32
 *       fx.tableOff (also projOfs-relative).
 *     - FX_DATA @ blob+projOfs+tableOff: num(u16) reserved(u16) then
 *       `num` x FX_TAB (10 bytes): id(u16, == MSM_SE.fxId) macro(u16)
 *       maxVoices/priority/volume/panning/key/vGroup(u8 each).
 *     - POOL_DATA (16 bytes) @ blob+poolOfs: macroOff/curveOff/keymapOff/
 *       layerOff(u32, relative to blob+poolOfs). The macro program for a
 *       given FX_TAB.macro id is found by walking a MEM_DATA singly-linked
 *       list starting @ blob+poolOfs+pool.macroOff: nextOff(u32, SELF-
 *       relative this time -- ADDS to the current node's own offset, per
 *       src/musyx/runtime/s_data.c's GetPoolAddr -- genuinely different
 *       from GROUP_DATA's base-relative scheme above) id(u16) reserved(u16)
 *       then up to 0x400 bytes of payload -- for a macro, this is a flat
 *       array of MSTEP (8 bytes: u32 para[0], u32 para[1]) "instructions".
 *     - MSTEP opcode = para[0] & 0x7f (src/musyx/runtime/synthmacros.c's
 *       macHandleActive dispatch switch): 0x0 = end-of-macro, 0x10 =
 *       StartSample (sample id = (para[0]>>8) & 0xffff). This file's own
 *       macro "interpreter" is deliberately minimal: linear-scan a macro's
 *       MSTEP array for the FIRST 0x10 (StartSample) opcode and use ITS
 *       sample id, ignoring every other opcode entirely (ADSR envelopes,
 *       vibrato, LFOs, portamento, pitch sweeps, ...) -- correct for
 *       simple sampled SFX (every macro this game actually uses is
 *       exactly this shape: a handful of steps ending in StartSample then
 *       a short wait then StopSample/end), not a general MuSyX macro VM.
 *       This does NOT handle multi-layer/velocity-switched instruments,
 *       envelopes, or pitch.
 *     - SDIR_DATA (32 bytes each) @ blob+sdirOfs, linear list terminated
 *       by id==0xffff (src/musyx/runtime/synthdata.h's own struct): id(u16)
 *       ref_cnt(u16) offset(u32, relative to THIS group's OWN sample pool,
 *       header.sampOfs+grpInfo.sampOfs) addr(u32, unused --live-only field)
 *       then a 16-byte SAMPLE_HEADER: info(u32 -- LOW 16 bits are the
 *       sample's native rate in Hz, e.g. 0x3c007d00 -> 0x7d00==32000,
 *       0x3c00ac44 -> 0xac44==44100, both real audio sample rates; HIGH
 *       byte is a root MIDI key, e.g. 0x3c==60=middle C) length
 *       (u32 -- TOP byte is compType per src/musyx/runtime/synthdata.c's
 *       own dataGetSample; only compType==0 is handled -- treated as
 *       standard GC DSP-ADPCM, matching every sample actually used by
 *       this game; the low 24 bits are a PCM SAMPLE count -- NOT bytes;
 *       raw byte length is ceil(length/14)*8 since one 8-byte frame
 *       decodes to 14 samples) loopOffset(u32) loopLength(u32) (both PCM
 *       samples too) -- then extraData(u32) @ offset 0x1C, which is NOT a
 *       sample-data offset at all (a name collision with the unrelated
 *       SAMPLE_INFO.extraData) -- it is the relative offset, from
 *       blob+sdirOfs (the SDIR TABLE's own base, per synthdata.c's
 *       `(size_t)&(dataSmpSDirs[i].data)->id + result->extraData`), of
 *       this sample's OWN small ADPCM side-data block -- i.e. it lives
 *       inline in the group's metadata blob (already fully in memory),
 *       NOT in a separate global table like the .pdt format above. That
 *       block is 0x28 (40) bytes, NOT just a bare 8-pair coefficient
 *       table: numCoef(u16, always 8) initialPS(u8) loopPS(u8) loopY0(s16)
 *       loopY1(s16) then the real 8-pair (16 x s16, 32 bytes) coefTab @
 *       offset +8 -- this exact sub-layout has NO decompiled C anywhere
 *       (musyx/runtime's own consumer, salBuildCommandList, exists only
 *       as un-decompiled PowerPC asm, build/GP6E01/asm/musyx/runtime/
 *       hw_dspctrl.s); `initialPS` matches the real sample data's own
 *       frame-0 header byte exactly for every sample checked. Getting
 *       this +8 wrong (reading coefficients starting right at extraData)
 *       silently misreads numCoef/initialPS/loopPS/loopY0/loopY1 as 2
 *       bogus "coefficient pairs".
 *
 * SCOPE:
 *   - The `baseGrpNum` BASE groups (5 on this disc, common/CMN menu SFX +
 *     similar always-resident sounds) load at init and stay resident
 *     forever. The OTHER groups load/unload dynamically through the
 *     game's own msmSysLoadGroup / msmSysDelGroupAll calls
 *     (HuAudSndGrpSetSet / HuAudDllSndGrpSet per scene) -- see the
 *     "DYNAMIC GROUPS" section below.
 *   - Only compType==0 (DSP-ADPCM) samples are handled; anything else is
 *     logged loudly and skipped rather than guessed at.
 *   - Only the macro's FIRST StartSample opcode is honored -- no vibrato,
 *     pitch-sweep or portamento. A real, audible sampled voice plays; most
 *     of the expressive-synthesis opcodes MuSyX supports do not run. The one
 *     exception is the authored VOLUME ENVELOPE (mcmdScaleVolume /
 *     mcmdEnvelope / mcmdFadeIn -- 0x0d/0x0f/0x14), which the same macro
 *     walk compiles into a small piecewise-linear program the mixer applies
 *     per rendered frame; msm_safe.h's own "AUTHORED MACRO VOLUME ENVELOPE"
 *     comment is the ground truth for what is modelled, what is refused, and
 *     why refusing leaves the voice bit-identical to the pre-envelope build.
 *     MP6_AUDIO_SE_ENV_CENSUS=1 prints the compiled program -- or the verdict
 *     refusing one -- for every seId whose macro authors a volume opcode at
 *     all; MP6_AUDIO_SE_MACRO_DUMP=<seId>[,...] prints the raw MSTEP stream
 *     behind one; MP6_AUDIO_NO_MACRO_ENV=1 turns the whole feature off in the
 *     SAME binary for an A/B. Resolution also handles
 *     KEYMAP/LAYER indirection (an
 *     fx's object id can name those, not just a macro -- see
 *     resolve_first_sample), but a multi-row layer still starts only its
 *     FIRST covering row: one voice per SE, skipped rows logged.
 *   - No 3D positional emitters (MSM_SEPARAM_POS / msmSeSetListener family
 *     stay the existing auto-generated no-op) -- flat stereo pan only,
 *     matching the dry-stereo scope used for streams above.
 * ======================================================================= */
/* The voice table's CAPACITY -- how many slots exist -- which is NOT how
 * many the runtime uses. The ACTIVE count is mp6_sfx_voice_cap() below: 16
 * (retail) or 32 (Enhancements: "Extended SFX voices"), initialized at
 * msmSysInit and updated under the mixer lock. Every behavior (allocation, mixing,
 * stop/pause sweeps, key-group release, the diagnostic census) is bounded by
 * that active count; only STORAGE -- this array and the fixed-size stack
 * mirrors of it -- is sized by the capacity. See msm_safe.h's own
 * "SFX voice-table capacity" comment for why the table is static at 32
 * rather than sized to the cap. */
#define MP6_MSM_MAX_SFX_VOICES MP6_MSM_SFX_VOICES_EXTENDED
#if MP6_MSM_MAX_SFX_VOICES != MP6_SS_AUDIO_MAX_VOICES
#error "savestate voice shadow must cover every runtime SFX slot"
#endif
#if MP6_DIAG_SFX_VOICE_MAX < MP6_MSM_MAX_SFX_VOICES
#error "diag audio snapshot must cover every runtime SFX slot"
#endif
/* Cap on ALL simultaneously-loaded groups: 5 init-time base groups + the
 * real engine's own dynamic-stack caps (MSM_INFO.stackDepthA/B = 0/8 on
 * this disc, logged at init) + msmSysLoadGroupBase's own extra-base-group
 * headroom (real cap: baseGrpNo < 0xF, msmsys.c) still fit with margin. */
#define MP6_MSM_MAX_SE_GROUPS 24

static char g_msmPath[512];
static int g_msmReady;
static uint32_t g_msmFileSize;
static int g_seMasterVol = 127;

typedef struct {
    uint16_t gid, fxId;
    int8_t vol, pan;
} MsmSeDef;

static MsmSeDef *g_seDefs;
static int g_seDefCount;

/* One fx->sample resolution PER GROUP -- a dynamically UNLOADED group
 * must take exactly its own entries away with it, and resolution is
 * gid-scoped (MSM_SE.gid names the group an fx id must be looked up in --
 * fx ids happen to be globally unique across this disc's groups, but
 * scoping by gid is the semantics the SE table actually encodes). */
typedef struct {
    uint16_t fxId;
    uint16_t sampleId;  /* resolved via the fx's macro's first StartSample opcode */
    /* AUTHORED NOTE LENGTH, in milliseconds, measured from the macro's own
     * StartSample to its EndOfMacro/Stop -- 0 when unknown.
     *
     * WHY THIS EXISTS. The real engine frees the voice when the MACRO ends:
     * src/musyx/runtime/synthmacros.c's mcmdEndOfMacro() calls voiceFree(),
     * and msmse.c's msmSePeriodicProc then reaps the SE_PLAYER as soon as
     * sndFXCheck() reports the voice is gone. The sample's own SDIR loop
     * flag only says "repeat WHILE the note is held" -- it never says "hold
     * forever". This port has no macro interpreter, so without this number a
     * loop-flagged sample plays until the game happens to call msmSeStop --
     * and for a macro that ends on its own, the game never does, so a 1.85 s
     * one-shot became an infinite loop (seId 1204, the opening storybook).
     *
     * Only LINEAR macros get a number: any branch/conditional opcode, or a
     * 0xFFFF "wait for keyoff" step, leaves this 0, which keeps the previous
     * behavior (loop until the game stops it) -- correct for those, because
     * a keyoff-held macro really does sound until msmSeStop. */
    uint32_t lifeMs;
    /* DIAGNOSTIC ONLY (MP6_AUDIO_SE_CENSUS) -- never read by playback.
     * Bitmask of MP6_LIFEWHY_* saying what the macro scan actually SAW, so
     * "this seId can loop forever" can be attributed to a specific opcode
     * class instead of guessed at. lifeTicks is the tempo-scaled wait total
     * the scan had to discard (MP6_LIFEWHY_TICKWAIT). */
    uint8_t lifeWhy;
    uint8_t lifeSmpEnd; /* 1 = an indefinite Wait resumes on SAMPLE END (cFlags 0x40000) */
    /* MusyX KEY GROUP, from the macro's own SetKeyGroup (opcode 0x59,
     * synthmacros.c mcmdSetKeyGroup): starting this fx KILLS (kill=1) or
     * KEYS OFF (kill=0) every voice already sounding in the same group.
     * 0 = no key group. This is the real engine's one-at-a-time rule and
     * the only thing that stops a rapidly retriggered SE from stacking. */
    uint8_t keyGroup;
    uint8_t keyGroupKill;
    uint8_t hasAdsr; /* macro set its own ADSR (0x0c/0x16/0x21) -> a keyoff may have a real release */
    /* MP6_ENVWHY_* -- diagnostic only (MP6_AUDIO_SE_ENV_CENSUS), see
     * find_macro_first_sample. It lives HERE, in two of the three padding
     * bytes this struct already had between hasAdsr and lifeTicks, while the
     * compiled PROGRAM lives in the sparse side table below. The split is not
     * arbitrary: a refusal verdict is set for essentially the whole bank
     * (MP6_ENVWHY_KEYOFF alone is set on all 2330 resolvable fx, because
     * `Wait 0xFFFF; StopSample; EndOfMacro` is their universal tail), so a
     * side row per non-zero envWhy would not be sparse at all -- while a
     * compiled program exists for exactly 10 of them. */
    uint16_t envWhy;
    uint32_t lifeTicks;
} MsmFxEntry;

/* 20 bytes -- the size this entry had before the volume envelope existed, and
 * the reason the program moved out of it. A group's fx index is a power-of-two
 * grown array over EVERY fx the group resolves: 1472 entries across the five
 * resident base groups (fxCap 64+512+512+128+256), so one byte here is 1.4 KB
 * of permanently resident host metadata. Embedding Mp6MsmSeEnv grew the entry
 * to 124 bytes and spent 182,528 bytes to carry ten programs. */
_Static_assert(sizeof(MsmFxEntry) == 20,
               "MsmFxEntry grew -- per-fx state is paid 1472 times across the resident base "
               "groups; a compiled volume envelope belongs in the sparse MsmFxEnvRow table");

/* THE AUTHORED VOLUME ENVELOPE, compiled out of the same macro walk that
 * produced sampleId/lifeMs -- msm_safe.h's Mp6MsmSeEnv has the format and the
 * three opcodes it models. An absent row means "this macro authors no volume
 * move this port can reproduce exactly", and then the mixer's per-voice gain
 * expression is untouched. Copied BY VALUE into the voice at msmSePlay (the
 * group may unload while the voice still sounds).
 *
 * SPARSE, AND KEYED BY THE fx's OWN INDEX in its group's fx[] array.
 * MP6_AUDIO_SE_ENV_CENSUS=1 over all 115 groups: resolved=2330 compiled=10
 * APPLIED=10 (envRows=10, 1040 bytes bank-wide). Ten programs is not a shape
 * worth paying for per fx, and only ONE of the five resident base groups
 * authors any at all (gid=1, seId 1064 and 1106) -- so the resident cost of
 * this table is two OCCUPIED rows inside one group's initial 4-row
 * allocation. The boot log's metadata total charges the CAPACITY, which is
 * why gid=1 accounts for 416 bytes here and the other four base groups for
 * zero.
 *
 * WHY THE INDEX AND NOT THE fxId. The index is unambiguous. Nothing in the
 * FX_DATA format forbids a repeated FX_TAB.id, and every reader resolves an
 * fxId by taking the FIRST matching entry -- so a table keyed by fxId could
 * hand one duplicate's program to the other. add_fx_entry writes the row for
 * the entry it is appending, and each reader looks up the index it already had
 * to scan for.
 *
 * NO "HAS AN ENVELOPE" MARKER IN MsmFxEntry, deliberately: that would be a
 * second copy of a fact this table already holds, i.e. a way for the two to
 * disagree about which fx is shaped. The ABSENCE of a row is exactly
 * segCount 0 -- the same neutral state every refusal in find_macro_first_sample
 * already lands on -- so there is one truth, not two. The lookup scan is over
 * fxEnvCount (two for the whole resident bank) inside a caller that already
 * scanned fxCount (up to 420) to find the fx at all. */
typedef struct {
    int fxIndex;      /* index into the owning group's fx[] -- unique per row */
    Mp6MsmSeEnv env;  /* segCount is always non-zero; a refusal writes no row */
} MsmFxEnvRow;

/* Why find_macro_first_sample could not produce a usable lifeMs, or what
 * else the macro contained. See that function. */
#define MP6_LIFEWHY_KEYOFFWAIT 0x01u /* Wait 0xFFFF -- held until keyoff */
#define MP6_LIFEWHY_TICKWAIT   0x02u /* tempo-scaled (tick) Wait */
#define MP6_LIFEWHY_BRANCH     0x04u /* IfKey/IfVel/IfMod/IfRandom/Loop/Goto/Gosub/Return/PlayMacro */
#define MP6_LIFEWHY_NOEND      0x08u /* no EndOfMacro/Stop inside the window */
#define MP6_LIFEWHY_RANDWAIT   0x10u /* Wait with the "random duration" bit set */
#define MP6_LIFEWHY_STOPSAMPLE 0x20u /* 0x11 StopSample after the StartSample */
#define MP6_LIFEWHY_KEYOFFCMD  0x40u /* 0x12 KeyOff after the StartSample */
/* Resume condition of an indefinite (0xFFFF) Wait, straight from mcmdWait:
 * para[0] bit8 sets SYNTH_VOICE.cFlags 4 (macSetExternalKeyoff resumes the
 * macro), para[0] bit24 sets cFlags 0x40000 (macSampleEndNotify resumes it).
 * The second one is the ONE-SHOT idiom -- "hold until this sample ends,
 * then fall through to EndOfMacro -> voiceFree". */
#define MP6_LIFEWHY_WAITKEYOFF 0x80u /* indefinite Wait resumes on KEYOFF */

/* What the VOLUME-ENVELOPE half of the same macro walk saw. Diagnostic only
 * (MP6_AUDIO_SE_ENV_CENSUS) -- playback reads only whether the fx has an
 * MsmFxEnvRow at all, never this bitmask.
 * Every bit except SAW_VOLOP is a REFUSAL: the scan met something it cannot
 * reproduce exactly and dropped the whole program, leaving the voice's gain
 * expression untouched. See msm_safe.h's own envelope comment for why each
 * of these is unmodellable rather than merely unimplemented. */
#define MP6_ENVWHY_SAW_VOLOP   0x01u /* macro contains at least one 0x0d/0x0f/0x14 */
#define MP6_ENVWHY_UNMODELLED  0x02u /* bias byte, real curve, or tick-timed ramp */
#define MP6_ENVWHY_TIMEUNKNOWN 0x04u /* a volume move after macro time stopped being linear ms */
#define MP6_ENVWHY_TOOMANY     0x08u /* more moves than MP6_MSM_SE_ENV_MAX_SEGS */
#define MP6_ENVWHY_OVERLAP     0x10u /* a move began before the previous one finished */
#define MP6_ENVWHY_ADSR        0x20u /* 0x0c/0x16/0x21 -- a real ADSR/DLS state machine */
#define MP6_ENVWHY_KEYOFF      0x40u /* 0x11/0x12 after the StartSample -- release not modelled */
#define MP6_ENVWHY_PRESTART    0x80u /* a RAMP (not just a set) before the StartSample */
#define MP6_ENVWHY_BRANCH     0x100u /* branch/loop/goto/gosub after the StartSample */

/* The whole grpInfo directory (ALL 115 entries on this disc, not just the
 * 5 base ones), parsed once at init and kept -- msmSysLoadGroup takes a
 * grpInfo INDEX (include/msm_grp.h's MSM_GRP_* constants, e.g.
 * MSM_GRP_MENU=9/MSM_GRP_FILESEL=8; the .msm's grpSet table is EMPTY on
 * this disc -- grpSetSize=0 -- so "group set" ids ARE grpInfo indices),
 * and needs every entry's layout info to load it on demand. ~32 bytes x
 * 115 = trivial. */
typedef struct {
    uint16_t gid;
    int8_t stackNo;    /* which real-engine stack (A=0/B=1) -- informational here */
    int8_t subGrpId;   /* ANOTHER grpInfo INDEX to co-load first (0 = none) --
                          real msmSysLoadGroupSub loads it alongside, msmsys.c */
    uint32_t dataOfs, dataSize;   /* metadata blob, relative to header.grpDataOfs */
    uint32_t sampOfs, sampSize;   /* sample pool, relative to header.sampOfs */
} MsmGrpInfo;

static MsmGrpInfo *g_grpInfo;
static int g_grpInfoCount;
static uint32_t g_msmGrpDataOfs;  /* header.grpDataOfs */
static uint32_t g_msmSampOfsHdr;  /* header.sampOfs */
static int g_baseGrpIdx[32];      /* MSM_INFO.baseGrp[] (grpInfo indices) */
static int g_baseGrpNum;
static s32 g_sampSizeMaxBase;     /* max base-group sampSize == real sys.sampSize */
static s32 g_sampSizeMaxDyn;      /* max NON-base sampSize == real sys.sampSizeBase */
static int g_grpLoadMode;         /* msmSysSetGroupLoadMode value -- recorded + logged;
                                     this port's flat slot table behaves identically in
                                     both modes (no ARAM stack layout to juggle) */

typedef struct {
    int inUse;
    int baseGrpF;        /* 1 = survives msmSysDelGroupAll (init-time base groups
                            AND msmSysLoadGroupBase-added ones, like the real
                            engine's grp->baseGrpF) */
    int dynBaseF;        /* 1 = added via msmSysLoadGroupBase AFTER init (the only
                            ones msmSysDelGroupBase may remove; the 5 init-time
                            base groups are permanent) */
    int grpIdx;          /* grpInfo INDEX this slot was loaded from */
    uint16_t gid;        /* g_grpInfo[grpIdx].gid -- the SE table's own gid unit */
    int loadOrder;       /* ever-increasing stamp -- msmSysDelGroupBase pops the
                            most recently loaded first (real sys.grpLoadId LIFO) */
    uint8_t *blob;       /* this group's own "data" blob (GRP_HEAD + GROUP_DATA/
                          * FX_DATA under projOfs + POOL_DATA/macros under poolOfs
                          * + SDIR table under sdirOfs), malloc'd; freed on unload */
    uint32_t blobSize;
    uint32_t poolOfs, projOfs, sdirOfs;
    uint32_t sampPoolSize;
    int parseError;      /* sticky: an internal offset/span escaped blobSize */
    uint32_t sampPoolFileOfs; /* header.sampOfs + this group's own grpInfo.sampOfs --
                                  absolute .msm file offset of byte 0 of this group's
                                  OWN sample pool (SDIR_DATA.offset is relative to this) */
    MsmFxEntry *fx;      /* this group's OWN fx->sample index, malloc'd; freed on unload */
    int fxCount;
    int fxCap;
    MsmFxEnvRow *fxEnv;  /* SPARSE compiled volume programs for the few entries in
                          * fx[] that have one -- see MsmFxEnvRow. malloc'd, freed
                          * with the group, and grown only when a program compiles,
                          * so a group that authors none never allocates it. */
    int fxEnvCount;
    int fxEnvCap;
} MsmSeGroup;

static MsmSeGroup g_seGroups[MP6_MSM_MAX_SE_GROUPS];
static int g_grpLoadCounter = 1;

typedef struct {
    uint32_t offset;   /* relative to the owning group's sampPoolFileOfs (a BYTE offset --
                          frame-0's own PS header byte at exactly this offset matches
                          the coef sub-header's `initialPS` for every sample) */
    uint32_t length;   /* PCM SAMPLE count (masked, low 24 bits of SAMPLE_HEADER.length)
                          -- NOT a byte count. Misreading this as bytes decodes wrong-
                          coefficient ADPCM onto every SFX's tail (audible as noise
                          after the real sound). Confirmed threefold: (1) the
                          decompiled runtime's own hwGetPos (src/musyx/runtime/
                          hardware.c) converts a DSP nibble address to a POSITION via
                          `((cur - addr*2) / 16) * 14` -- 16 nibbles (one 8-byte frame)
                          == 14 units, so positions/lengths in this API are PCM
                          samples by definition; (2) mcmdStartSample (synthmacros.c)
                          clamps a sample-START offset against exactly this field --
                          same units; (3) consecutive real SDIR entries' byte offsets
                          differ by ceil(length/14)*8 for every adjacent pair across
                          all 5 base groups (the byte reading instead overruns every
                          group's own sampSize). */
    uint8_t compType;  /* top byte of SAMPLE_HEADER.length -- only 0 (ADPCM) handled */
    uint32_t coefTableRelOfs; /* == SDIR_DATA.extraData, relative to blob+sdirOfs */
    uint32_t sampleRateHz;    /* low 16 bits of SAMPLE_HEADER.info */
    uint32_t loopStart;       /* SAMPLE_HEADER.loopOffset -- PCM samples (same unit) */
    uint32_t loopLength;      /* SAMPLE_HEADER.loopLength -- PCM samples; 0 = one-shot */
} MsmSampleInfo;

typedef struct {
    int active;
    int paused;
    int loop;
    int16_t *pcm;        /* MONO, malloc'd, totalFrames samples */
    uint32_t totalFrames;
    uint32_t loopStartFrame;
    uint32_t loopEndFrame; /* exclusive */
    uint64_t posFrac;    /* Q16.16, in the sample's own native-rate frames */
    uint32_t stepFrac;   /* Q16.16 per-output-sample advance, native rate -> MP6_MSM_OUT_RATE */
    int baseVol, vol;    /* 0-127 each, same authored*runtime*master formula as g_chan */
    float gainL, gainR;  /* from pan (0-127, 64==center), simple linear pan law */
    int pan;             /* retained explicitly so savestates can replay the exact gain */
    int seId;            /* immutable MSM_SE-table replay identity */
    int grpIdx;          /* group that supplied the private decoded PCM */
    int no;              /* unique, ever-increasing handle -- matches the REAL msmSePlay's
                          * own player->no contract (msmSeSearchEntry-by-.no in the decomp) */
    /* MusyX KEY GROUP this voice belongs to (0 = none), from its fx macro's
     * SetKeyGroup opcode. See MsmFxEntry.keyGroup and mp6_se_keygroup_release. */
    int keyGroup;
    uint16_t gid;        /* The owning SE def's group id -- msmSeStopAll(checkGrp=TRUE)
                          * (the real "stop only NON-base-group voices" semantics, msmse.c's
                          * own msmSysCheckBaseGroup gate) needs it. The voice's decoded PCM
                          * is its own private copy, so a voice may safely OUTLIVE its
                          * group's unload. */
    /* Fade envelope -- same shape as MsmChan's own 3 fields above, see
     * that struct's own comment and this file's header comment. */
    float fadeMul;
    float fadeStep;
    int fadeAction;
    /* THE MACRO'S AUTHORED VOLUME ENVELOPE, resolved onto THIS voice's own
     * frame clock at msmSePlay (msm_safe.h's Mp6MsmEnvPlan). segCount == 0 --
     * the state every voice that has no modellable envelope keeps -- makes
     * the mixer skip the multiply entirely, so those voices render bit-for-bit
     * what they rendered before this field existed.
     *
     * DERIVED, NOT INTEGRATED, and that is deliberate: the multiplier is a
     * pure function of the voice's CURRENT sample index, which the mixer
     * already computes and a savestate already marshals (Mp6SsAudioVoice.
     * posFrac). There is no envelope accumulator to capture, to validate, or
     * to get out of step with the position -- a restored voice resumes its
     * envelope exactly where its position says it is. That is only sound
     * while position and elapsed time are the same thing, which is why
     * msmSePlay refuses to install a plan on a voice that still LOOPS. */
    Mp6MsmEnvPlan envPlan;
    /* MP6_AUDIO_TIMELINE bookkeeping ONLY -- never read by the mixer's audio
     * path, never captured by a savestate. loopWraps is bumped inside the
     * mixer (which already holds g_mixerLock for the whole render), so the
     * game-thread census in msmSysRegularProc reads it under the same lock. */
    uint64_t tlStartTick;
    uint32_t tlWraps;
    uint32_t tlLastCensusWraps;
    int tlReported;
} MsmSeVoice;

static MsmSeVoice g_sfxVoice[MP6_MSM_MAX_SFX_VOICES];
static int g_seNoCounter = 1;

/* Host configuration, excluded from game-state snapshots. Changes retire
 * removed slots under g_mixerLock without renumbering surviving handles.
 * Atomic because a few range checks read the limit outside the mixer lock;
 * table access still uses the existing mixer lock. */
static _Atomic int g_sfxVoiceCap;

/* Read the latch, resolving it on first use if msmSysInit has not run yet.
 * The lazy path exists only so a caller that runs before init (nothing does
 * today -- msmSePlay needs g_msmReady and the mixer needs g_mixerLockInit)
 * cannot read a zero cap and silently behave as if there were no voices. The
 * store is idempotent: mp6_enh_sfx_voices() is a pure function of the
 * process environment/config, so a benign race writes the same value. */
static int mp6_sfx_voice_cap(void)
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

static Mp6Mutex g_mixerLock; /* The host seam's caller-storage mutex (win32
                              * backend is the same CRITICAL_SECTION inside) */
static int g_mixerLockInit;
/* Restore-only render gate.  SFX replay decodes outside g_mixerLock, so the
 * callback must emit silence until every recreated voice has had its exact
 * slot/position/fade/handle published; otherwise it can play frame zero in
 * the short window after msmSePlay publishes the temporary voice. */
static int g_savestateMixerMuted;

static void mp6_lock(void) { mp6_host_mutex_lock(&g_mixerLock); }
static void mp6_unlock(void) { mp6_host_mutex_unlock(&g_mixerLock); }

/* A control transaction detaches at most every voice/channel plus one
 * replacement. Ownership moves under the mixer lock; heap work never does. */
typedef struct {
    int16_t *pcm[MP6_MSM_MAX_SFX_VOICES + MP6_MSM_MAX_CHAN + 1];
    size_t count;
} MsmPcmRetirement;

static void mp6_pcm_detach(MsmPcmRetirement *retired, int16_t **pcm)
{
    if (*pcm) {
        if (retired->count >= sizeof(retired->pcm) / sizeof(retired->pcm[0])) abort();
        retired->pcm[retired->count++] = *pcm;
        *pcm = NULL;
    }
}

static void mp6_pcm_release(MsmPcmRetirement *retired)
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

/* A SEPARATE lock for the loaded-group table (g_seGroups/g_grpInfo and
 * every blob/fx pointer hanging off it). NOT g_mixerLock: the mixer render
 * thread never touches group state at all (voices carry their own decoded
 * PCM copies), so group loads -- which do real file I/O -- must not stall
 * the audio callback. Group mutation happens on the game's own thread
 * (HuAudSndGrpSetSet at scene changes); the lock exists because the
 * OPT-IN leak-stress threads (MP6_AUDIO_LEAKTEST_SE / MP6_AUDIO_LEAKTEST_GRPSWAP
 * below) call msmSePlay/msmSysLoadGroup from a second thread concurrently
 * with it. msmSePlay COPIES everything it needs (sample info + coef table)
 * out under this lock, then does its file read + decode lock-free -- see
 * its own comment. */
static Mp6Mutex g_grpLock; /* Same CRITICAL_SECTION-backed mutex as g_mixerLock above */

static void mp6_grp_lock(void) { mp6_host_mutex_lock(&g_grpLock); }
static void mp6_grp_unlock(void) { mp6_host_mutex_unlock(&g_grpLock); }

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
static BOOL audio_file_size(const char *path, uint32_t *sizeOut)
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

static BOOL get_pack(int streamId, PdtPack *out)
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
 * samples" differ by a factor of 14. pack_adpcm_frame_count() returns the
 * FORMER (what decode_substream's
 * own `numFrames` parameter -- and mp6_dspadpcm_decode's, matching it --
 * both mean); callers that need a PCM sample count (buffer sizing,
 * totalFrames/loopStartFrame bookkeeping) must multiply by 14 themselves,
 * explicitly, at the call site -- never pass a sample count in here. */
static uint32_t pack_adpcm_frame_count(const PdtPack *pack)
{
    return pack->loopEndByte / 8;
}

static s32 decode_substream(uint32_t sampleOfs, int16_t coefIdx, uint32_t numFrames, int16_t *outMono)
{
    uint8_t *raw;
    MP6AdpcmState state;

    if (numFrames == 0 || outMono == NULL ||
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
    raw = (uint8_t *)malloc((size_t)numFrames * 8);
    if (!raw) {
        fprintf(stderr, "[AUDIO] msm_bridge: out of memory decoding %u ADPCM frames\n", (unsigned)numFrames);
        return MSM_ERR_OUTOFMEM;
    }
    if (!pdt_read_range(sampleOfs, numFrames * 8, raw)) {
        free(raw);
        return MSM_ERR_READFAIL;
    }
    state.hist1 = 0;
    state.hist2 = 0;
    mp6_dspadpcm_decode(raw, numFrames, &g_coef[coefIdx], &state, outMono);
    free(raw);
    return 0;
}

/* =======================================================================
 * MP6_SND.msm bank access + fx->sample resolution. See the
 * MsmSeGroup/MsmFxEntry declarations above for the full format writeup.
 * ======================================================================= */
static BOOL msm_read_range(uint32_t offset, uint32_t length, void *dst)
{
    DVDFileInfo fi;
    BOOL ok;

    if (length == 0) return TRUE;
    if (!DVDOpen(g_msmPath, &fi)) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDOpen(\"%s\") failed\n", g_msmPath);
        return FALSE;
    }
    if (dst == NULL || length > (uint32_t)INT_MAX ||
        !mp6_msm_span_valid_u32(fi.length, offset, length)) {
        fprintf(stderr, "[AUDIO] msm_bridge: invalid .msm read span "
                "(offset=%u length=%u file=%u)\n",
                (unsigned)offset, (unsigned)length, (unsigned)fi.length);
        DVDClose(&fi);
        return FALSE;
    }
    ok = DVDReadPrio(&fi, dst, (s32)length, (s32)offset, 2);
    DVDClose(&fi);
    if (!ok) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDReadPrio (msm) failed (offset=%u length=%u)\n",
                (unsigned)offset, (unsigned)length);
    }
    return ok;
}

static BOOL group_span(MsmSeGroup *grp, uint64_t offset, uint64_t size, const char *what)
{
    if (offset > grp->blobSize || size > (uint64_t)grp->blobSize - offset) {
        if (!grp->parseError) {
            fprintf(stderr,
                    "[AUDIO] malformed .msm group idx=%d gid=%u: %s span "
                    "offset=%llu size=%llu escapes %u-byte blob\n",
                    grp->grpIdx, (unsigned)grp->gid, what,
                    (unsigned long long)offset, (unsigned long long)size,
                    (unsigned)grp->blobSize);
        }
        grp->parseError = 1;
        return FALSE;
    }
    return TRUE;
}

/* Everything ONE macro walk recovers: the note length, the volume envelope,
 * the key group, and the diagnostic bitmasks explaining each. Declared here
 * rather than next to find_macro_first_sample (which fills it, and whose own
 * header comment is the ground truth for every field) because add_fx_entry
 * below takes the whole struct. */
typedef struct {
    uint32_t lifeMs;   /* 0 unless the macro was fully linear + ms-timed */
    uint32_t ticks;    /* total of the tempo-scaled Wait steps it discarded */
    uint8_t why;       /* MP6_LIFEWHY_* bitmask */
    uint8_t smpEnd;    /* 1 = an indefinite Wait resumes on SAMPLE END */
    uint8_t keyGroup;  /* SetKeyGroup (0x59) group id, 0 = none */
    uint8_t keyGroupKill; /* SetKeyGroup kill flag */
    uint8_t hasAdsr;   /* macro set its own ADSR envelope */
    Mp6MsmSeEnv env;   /* the compiled volume program -- segCount 0 == none */
    uint16_t envWhy;   /* MP6_ENVWHY_* bitmask */
} MsmMacroScan;

/* Appends to the OWNING GROUP's fx index (freed wholesale with the
 * group on unload -- see MsmFxEntry's own comment for why per-group), plus a
 * SPARSE MsmFxEnvRow for the rare fx whose macro compiled a volume program.
 * Takes the macro scan's whole result rather than one positional argument
 * per recovered field: the two structs carry the same facts, and a scalar
 * list this long is where a mis-ordered pair of same-typed flags hides. */
static BOOL add_fx_entry(MsmSeGroup *grp, uint16_t fxId, uint16_t sampleId,
                         const MsmMacroScan *life)
{
    MsmFxEntry *e;
    if (grp->fxCount >= grp->fxCap) {
        int newCap = grp->fxCap ? grp->fxCap * 2 : 64;
        MsmFxEntry *n = (MsmFxEntry *)realloc(grp->fx, (size_t)newCap * sizeof(MsmFxEntry));
        if (!n) {
            fprintf(stderr,
                    "[AUDIO] msm group idx=%d gid=%u: out of memory growing FX index to %d entries\n",
                    grp->grpIdx, (unsigned)grp->gid, newCap);
            grp->parseError = 1; /* materialization must not publish a partial index */
            return FALSE;
        }
        grp->fx = n;
        grp->fxCap = newCap;
    }
    /* The envelope row is appended BEFORE fxCount rises, and its failure is
     * the same sticky parseError the fx index uses. A group must never be
     * published carrying an fx whose compiled program went missing: the fx
     * would then play at the pre-envelope FLAT gain, which for seId 1106 is
     * exactly the full-scale beep this feature exists to remove -- a silent
     * downgrade to the bug. The initial capacity is 4 because the whole disc
     * compiles ten programs across 115 groups (four in the widest group). */
    if (life->env.segCount != 0) {
        if (grp->fxEnvCount >= grp->fxEnvCap) {
            int newCap = grp->fxEnvCap ? grp->fxEnvCap * 2 : 4;
            MsmFxEnvRow *n = (MsmFxEnvRow *)realloc(grp->fxEnv,
                                                    (size_t)newCap * sizeof(MsmFxEnvRow));
            if (!n) {
                fprintf(stderr,
                        "[AUDIO] msm group idx=%d gid=%u: out of memory growing FX volume-envelope "
                        "table to %d rows\n", grp->grpIdx, (unsigned)grp->gid, newCap);
                grp->parseError = 1;
                return FALSE;
            }
            grp->fxEnv = n;
            grp->fxEnvCap = newCap;
        }
        grp->fxEnv[grp->fxEnvCount].fxIndex = grp->fxCount;
        grp->fxEnv[grp->fxEnvCount].env = life->env;
        grp->fxEnvCount++;
    }
    e = &grp->fx[grp->fxCount];
    memset(e, 0, sizeof(*e));
    e->fxId = fxId;
    e->sampleId = sampleId;
    e->lifeMs = life->lifeMs;
    e->lifeWhy = life->why;
    e->lifeTicks = life->ticks;
    e->lifeSmpEnd = life->smpEnd;
    e->keyGroup = life->keyGroup;
    e->keyGroupKill = life->keyGroupKill;
    e->hasAdsr = life->hasAdsr;
    e->envWhy = life->envWhy;
    grp->fxCount++;
    return TRUE;
}

/* The compiled volume program for the fx at `fxIndex` in this group's fx[], or
 * NULL for the overwhelming majority that have none -- which the caller must
 * treat exactly as segCount 0, because that is what an absent row means (see
 * MsmFxEnvRow). Read-only; the caller owns whatever lock protects `grp`. */
static const Mp6MsmSeEnv *se_group_fx_env(const MsmSeGroup *grp, int fxIndex)
{
    int i;
    for (i = 0; i < grp->fxEnvCount; i++) {
        if (grp->fxEnv[i].fxIndex == fxIndex) return &grp->fxEnv[i].env;
    }
    return NULL;
}

/* GROUP_DATA linked list @ blob+projOfs -- nextOff is BASE-relative to
 * projOfs (see this file's header comment). Finds the entry whose own
 * `id` field matches targetGid; *outOff receives its offset (relative to
 * blob+projOfs, i.e. the absolute address is blob+projOfs+*outOff). */
static BOOL find_group_data(MsmSeGroup *grp, uint16_t targetGid, uint32_t *outOff)
{
    uint32_t off = 0;
    int guard = 0;

    for (;;) {
        uint64_t abs = (uint64_t)grp->projOfs + off;
        const uint8_t *g;
        if (!group_span(grp, abs, 0x28, "GROUP_DATA")) return FALSE;
        g = grp->blob + (size_t)abs;
        uint32_t nextOff = be32(g + 0);
        uint16_t id = (uint16_t)be16(g + 4);
        if (id == targetGid) {
            *outOff = off;
            return TRUE;
        }
        if (nextOff == 0xFFFFFFFFu) return FALSE;
        if (nextOff == off || ++guard > 64) {
            grp->parseError = 1;
            return FALSE;
        }
        off = nextOff; /* base-relative -- replaces, does not accumulate */
    }
}

/* MEM_DATA singly-linked pool lists @ blob+poolOfs+POOL_DATA.<which>Off --
 * nextOff is SELF-relative here (adds to the CURRENT node's own offset; a
 * genuinely different scheme than GROUP_DATA's base-relative one above,
 * per src/musyx/runtime/s_data.c's GetPoolAddr). Handles all four pool
 * lists (POOL_DATA is 4 u32 offsets: macroOff@+0 curveOff@+4 keymapOff@+8
 * layerOff@+0xC, include/musyx/synthdata.h) because fx table entries can
 * name KEYMAP (0x4000-tagged) and LAYER (0x8000-tagged) objects, not just
 * macros -- see resolve_first_sample below. listOfsField is one of
 * 0/4/8/0xC; a 0 stored offset means "this group has no such list" (e.g.
 * gid 176's keymapOff/curveOff are both 0). Node ids are stored WITH
 * their type tag (s_data.c's InsertData does `id |= 0x4000/0x8000` before
 * the lookup). */
static const uint8_t *pool_find_node(MsmSeGroup *grp, uint32_t listOfsField, uint16_t nodeId)
{
    const uint8_t *poolBase;
    uint32_t off;
    int guard = 0;

    if (listOfsField > 0xC || !group_span(grp, grp->poolOfs, 16, "POOL_DATA")) return NULL;
    poolBase = grp->blob + grp->poolOfs;
    off = be32(poolBase + listOfsField);
    if (off == 0) return NULL; /* absent list */
    if (off < 16) {
        grp->parseError = 1; /* must begin after the 4-offset POOL_DATA header */
        return NULL;
    }
    for (;;) {
        uint64_t abs = (uint64_t)grp->poolOfs + off;
        const uint8_t *m;
        if (!group_span(grp, abs, 8, "MEM_DATA")) return NULL;
        m = grp->blob + (size_t)abs;
        uint32_t nextOff = be32(m + 0);
        uint16_t id = (uint16_t)be16(m + 4);
        if (id == nodeId) return m;
        if (nextOff == 0xFFFFFFFFu) return NULL;
        if (++guard > 4096) {
            grp->parseError = 1;
            return NULL;
        }
        if (nextOff == 0 || (uint64_t)off + nextOff > UINT32_MAX) {
            grp->parseError = 1;
            return NULL;
        }
        off += nextOff; /* self-relative -- accumulates */
    }
}

/* Scans a macro node's MSTEP array for the first StartSample (0x10)
 * opcode; *outSampleId receives (para[0]>>8)&0xffff from that step.
 *
 * *outLifeMs receives the macro's AUTHORED NOTE LENGTH -- see MsmFxEntry's
 * own lifeMs comment for why this port needs it at all. It is the sum of
 * the Wait (0x04) / WaitMs (0x07) steps that follow StartSample, up to the
 * macro's EndOfMacro (0x00) / Stop (0x01) -- the two opcodes whose handlers
 * (synthmacros.c mcmdEndOfMacro/mcmdStop) call voiceFree().
 *
 * WAIT STEP ENCODING, straight from synthmacros.c's mcmdWait: the duration
 * is `(u16)(para[1] >> 16)`; `(para[1] >> 8) & 1` selects milliseconds over
 * sequencer ticks (mcmdWaitMs forces that bit); and 0xFFFF means "wait
 * indefinitely", i.e. until keyoff.
 *
 * DELIBERATELY CONSERVATIVE. *outLifeMs is left 0 -- "unknown, keep the
 * previous loop-until-stopped behavior" -- for anything this straight-line
 * reader cannot account for exactly: an indefinite wait, a TICK-based wait
 * (sndConvertTicks scales by the live sequencer tempo, which is not knowable
 * here), any branch/conditional/call opcode, or a macro that never reaches
 * its own end within the scanned window. Only a fully linear, purely
 * millisecond-timed macro yields a number.
 *
 * `outLife->why` additionally records what the scan SAW (MP6_LIFEWHY_*) --
 * diagnostic only, so the census can attribute an "unbounded" verdict to a
 * specific opcode class rather than lumping every failure together.
 *
 * THE VOLUME ENVELOPE rides on this exact same walk, because it needs the
 * exact same clock: the ms-timed Wait steps between the StartSample and the
 * macro's end ARE the time axis the authored volume moves sit on. It is
 * compiled into `outLife->env` under the same discipline as lifeMs -- every
 * feature this straight-line reader cannot reproduce EXACTLY drops the whole
 * program (segCount 0) instead of approximating it, with the reason recorded
 * in `outLife->envWhy`. msm_safe.h's "AUTHORED MACRO VOLUME ENVELOPE" comment
 * lists what is modelled and what is refused; the refusals enforced here on
 * top of the per-opcode ones are:
 *
 *   - any 0x0c/0x16/0x21 anywhere (a real ADSR/DLS state machine),
 *   - any branch/loop/goto/gosub after the StartSample (macro time stops
 *     being a straight sum of Waits, and a backward jump could re-run a
 *     volume move at a time this reader never sees),
 *   - a volume RAMP before the StartSample (it would already be mid-move when
 *     the sample begins; an instantaneous SET before it is fine, and becomes
 *     the program's own move at time zero),
 *   - a volume move once macro time is no longer a known millisecond count,
 *   - more moves than MP6_MSM_SE_ENV_MAX_SEGS, or two that overlap in time.
 *
 * A 0x11 StopSample / 0x12 KeyOff after the StartSample is NOT in that list --
 * it is recorded (MP6_ENVWHY_KEYOFF) and otherwise ignored. It looks like it
 * should matter, because keyoff is what drives an ADSR's RELEASE phase, and
 * refusing on it was the draft's first instinct. It is wrong for this bank: a
 * volume ADSR exists only where mcmdSetADSR/mcmdSetADSRFromCtrl called
 * hwSetADSR, and MP6_AUDIO_SE_ENV_CENSUS over all 115 groups finds 0x0c/0x16/
 * 0x21 in ZERO of the 2330 resolvable fx. With no ADSR ever established,
 * hwKeyOff has no volume envelope to release, and 0x11/0x12 move no volume at
 * all -- they are the universal `Wait 0xFFFF; StopSample; EndOfMacro` tail,
 * present in every single one of those 2330 fx. Refusing on them refused the
 * entire bank.
 */
static BOOL find_macro_first_sample(MsmSeGroup *grp, uint16_t macroId,
                                    uint16_t *outSampleId, MsmMacroScan *outLife)
{
    const uint8_t *m = pool_find_node(grp, 0 /* POOL_DATA.macroOff */, macroId);
    const uint8_t *steps;
    int i;
    int started = 0;
    /* Accumulated straight into the caller's struct rather than into a dozen
     * scalars copied out again at each of the three exits below -- that shape
     * is precisely where a newly-added field gets published on one exit path
     * and silently zeroed on the other two. */
    MsmMacroScan sc;
    uint32_t lifeMs = 0;
    int lifeUsable = 1;
    /* Volume-envelope compilation state; see this function's header comment.
     * The program is built here and published only if nothing refused it. */
    Mp6MsmSeEnv env;
    int envUsable = 1;
    int envTimeKnown = 1;                  /* ms since StartSample is still exact */
    uint32_t envMs = 0;                    /* ms since StartSample */
    uint32_t envPrevEndMs = 0;             /* end of the last emitted move */
    uint32_t curVol = MP6_MSM_ENV_UNITY;   /* running 16.16 volume; starts at the
                                            * modelled orgVolume (full scale) */

    memset(&sc, 0, sizeof(sc));
    memset(&env, 0, sizeof(env));
    if (outLife) *outLife = sc;
    if (!m) return FALSE;
    steps = m + 8;
    for (i = 0; i < 64; i++) {
        uint64_t stepOff = (uint64_t)(steps - grp->blob) + (size_t)i * 8;
        uint32_t p0, p1;
        uint8_t opcode;
        if (started) {
            /* Past the StartSample the walk exists only to MEASURE, so it
             * must be silent and side-effect free: group_span sets the
             * group's sticky parseError, which would reject an otherwise
             * perfectly loadable group just because a macro's step array
             * runs to the very end of the blob. Stop quietly instead --
             * the fx entry is already earned, only lifeMs is lost. */
            if (stepOff > grp->blobSize || (uint64_t)8 > (uint64_t)grp->blobSize - stepOff) {
                sc.why |= MP6_LIFEWHY_NOEND;
                break;
            }
        } else if (!group_span(grp, stepOff, 8, "macro MSTEP")) {
            return FALSE;
        }
        p0 = be32(steps + (size_t)i * 8);
        p1 = be32(steps + (size_t)i * 8 + 4);
        opcode = (uint8_t)(p0 & 0x7Fu);
        if (opcode == 0x10 && !started) {
            started = 1;
            *outSampleId = (uint16_t)((p0 >> 8) & 0xFFFFu);
            /* Whatever the pre-StartSample steps left the volume at is the
             * level the sample BEGINS at, so it is the program's first move --
             * an instantaneous one at time zero. This is not a corner case: it
             * is how MP6 authors a muted fx (seId 1106's macro 0x6a scales the
             * volume to 0 from orgVolume in the step immediately before its
             * StartSample), and skipping it is exactly what made that SE a
             * full-scale beep in this port. A level that never moved off full
             * scale emits nothing, so those voices stay bit-identical. */
            if (curVol != MP6_MSM_ENV_UNITY) {
                float mTo = 1.0f;
                if (!mp6_msm_env_mul_from_vol(curVol, &mTo)) {
                    envUsable = 0;
                    sc.envWhy |= MP6_ENVWHY_UNMODELLED;
                } else {
                    Mp6MsmEnvSeg *seg = &env.seg[env.segCount++];
                    seg->startMs = 0;
                    seg->durMs = 0;
                    seg->from = 1.0f;
                    seg->to = mTo;
                }
            }
        }
        if (opcode == 0x0 || opcode == 0x1) { /* EndOfMacro / Stop -> voiceFree() */
            if (started && lifeUsable) sc.lifeMs = lifeMs;
            break;
        }
        if (opcode == 0x4 || opcode == 0x7) {
            uint32_t w = (p1 >> 16) & 0xFFFFu;
            /* mcmdWait's own "random duration" bit: the real wait is
             * sndRand() % w, so the authored number is an UPPER bound, not
             * the duration. Recorded, never silently trusted. */
            int randWait = ((p0 >> 16) & 1u) != 0;
            if (randWait) sc.why |= MP6_LIFEWHY_RANDWAIT;
            if (w == 0xFFFFu) {
                lifeUsable = 0; /* held -- resumed by keyoff and/or sample end */
                sc.why |= MP6_LIFEWHY_KEYOFFWAIT;
                if ((p0 >> 8) & 1u) sc.why |= MP6_LIFEWHY_WAITKEYOFF;
                if (((p0 >> 24) & 1u) && started) sc.smpEnd = 1;
                envTimeKnown = 0; /* resumes at an unknowable moment */
            } else if (opcode == 0x7 || ((p1 >> 8) & 1u)) {
                if (started) lifeMs += w;
                /* For the ENVELOPE's clock this is only usable when the wait
                 * is (a) a plain relative one -- mcmdWait's ((u8)para[1] & 1)
                 * bit re-bases the deadline on macStartTime instead of adding
                 * to it -- and (b) not the random variant. A zero duration is
                 * fine: mcmdWait returns without waiting at all. */
                if (started) {
                    if (randWait || (p1 & 1u)) envTimeKnown = 0;
                    else envMs += w;
                }
            } else {
                lifeUsable = 0; /* tempo-scaled tick wait -- not knowable here */
                sc.why |= MP6_LIFEWHY_TICKWAIT;
                if (started) sc.ticks += w;
                envTimeKnown = 0;
            }
        } else if (opcode == 0x2 || opcode == 0x3 || opcode == 0x5 ||
                   opcode == 0x6 || opcode == 0x8 || opcode == 0xA ||
                   opcode == 0x13 || opcode == 0x24 || opcode == 0x25) {
            lifeUsable = 0; /* conditional / loop / goto / call -- not linear */
            sc.why |= MP6_LIFEWHY_BRANCH;
            if (started) {
                /* A backward jump can re-run a volume move at a time this
                 * straight-line reader never visits, so the program is not
                 * merely incomplete past this point -- it is wrong. */
                envUsable = 0;
                sc.envWhy |= MP6_ENVWHY_BRANCH;
            }
        } else if (opcode == 0x11 && started) {
            sc.why |= MP6_LIFEWHY_STOPSAMPLE; /* mcmdStopSample -> hwBreak */
            sc.envWhy |= MP6_ENVWHY_KEYOFF;   /* recorded, NOT a refusal -- see below */
        } else if (opcode == 0x12 && started) {
            sc.why |= MP6_LIFEWHY_KEYOFFCMD;  /* mcmdKeyOff */
            sc.envWhy |= MP6_ENVWHY_KEYOFF;
        } else if (opcode == 0x59) {
            /* mcmdSetKeyGroup -- see MsmFxEntry.keyGroup. */
            sc.keyGroup = (uint8_t)((p0 >> 8) & 0xFFu);
            sc.keyGroupKill = (uint8_t)(((p0 >> 16) & 0xFFu) != 0);
        } else if (opcode == 0x0C || opcode == 0x16 || opcode == 0x21) {
            sc.hasAdsr = 1; /* SetADSR / SetADSRFromCtrl / ScaleVolumeDLS */
            envUsable = 0;  /* a real ADSR state machine, not a linear move */
            sc.envWhy |= MP6_ENVWHY_ADSR;
        } else if (opcode == 0x0D || opcode == 0x0F || opcode == 0x14) {
            uint32_t vFrom = 0, vTo = 0, durMs = 0;
            sc.envWhy |= MP6_ENVWHY_SAW_VOLOP;
            if (!mp6_msm_env_vol_step(opcode, p0, p1, curVol, MP6_MSM_ENV_UNITY,
                                      &vFrom, &vTo, &durMs)) {
                envUsable = 0;
                sc.envWhy |= MP6_ENVWHY_UNMODELLED;
            } else if (!started) {
                /* Before the sample exists an instantaneous set only moves the
                 * running level, which the StartSample step above then emits
                 * as the program's move at time zero; a RAMP would still be
                 * moving when the sample begins, which is not expressible as
                 * a segment measured from the StartSample. */
                if (durMs != 0) {
                    envUsable = 0;
                    sc.envWhy |= MP6_ENVWHY_PRESTART;
                } else {
                    curVol = vTo;
                }
            } else if (!envTimeKnown) {
                envUsable = 0;
                sc.envWhy |= MP6_ENVWHY_TIMEUNKNOWN;
            } else if (env.segCount >= MP6_MSM_SE_ENV_MAX_SEGS) {
                envUsable = 0;
                sc.envWhy |= MP6_ENVWHY_TOOMANY;
            } else if (envMs < envPrevEndMs) {
                /* The engine would restart this move from wherever the
                 * previous ramp had actually got to (synth.c keeps ramping
                 * envCurrent while the macro runs on), a value this reader
                 * does not compute. */
                envUsable = 0;
                sc.envWhy |= MP6_ENVWHY_OVERLAP;
            } else {
                float mFrom = 1.0f, mTo = 1.0f;
                if (!mp6_msm_env_mul_from_vol(vFrom, &mFrom) ||
                    !mp6_msm_env_mul_from_vol(vTo, &mTo)) {
                    envUsable = 0;
                    sc.envWhy |= MP6_ENVWHY_UNMODELLED;
                } else {
                    Mp6MsmEnvSeg *seg = &env.seg[env.segCount++];
                    seg->startMs = envMs;
                    seg->durMs = durMs;
                    seg->from = mFrom;
                    seg->to = mTo;
                    envPrevEndMs = envMs + durMs;
                    curVol = vTo;
                }
            }
        }
    }
    /* Falling out of the 64-step window without an explicit terminator is the
     * same "no duration, but the sample is real" verdict as running past the
     * blob: the loop's own exits set NOEND / lifeMs, this only covers running
     * the counter out. */
    if (i >= 64) sc.why |= MP6_LIFEWHY_NOEND;
    if (envUsable && env.segCount > 0) sc.env = env;
    if (outLife) *outLife = sc;
    return started ? TRUE : FALSE;
}

/* Full object-id dispatch for an FX_TAB.macro field -- the id's top 2
 * bits select the object TABLE, exactly src/musyx/runtime/synth.c's own
 * dispatch (StartLayer's `switch (l->id & 0xC000)`: 0 -> macStart, 0x4000
 * -> StartKeymap, 0x8000 -> StartLayer):
 *   0x0000: a plain macro -- resolve its first StartSample.
 *   0x4000: a KEYMAP -- 128 x 8-byte KEYMAP entries (one per MIDI key,
 *     include/musyx/synthdata.h); pick entry [key & 0x7f], apply its
 *     transpose, recurse on ITS id (which may be a macro or a layer --
 *     never another keymap, per StartKeymap's own `!= 0x4000` guard).
 *   0x8000: a LAYER -- u32 num + num x 12-byte LAYER rows (id/keyLow/
 *     keyHigh/transpose/volume/prioOffset/panning); the real engine
 *     starts EVERY row covering the key (that's what layering means --
 *     e.g. gid 176's layer 0x8021, seId 1180, is a 2-row stack of
 *     samples 1357+1358). This port resolves the FIRST covering row only
 *     -- one voice per SE, matching msmSePlay's existing single-voice
 *     shape -- and logs the rows it skips, loudly, so the simplification
 *     is visible per-load.
 * `key` starts as FX_TAB.key. depth guards recursion (real data here is
 * at most layer->macro; keymap->layer->macro is the deepest legal chain). */
static BOOL resolve_first_sample(MsmSeGroup *grp, uint16_t objId, int key, int depth,
                                 uint16_t *outSampleId, MsmMacroScan *outLife)
{
    if (outLife) memset(outLife, 0, sizeof(*outLife));
    if (depth <= 0) return FALSE;
    if (key < 0) key = 0;
    if (key > 127) key = 127;

    switch (objId & 0xC000) {
    case 0x0000:
        return find_macro_first_sample(grp, objId, outSampleId, outLife);

    case 0x4000: { /* KEYMAP */
        const uint8_t *m = pool_find_node(grp, 8 /* POOL_DATA.keymapOff */, objId);
        const uint8_t *entry;
        uint16_t subId;
        int8_t transpose;
        if (!m) return FALSE;
        entry = m + 8 + (size_t)(key & 0x7f) * 8;
        if (!group_span(grp, (uint64_t)(entry - grp->blob), 8, "KEYMAP entry")) return FALSE;
        subId = (uint16_t)be16(entry + 0);
        transpose = (int8_t)entry[2];
        if (subId == 0xFFFF || (subId & 0xC000) == 0x4000) return FALSE; /* StartKeymap's own guard */
        return resolve_first_sample(grp, subId, key + transpose, depth - 1, outSampleId,
                                    outLife);
    }

    case 0x8000: { /* LAYER */
        const uint8_t *m = pool_find_node(grp, 0xC /* POOL_DATA.layerOff */, objId);
        uint32_t num, i;
        int matched = 0;
        BOOL resolved = FALSE;
        if (!m) return FALSE;
        if (!group_span(grp, (uint64_t)(m - grp->blob), 12, "LAYER header")) return FALSE;
        num = be32(m + 8);
        if (num > 128) {
            grp->parseError = 1;
            return FALSE; /* implausible -- refuse to walk garbage */
        }
        if (!group_span(grp, (uint64_t)(m - grp->blob) + 12,
                        (uint64_t)num * 12, "LAYER rows")) return FALSE;
        for (i = 0; i < num; i++) {
            const uint8_t *row = m + 12 + (size_t)i * 12;
            uint16_t subId = (uint16_t)be16(row + 0);
            uint8_t keyLow = row[2], keyHigh = row[3];
            int8_t transpose = (int8_t)row[4];
            if (subId == 0xFFFF || key < keyLow || key > keyHigh) continue;
            matched++;
            if (!resolved) {
                resolved = resolve_first_sample(grp, subId, key + transpose, depth - 1,
                                               outSampleId, outLife);
            }
        }
        if (matched > 1) {
            printf("[AUDIO] resolve_first_sample: layer %#x covers key %d with %d rows -- "
                   "single-voice port plays the FIRST row only (see the layer scope note)\n",
                   (unsigned)objId, key, matched);
        }
        return resolved;
    }

    default: /* 0xC000 -- no such object table in this format */
        return FALSE;
    }
}

/* Reads a type==1 (FX) GROUP_DATA entry's own FX_DATA table (its union @
 * offset 0x1C is fx.tableOff when type==1) and resolves every FX_TAB
 * entry's sample via find_macro_first_sample, appending each successful
 * resolution to the OWNING GROUP's fx index (per-group, so unload takes
 * exactly its own entries with it). */
static void resolve_fx_table(MsmSeGroup *grp, uint32_t groupDataOff)
{
    const uint8_t *prjBase;
    const uint8_t *g;
    uint16_t type;
    uint32_t fxTableOff;
    const uint8_t *fxBase;
    uint16_t fxNum, i;

    if (!group_span(grp, grp->projOfs, 1, "project base") ||
        !group_span(grp, (uint64_t)grp->projOfs + groupDataOff, 0x28, "FX GROUP_DATA")) return;
    prjBase = grp->blob + grp->projOfs;
    g = prjBase + groupDataOff;
    type = (uint16_t)be16(g + 6);
    if (type != 1) return; /* song/sequencer group, not an FX group -- no FX_DATA to read */
    fxTableOff = be32(g + 0x1C);
    if (!group_span(grp, (uint64_t)grp->projOfs + fxTableOff, 4, "FX_DATA header")) return;
    fxBase = prjBase + fxTableOff;
    fxNum = (uint16_t)be16(fxBase + 0);
    if (!group_span(grp, (uint64_t)grp->projOfs + fxTableOff + 4,
                    (uint64_t)fxNum * 10, "FX_DATA entries")) return;
    for (i = 0; i < fxNum; i++) {
        const uint8_t *e = fxBase + 4 + (size_t)i * 10;
        uint16_t fid = (uint16_t)be16(e + 0);
        uint16_t objId = (uint16_t)be16(e + 2); /* macro OR keymap/layer -- see resolve_first_sample */
        uint8_t key = e[8];                     /* FX_TAB.key -- selects keymap entries/layer rows */
        uint16_t sampleId;
        MsmMacroScan life;
        memset(&life, 0, sizeof(life));
        if (resolve_first_sample(grp, objId, key, 4, &sampleId, &life)) {
            if (!add_fx_entry(grp, fid, sampleId, &life)) return;
        }
    }
}

/* A/B switch for the authored VOLUME ENVELOPE, diagnostics only:
 * MP6_AUDIO_NO_MACRO_ENV=1 restores the pre-fix behavior (flat gain for the
 * whole voice) in the SAME binary, so "before" and "after" are one build and
 * one method. Same shape and same reason as MP6_AUDIO_NO_MACRO_LIFE below.
 * Latched on first use: msmSePlay reads it per start, and a value that changed
 * mid-run would make two voices of the same seId disagree. */
static int mp6_se_macro_env_disabled(void)
{
    static int s_disabled = -1;
    if (s_disabled < 0) {
        const char *e = getenv("MP6_AUDIO_NO_MACRO_ENV");
        s_disabled = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return s_disabled;
}

/* Resolve an fx's compiled envelope onto a voice about to start, or leave
 * `out` neutral (segCount 0 -- the mixer never touches that voice's gain).
 *
 * THE LOOP GATE is the load-bearing part. The mixer reads the envelope at the
 * voice's SAMPLE INDEX, which is only a faithful clock for elapsed time while
 * the index advances monotonically from zero; a voice that still LOOPS rewinds
 * its index on every wrap and would replay a timed move forever. So a TIMED
 * program is installed only once the voice is a one-shot -- which is usually
 * already true here, because a macro linear enough to compile a timed program
 * is usually also linear enough for mp6_se_authored_note_frames to have
 * flattened its loop, and that is exactly why msmSePlay calls this AFTER the
 * flattening rather than before.
 *
 * A CONSTANT program (mp6_msm_env_is_constant) has no timed move to replay and
 * is therefore installed on looping voices too. That is not a nicety: MP6's
 * muted fx are authored precisely that way -- one instantaneous ScaleVolume
 * before the StartSample, then a loop-flagged sample held until keyoff -- so
 * gating them out would leave the loudest wrong sound in the bank untouched. */
static void mp6_se_install_env(const Mp6MsmSeEnv *env, const MsmSampleInfo *info,
                               int loopMode, Mp6MsmEnvPlan *out)
{
    uint32_t rate;
    memset(out, 0, sizeof(*out));
    if (mp6_se_macro_env_disabled() || env == NULL || env->segCount == 0) return;
    if (loopMode != 0 && !mp6_msm_env_is_constant(env)) return;
    rate = info->sampleRateHz ? info->sampleRateHz : MP6_MSM_OUT_RATE;
    if (!mp6_msm_env_plan(env, rate, out)) memset(out, 0, sizeof(*out));
}

/* THE AUTHORED NOTE LENGTH, in the sample's own native frames, or 0 for
 * "no cap -- play the sample/loop exactly as before".
 *
 * Non-zero only for a LOOP-FLAGGED sample whose macro carries a usable
 * linear duration (MsmFxEntry.lifeMs): a one-shot sample already ends at
 * its own length, and a keyoff-held macro really does sound until the game
 * calls msmSeStop. msmSePlay uses this to flatten the loop into a finite
 * one-shot; the savestate position validator uses it to compute the same
 * bound, so the two can never disagree about how long a voice may be. */
static uint32_t mp6_se_authored_note_frames(const MsmSampleInfo *info, uint32_t lifeMs,
                                            int loopMode, uint32_t loopEndFrame)
{
    uint32_t rate;
    uint64_t frames;
    /* A/B switch, diagnostics only: MP6_AUDIO_NO_MACRO_LIFE=1 restores the
     * pre-fix behavior (loop until the game stops it) in the SAME build, so
     * "before" and "after" can be measured with one binary and one method. */
    static int s_disabled = -1;
    if (s_disabled < 0) {
        const char *e = getenv("MP6_AUDIO_NO_MACRO_LIFE");
        s_disabled = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    if (s_disabled) return 0;

    (void)loopEndFrame;
    if (loopMode <= 0 || lifeMs == 0 || info == NULL) return 0;
    rate = info->sampleRateHz ? info->sampleRateHz : MP6_MSM_OUT_RATE;
    frames = ((uint64_t)lifeMs * rate + 999u) / 1000u;
    if (frames == 0 || !mp6_msm_decode_budget_valid(frames, 4u)) return 0;
    return (uint32_t)frames;
}

/* SDIR_DATA linear list @ blob+sdirOfs, terminated by id==0xffff. */
static BOOL lookup_sdir(MsmSeGroup *grp, uint16_t sampleId, MsmSampleInfo *out)
{
    uint32_t off = 0;

    for (;;) {
        uint64_t abs = (uint64_t)grp->sdirOfs + off;
        const uint8_t *s;
        if (!group_span(grp, abs, 2, "SDIR terminator/entry")) return FALSE;
        s = grp->blob + grp->sdirOfs;
        uint16_t id = (uint16_t)be16(s + off + 0);
        if (id == 0xFFFFu) return FALSE;
        if (!group_span(grp, abs, 32, "SDIR entry")) return FALSE;
        if (id == sampleId) {
            uint32_t hdrInfo = be32(s + off + 0xC);
            uint32_t hdrLength = be32(s + off + 0x10);
            out->offset = be32(s + off + 4);
            out->length = hdrLength & 0xFFFFFFu; /* PCM samples -- see MsmSampleInfo */
            out->compType = (uint8_t)(hdrLength >> 24);
            out->coefTableRelOfs = be32(s + off + 0x1C); /* SDIR_DATA.extraData */
            out->sampleRateHz = hdrInfo & 0xFFFFu;
            out->loopStart = be32(s + off + 0x14);  /* SAMPLE_HEADER.loopOffset */
            out->loopLength = be32(s + off + 0x18); /* SAMPLE_HEADER.loopLength */
            return TRUE;
        }
        if (off > UINT32_MAX - 32) {
            grp->parseError = 1;
            return FALSE;
        }
        off += 32;
    }
}

/* Validate every authored SDIR entry while the group is loaded, not merely
 * the first sample a later msmSePlay happens to request. This keeps corrupt
 * offsets in dormant/rare SFX from surviving initialization and turning into
 * a delayed out-of-bounds read. Unsupported compression types are still
 * allowed (msmSePlay reports them as unsupported); their codec-specific byte
 * sizing is unknown here, so only the common 32-byte directory entry is
 * checked for those. */
static BOOL validate_sdir(MsmSeGroup *grp)
{
    uint32_t off = 0;

    for (;;) {
        uint64_t abs = (uint64_t)grp->sdirOfs + off;
        const uint8_t *s;
        uint16_t id;
        uint32_t hdrLength;
        uint32_t length;
        uint8_t compType;

        if (!group_span(grp, abs, 2, "SDIR terminator/entry")) return FALSE;
        s = grp->blob + (size_t)abs;
        id = (uint16_t)be16(s + 0);
        if (id == 0xFFFFu) return TRUE;
        if (!group_span(grp, abs, 32, "SDIR entry")) return FALSE;

        hdrLength = be32(s + 0x10);
        length = hdrLength & 0xFFFFFFu;
        compType = (uint8_t)(hdrLength >> 24);
        if (compType == 0) {
            uint32_t sampleOff = be32(s + 4);
            uint32_t coefRel = be32(s + 0x1C);
            uint32_t adpcmFrames;
            uint32_t byteLen;
            uint32_t loopEnd;
            uint64_t coefOff;

            if (length == 0) {
                fprintf(stderr, "[AUDIO] malformed .msm group idx=%d gid=%u: "
                                "SDIR sample %u has zero length\n",
                        grp->grpIdx, (unsigned)grp->gid, (unsigned)id);
                grp->parseError = 1;
                return FALSE;
            }
            adpcmFrames = (length + 13u) / 14u;
            byteLen = adpcmFrames * 8u; /* length is 24-bit, so this cannot overflow */
            if (!mp6_msm_span_valid_u32(grp->sampPoolSize, sampleOff, byteLen)) {
                fprintf(stderr, "[AUDIO] malformed .msm group idx=%d gid=%u: "
                                "SDIR sample %u span %#x+%#x escapes %#x-byte pool\n",
                        grp->grpIdx, (unsigned)grp->gid, (unsigned)id,
                        (unsigned)sampleOff, (unsigned)byteLen,
                        (unsigned)grp->sampPoolSize);
                grp->parseError = 1;
                return FALSE;
            }
            coefOff = (uint64_t)grp->sdirOfs + coefRel;
            if (!group_span(grp, coefOff, 40, "ADPCM coefficient block")) return FALSE;
            if (be16(grp->blob + (size_t)coefOff) != 8u) {
                fprintf(stderr, "[AUDIO] malformed .msm group idx=%d gid=%u: "
                                "SDIR sample %u coefficient count is not 8\n",
                        grp->grpIdx, (unsigned)grp->gid, (unsigned)id);
                grp->parseError = 1;
                return FALSE;
            }
            if (mp6_msm_loop_bounds(length, be32(s + 0x14), be32(s + 0x18),
                                    &loopEnd) < 0) {
                fprintf(stderr, "[AUDIO] malformed .msm group idx=%d gid=%u: "
                                "SDIR sample %u has invalid loop bounds\n",
                        grp->grpIdx, (unsigned)grp->gid, (unsigned)id);
                grp->parseError = 1;
                return FALSE;
            }
        }
        off += 32u;
    }
}

/* =======================================================================
 * DYNAMIC GROUPS -- the load/unload core the game-facing
 * msmSysLoadGroup/msmSysDelGroupAll/... API (further below) and the
 * init-time base-group loads both run through.
 *
 * GAME-SIDE FLOW this implements (matching the real decomp code exactly):
 * game/audio.c's HuAudSndGrpSetSet(grpSet) does msmSysDelGroupAll() ->
 * msmSysGetSampSize(grpSet) -> HuMemDirectMalloc a STAGING buffer ->
 * msmSysLoadGroup(grpSet, buf, FALSE) -> HuMemDirectFree (the buffer is a
 * DVD->ARAM staging area on real hardware, freed right after the load --
 * this port reads straight from the .msm into its own malloc'd blobs and
 * ignores `buf` entirely, so the game's malloc/free round-trip is
 * preserved but never dereferenced). Callers: src/REL/bootDll/boot.c:
 * 167/312 (MSM_GRP_MENU at boot -- BEFORE OpeningExec, which is what
 * makes the opening-storybook seIds 1200..1204 = gid 176 resolve) and
 * game/objmain.c's omWatchOverlayProc -> HuAudDllSndGrpSet(overlay) on
 * every scene/overlay switch (openingdll/mdseldll -> MSM_GRP_MENU=9 ->
 * gid 176, fileseldll -> MSM_GRP_FILESEL=8 -> gid 184 -- seIds
 * 1163/1164/1167 are gid 184, 1180/1188..1194/1200..1204 are gid 176).
 *
 * grpId UNIT: a grpInfo-table INDEX (include/msm_grp.h's MSM_GRP_*), NOT a
 * gid and NOT a "set" id -- this disc's grpSet table is EMPTY
 * (MSM_HEADER.grpSetSize == 0), and the real msmSysLoadGroup indexes
 * sys.grpInfo[grpId] directly (src/msm/msmsys.c).
 *
 * WHAT LOADING MEANS HERE: malloc + read the group's metadata blob
 * (GRP_HEAD/GROUP_DATA/FX_DATA/POOL macros/SDIR -- everything msmSePlay's
 * existing resolution walks), resolve its FX_DATA into the group's OWN
 * fx->sample index, and record where its sample pool lives in the .msm
 * (samples themselves stay on disk; msmSePlay reads + decodes per play,
 * exactly as it always has for base groups). Unloading frees the blob +
 * fx index -- nothing else in the process references them (voices carry
 * their own decoded PCM copies), enforced by g_grpLock (see its comment).
 *
 * The real engine's grpLoadMode/stack-A-B machinery (msmSysLoadGroupSub's
 * pop/re-push dance) exists to manage a FIXED ARAM window; this port has
 * flat host RAM, so both modes behave identically here: find a free slot,
 * load, done. The shipped game boots into MSM_GROUP_LOAD_AUTO (boot.c:311
 * msmSysSetGroupLoadMode(TRUE)) -- the real engine's own simple
 * find-a-free-slot path, i.e. the semantics below match the mode the game
 * actually runs in, not just approximate it.
 * ======================================================================= */

/* g_grpLock must be held. Returns the loaded group owning `gid`, or NULL. */
static MsmSeGroup *se_group_find_by_gid(uint16_t gid)
{
    int i;
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (g_seGroups[i].inUse && g_seGroups[i].gid == gid) return &g_seGroups[i];
    }
    return NULL;
}

/* g_grpLock must be held. Formats "gid,gid,..." of every loaded group into
 * buf (for the msmSePlay unresolved-id diagnostic + load/unload logs). */
static void se_group_loaded_gids_str(char *buf, size_t bufSize)
{
    int i;
    size_t used = 0;
    buf[0] = '\0';
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (!g_seGroups[i].inUse) continue;
        used += (size_t)snprintf(buf + used, bufSize - used, "%s%u%s",
                                 used ? "," : "", (unsigned)g_seGroups[i].gid,
                                 g_seGroups[i].baseGrpF ? "*" : "");
        if (used >= bufSize - 8) break;
    }
}

/* Every malloc'd host metadata byte one loaded group owns: its data blob, its
 * fx index, and its sparse volume-envelope rows -- i.e. exactly what
 * se_group_unload frees. One helper because three call sites report this
 * number (the load log, msmSysDelGroupAll's freed-bytes total, and
 * msmSysDelGroupBase's) and all three must agree; when the fx entry changed
 * size they were three separate expressions to keep in step. */
static uint32_t se_group_bytes(const MsmSeGroup *G)
{
    return G->blobSize +
           (uint32_t)G->fxCap * (uint32_t)sizeof(MsmFxEntry) +
           (uint32_t)G->fxEnvCap * (uint32_t)sizeof(MsmFxEnvRow);
}

/* g_grpLock must be held. Totals for the memory-discipline logs: count +
 * malloc'd metadata bytes of loaded groups. */
static void se_group_totals(int *outCount, uint32_t *outBytes)
{
    int i, n = 0;
    uint32_t b = 0;
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (!g_seGroups[i].inUse) continue;
        n++;
        b += se_group_bytes(&g_seGroups[i]);
    }
    *outCount = n;
    *outBytes = b;
}

/* g_grpLock must be held. Frees everything the slot owns. */
static void se_group_unload(MsmSeGroup *G)
{
    free(G->blob);
    free(G->fx);
    free(G->fxEnv);
    memset(G, 0, sizeof(*G));
}

/* Materialize one grpInfo entry into caller-owned storage without publishing
 * it in g_seGroups.  Runtime loads use this and then publish a slot; savestate
 * preflight uses the same parser on a temporary group to validate an active
 * voice's exact sample/loop bounds even when its captured scene group is not
 * resident in the later live process. */
static s32 se_group_materialize(MsmSeGroup *G, int grpIdx, int baseGrpF,
                                int dynBaseF, const char *why)
{
    const MsmGrpInfo *gi;
    uint32_t groupDataOff;

    if (G == NULL || grpIdx <= 0 || grpIdx >= g_grpInfoCount) return MP6_MSM_ERR_64;
    gi = &g_grpInfo[grpIdx];
    if (gi->dataSize < 12 ||
        (uint64_t)g_msmGrpDataOfs + gi->dataOfs > UINT32_MAX ||
        !mp6_msm_span_valid_u32(g_msmFileSize,
                                (uint32_t)((uint64_t)g_msmGrpDataOfs + gi->dataOfs),
                                gi->dataSize)) {
        fprintf(stderr, "[AUDIO] se_group_materialize(%s): grpIdx=%d gid=%u has an invalid data blob\n",
                why, grpIdx, (unsigned)gi->gid);
        return MSM_ERR_INVALIDFILE;
    }

    memset(G, 0, sizeof(*G));
    G->blob = (uint8_t *)malloc(gi->dataSize);
    if (!G->blob) return MSM_ERR_OUTOFMEM;
    if (!msm_read_range((uint32_t)((uint64_t)g_msmGrpDataOfs + gi->dataOfs),
                        gi->dataSize, G->blob)) {
        se_group_unload(G);
        return MSM_ERR_READFAIL;
    }
    G->gid = gi->gid;
    G->grpIdx = grpIdx;
    G->blobSize = gi->dataSize;
    G->poolOfs = be32(G->blob + 0);
    G->projOfs = be32(G->blob + 4);
    G->sdirOfs = be32(G->blob + 8);
    G->sampPoolFileOfs = g_msmSampOfsHdr + gi->sampOfs;
    G->sampPoolSize = gi->sampSize;
    G->baseGrpF = baseGrpF;
    G->dynBaseF = dynBaseF;

    if (G->poolOfs < 12 || G->projOfs < 12 || G->sdirOfs < 12 ||
        !group_span(G, G->poolOfs, 16, "POOL_DATA") ||
        !group_span(G, G->projOfs, 0x28, "project GROUP_DATA") ||
        !group_span(G, G->sdirOfs, 2, "SDIR table") ||
        !validate_sdir(G)) {
        se_group_unload(G);
        return MSM_ERR_INVALIDFILE;
    }
    if (find_group_data(G, gi->gid, &groupDataOff)) {
        resolve_fx_table(G, groupDataOff);
    }
    if (G->parseError) {
        se_group_unload(G);
        return MSM_ERR_INVALIDFILE;
    }
    G->inUse = 1;
    return 0;
}

/* g_grpLock must be held. Loads grpInfo[grpIdx] into a free slot (dedup'd
 * by gid, matching the real msmSysCheckLoadGroupID). Returns 0 or a real
 * MSM_ERR_* value. `why` tags the log line with the caller. */
static s32 se_group_load(int grpIdx, int baseGrpF, int dynBaseF, const char *why)
{
    const MsmGrpInfo *gi;
    MsmSeGroup *G;
    MsmSeGroup *existing;
    s32 materializeResult;
    int slot, count;
    uint32_t bytes;

    if (grpIdx <= 0 || grpIdx >= g_grpInfoCount) {
        /* index 0 is the reserved/sentinel grpInfo slot on real hardware too
         * (src/msm/msmsys.c's msmSysLoadGroupBase rejects grpId<1) */
        fprintf(stderr, "[AUDIO] se_group_load(%s): grpIdx=%d out of range (1..%d) -- MSM_ERR_64\n",
                why, grpIdx, g_grpInfoCount - 1);
        return MP6_MSM_ERR_64;
    }
    gi = &g_grpInfo[grpIdx];

    existing = se_group_find_by_gid(gi->gid);
    if (existing) {
        /* Already loaded -- the real engine's msmSysCheckLoadGroupID returns
         * success without reloading. If a plain-loaded group is re-requested
         * as a BASE group, promote the flags (keeps msmSysDelGroupAll from
         * dropping something the game just declared always-resident). */
        if (baseGrpF && !existing->baseGrpF) {
            existing->baseGrpF = 1;
            existing->dynBaseF = dynBaseF;
            printf("[AUDIO] se_group_load(%s): grpIdx=%d gid=%u already loaded -- PROMOTED to base\n",
                   why, grpIdx, (unsigned)gi->gid);
        }
        return 0;
    }

    for (slot = 0; slot < MP6_MSM_MAX_SE_GROUPS; slot++) {
        if (!g_seGroups[slot].inUse) break;
    }
    if (slot == MP6_MSM_MAX_SE_GROUPS) {
        fprintf(stderr, "[AUDIO] se_group_load(%s): all %d group slots busy -- MSM_ERR_STACK_OVERFLOW "
                "(real caps: %d base + stackA/B, see init log)\n", why, MP6_MSM_MAX_SE_GROUPS, g_baseGrpNum);
        return MP6_MSM_ERR_STACK_OVERFLOW;
    }

    G = &g_seGroups[slot];
    materializeResult = se_group_materialize(G, grpIdx, baseGrpF, dynBaseF, why);
    if (materializeResult != 0) return materializeResult;
    G->loadOrder = g_grpLoadCounter++;

    se_group_totals(&count, &bytes);
    printf("[AUDIO] msm group LOADED (%s): grpIdx=%d gid=%u%s blob=%u bytes fx=%d sampPool@%#x+%#x "
           "-- now %d group(s), %u metadata bytes total\n",
           why, grpIdx, (unsigned)G->gid, baseGrpF ? " BASE" : "", (unsigned)G->blobSize,
           G->fxCount, (unsigned)G->sampPoolFileOfs, (unsigned)gi->sampSize, count, (unsigned)bytes);
    return 0;
}

/* Parses MP6_SND.msm (header -> MSM_INFO's baseGrp[] -> SE table -> each
 * base group's own data blob -> its FX_DATA -> per-group fx index). Called
 * once from msmSysInit, alongside the existing .pdt parse. Failure is loud
 * (stderr) but never fatal -- SFX simply stay silent, matching msmSysInit's
 * own existing "never block boot over an optional audio feature" policy. */
/* ---- DISC-WIDE ONE-SHOT/LOOP CENSUS (MP6_AUDIO_SE_CENSUS=1) ------------
 * Opt-in, diagnostic-only, runs once at bank init and touches nothing the
 * game can observe.
 *
 * The question it answers exactly: for EVERY seId on the disc, is the
 * sample the port would play loop-flagged, and does the port have an
 * authored note length to bound it with? A loop-flagged sample with no
 * bound is a voice this port will sound FOREVER until the game happens to
 * call msmSeStop -- the F5 "one-shot loops" class. Rows are attributed to
 * the macro-opcode class that cost us the bound (MP6_LIFEWHY_*), so the
 * inventory is evidence rather than a guess.
 *
 * It walks the whole grpInfo directory through the SAME parser a runtime
 * load uses (se_group_materialize into caller-owned storage -- never
 * published in g_seGroups), so it cannot disagree with what msmSePlay will
 * actually do. */
static void mp6_se_loop_census(void)
{
    int gi, i;
    int totalDefs = 0, resolved = 0, loopFlagged = 0, bounded = 0, unbounded = 0;
    int whyKeyoff = 0, whyTick = 0, whyBranch = 0, whyNoend = 0, whyNone = 0;
    int smpEndOnly = 0, keyoffOnly = 0, bothResume = 0, neitherResume = 0;
    int kgAny = 0, kgKill = 0, kgKeyoff = 0, kgLoopFlagged = 0, kgAdsr = 0;
    const char *e = getenv("MP6_AUDIO_SE_CENSUS");
    if (!(e != NULL && *e != '\0' && *e != '0')) return;

    printf("[SECENSUS] begin -- %d SE defs, %d grpInfo entries\n",
           g_seDefCount, g_grpInfoCount);
    for (gi = 1; gi < g_grpInfoCount; gi++) {
        MsmSeGroup tmp;
        uint16_t gid = g_grpInfo[gi].gid;
        int defsHere = 0;
        if (se_group_materialize(&tmp, gi, 0, 0, "census") != 0) {
            printf("[SECENSUS] grpIdx=%d gid=%u UNPARSEABLE -- skipped\n", gi, (unsigned)gid);
            continue;
        }
        /* An earlier grpInfo entry may already carry this gid (the directory
         * has duplicates); attribute each seId to the FIRST gid match only,
         * which is what se_group_find_by_gid would hand msmSePlay. */
        for (i = 0; i < g_seDefCount; i++) {
            const MsmFxEntry *fx = NULL;
            MsmSampleInfo info;
            uint32_t loopEnd = 0;
            int loopMode, j;
            uint32_t noteFrames;
            if (g_seDefs[i].gid != gid) continue;
            defsHere++;
            totalDefs++;
            for (j = 0; j < tmp.fxCount; j++) {
                if (tmp.fx[j].fxId == g_seDefs[i].fxId) { fx = &tmp.fx[j]; break; }
            }
            if (!fx || !lookup_sdir(&tmp, fx->sampleId, &info) || info.compType != 0) continue;
            resolved++;
            if (fx->keyGroup) {
                kgAny++;
                if (fx->keyGroupKill) kgKill++; else kgKeyoff++;
                if (fx->hasAdsr) kgAdsr++;
                printf("[SECENSUS] KEYGROUP seId=%d gid=%u fxId=%d samp=%d kg=%u kill=%u adsr=%u "
                       "len=%u loopLen=%u\n", i, (unsigned)gid, g_seDefs[i].fxId,
                       (int)fx->sampleId, (unsigned)fx->keyGroup, (unsigned)fx->keyGroupKill,
                       (unsigned)fx->hasAdsr, (unsigned)info.length, (unsigned)info.loopLength);
            }
            loopMode = mp6_msm_loop_bounds(info.length, info.loopStart, info.loopLength, &loopEnd);
            if (loopMode <= 0) continue;
            loopFlagged++;
            if (fx->keyGroup) kgLoopFlagged++;
            noteFrames = mp6_se_authored_note_frames(&info, fx->lifeMs, loopMode, loopEnd);
            if (noteFrames != 0) {
                bounded++;
                continue;
            }
            unbounded++;
            if (fx->lifeWhy & MP6_LIFEWHY_KEYOFFWAIT) {
                int ko = (fx->lifeWhy & MP6_LIFEWHY_WAITKEYOFF) != 0;
                if (fx->lifeSmpEnd && ko) bothResume++;
                else if (fx->lifeSmpEnd) smpEndOnly++;
                else if (ko) keyoffOnly++;
                else neitherResume++;
            }
            if (fx->lifeWhy & MP6_LIFEWHY_KEYOFFWAIT) whyKeyoff++;
            else if (fx->lifeWhy & MP6_LIFEWHY_TICKWAIT) whyTick++;
            else if (fx->lifeWhy & MP6_LIFEWHY_BRANCH) whyBranch++;
            else if (fx->lifeWhy & MP6_LIFEWHY_NOEND) whyNoend++;
            else whyNone++;
            printf("[SECENSUS] UNBOUNDED seId=%d gid=%u fxId=%d samp=%d rate=%uHz "
                   "len=%u loop=%u..%u (%.3fs body, %.3fs loop) lifeMs=%u ticks=%u why=%#x%s%s%s%s%s%s%s\n",
                   i, (unsigned)gid, g_seDefs[i].fxId, (int)fx->sampleId,
                   (unsigned)info.sampleRateHz, (unsigned)info.length,
                   (unsigned)info.loopStart, (unsigned)loopEnd,
                   (double)info.length / (info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE),
                   (double)(loopEnd - info.loopStart) /
                       (info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE),
                   (unsigned)fx->lifeMs, (unsigned)fx->lifeTicks, (unsigned)fx->lifeWhy,
                   (fx->lifeWhy & MP6_LIFEWHY_KEYOFFWAIT) ? " KEYOFFWAIT" : "",
                   (fx->lifeWhy & MP6_LIFEWHY_TICKWAIT)   ? " TICKWAIT"   : "",
                   (fx->lifeWhy & MP6_LIFEWHY_BRANCH)     ? " BRANCH"     : "",
                   (fx->lifeWhy & MP6_LIFEWHY_NOEND)      ? " NOEND"      : "",
                   (fx->lifeWhy & MP6_LIFEWHY_RANDWAIT)   ? " RANDWAIT"   : "",
                   (fx->lifeWhy & MP6_LIFEWHY_STOPSAMPLE) ? " STOPSAMPLE" : "",
                   (fx->lifeWhy & MP6_LIFEWHY_KEYOFFCMD)  ? " KEYOFFCMD"  : "");
            if (fx->lifeWhy & MP6_LIFEWHY_KEYOFFWAIT) {
                printf("[SECENSUS]     resume: keyoff=%d sampleEnd=%d\n",
                       (fx->lifeWhy & MP6_LIFEWHY_WAITKEYOFF) ? 1 : 0, (int)fx->lifeSmpEnd);
            }
        }
        (void)defsHere;
        se_group_unload(&tmp);
    }
    printf("[SECENSUS] done -- defs=%d resolved=%d loopFlagged=%d bounded=%d UNBOUNDED=%d "
           "(keyoffWait=%d tickWait=%d branch=%d noEnd=%d other=%d)\n",
           totalDefs, resolved, loopFlagged, bounded, unbounded,
           whyKeyoff, whyTick, whyBranch, whyNoend, whyNone);
    printf("[SECENSUS] indefinite-Wait resume split -- sampleEndOnly=%d keyoffOnly=%d "
           "both=%d neither=%d\n", smpEndOnly, keyoffOnly, bothResume, neitherResume);
    printf("[SECENSUS] key groups -- seDefs with a SetKeyGroup=%d (kill=%d keyoff=%d, "
           "customADSR=%d, loopFlagged=%d)\n", kgAny, kgKill, kgKeyoff, kgAdsr, kgLoopFlagged);
    fflush(stdout);
}

/* =======================================================================
 * VOLUME-ENVELOPE DIAGNOSTICS -- both opt-in, both read-only, neither
 * reachable unless its own environment variable is set.
 *
 *   MP6_AUDIO_SE_ENV_CENSUS=1            what every seId's macro compiled to
 *   MP6_AUDIO_SE_MACRO_DUMP=<seId>[,...] the raw MSTEP stream behind one
 *
 * They exist because the compiled program is the ONLY place the authored
 * shaping becomes visible before a sample is rendered: an SE that keeps its
 * flat gain and an SE whose envelope is a no-op sound identical, and only the
 * MP6_ENVWHY_* verdict distinguishes "nothing authored" from "authored
 * something this port refuses to guess at".
 * ======================================================================= */
static void mp6_env_why_str(uint16_t why, char *buf, size_t bufSize)
{
    struct { uint16_t bit; const char *name; } names[] = {
        { MP6_ENVWHY_SAW_VOLOP,   "volop" },
        { MP6_ENVWHY_UNMODELLED,  "UNMODELLED" },
        { MP6_ENVWHY_TIMEUNKNOWN, "TIMEUNKNOWN" },
        { MP6_ENVWHY_TOOMANY,     "TOOMANY" },
        { MP6_ENVWHY_OVERLAP,     "OVERLAP" },
        { MP6_ENVWHY_ADSR,        "ADSR" },
        { MP6_ENVWHY_KEYOFF,      "KEYOFF" },
        { MP6_ENVWHY_PRESTART,    "PRESTART" },
        { MP6_ENVWHY_BRANCH,      "BRANCH" }
    };
    size_t used = 0;
    int i;
    if (bufSize == 0) return;
    buf[0] = '\0';
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        size_t n;
        if (!(why & names[i].bit)) continue;
        n = strlen(names[i].name);
        if (used + n + 2 >= bufSize) break;
        if (used) buf[used++] = ' ';
        memcpy(buf + used, names[i].name, n);
        used += n;
        buf[used] = '\0';
    }
}

static void mp6_env_program_str(const Mp6MsmSeEnv *env, char *buf, size_t bufSize)
{
    size_t used = 0;
    int i;
    if (bufSize == 0) return;
    buf[0] = '\0';
    for (i = 0; i < (int)env->segCount && i < MP6_MSM_SE_ENV_MAX_SEGS; i++) {
        int n = snprintf(buf + used, bufSize - used, "%s[%ums +%ums %.3f->%.3f]",
                         used ? " " : "", (unsigned)env->seg[i].startMs,
                         (unsigned)env->seg[i].durMs, (double)env->seg[i].from,
                         (double)env->seg[i].to);
        if (n < 0 || (size_t)n >= bufSize - used) break;
        used += (size_t)n;
    }
}

/* The effective loop mode a voice for this fx would END UP with, i.e. after
 * mp6_se_authored_note_frames has had its chance to flatten the loop. The
 * census must ask exactly what msmSePlay asks, or it would report an envelope
 * as applied that the loop gate then refuses. */
static int mp6_se_effective_loop_mode(const MsmSampleInfo *info, uint32_t lifeMs)
{
    uint32_t loopEnd = 0;
    int loopMode = mp6_msm_loop_bounds(info->length, info->loopStart, info->loopLength,
                                       &loopEnd);
    if (loopMode <= 0) return loopMode;
    return mp6_se_authored_note_frames(info, lifeMs, loopMode, loopEnd) != 0 ? 0 : 1;
}

static void mp6_se_env_census(void)
{
    int gi, i;
    int resolved = 0, sawVolop = 0, compiled = 0, applied = 0, loopBlocked = 0;
    int planRefused = 0;
    int whyUnmodelled = 0, whyTime = 0, whyTooMany = 0, whyOverlap = 0;
    int whyAdsr = 0, whyKeyoff = 0, whyPrestart = 0, whyBranch = 0;
    int envRows = 0;   /* sparse envelope rows ACTUALLY allocated across all 115
                        * groups -- the side table's real bank-wide cost, and the
                        * number `compiled` has to be read against: a row exists per
                        * compiled program, whether or not an SE def points at it */
    const char *e = getenv("MP6_AUDIO_SE_ENV_CENSUS");
    if (!(e != NULL && *e != '\0' && *e != '0')) return;

    printf("[SEENV] begin -- %d SE defs, %d grpInfo entries, maxSegs=%d, disabled=%d, "
           "fxEntry=%u bytes/fx, envRow=%u bytes/COMPILED program\n",
           g_seDefCount, g_grpInfoCount, MP6_MSM_SE_ENV_MAX_SEGS,
           mp6_se_macro_env_disabled(), (unsigned)sizeof(MsmFxEntry),
           (unsigned)sizeof(MsmFxEnvRow));
    for (gi = 1; gi < g_grpInfoCount; gi++) {
        MsmSeGroup tmp;
        uint16_t gid = g_grpInfo[gi].gid;
        if (se_group_materialize(&tmp, gi, 0, 0, "envcensus") != 0) continue;
        for (i = 0; i < g_seDefCount; i++) {
            const MsmFxEntry *fx = NULL;
            const Mp6MsmSeEnv *envp;
            Mp6MsmSeEnv env;
            MsmSampleInfo info;
            Mp6MsmEnvPlan plan;
            int loopMode, j, planned, fxIdx = -1;
            char whyBuf[128], progBuf[256];
            if (g_seDefs[i].gid != gid) continue;
            for (j = 0; j < tmp.fxCount; j++) {
                if (tmp.fx[j].fxId == g_seDefs[i].fxId) { fx = &tmp.fx[j]; fxIdx = j; break; }
            }
            if (!fx || !lookup_sdir(&tmp, fx->sampleId, &info) || info.compType != 0) continue;
            resolved++;
            /* Resolved the way msmSePlay resolves it -- by fx INDEX, with an
             * absent row meaning segCount 0 -- so a census verdict cannot
             * disagree with the program playback would actually install. */
            memset(&env, 0, sizeof(env));
            envp = se_group_fx_env(&tmp, fxIdx);
            if (envp != NULL) env = *envp;
            if (fx->envWhy & MP6_ENVWHY_SAW_VOLOP) sawVolop++;
            if (fx->envWhy & MP6_ENVWHY_UNMODELLED) whyUnmodelled++;
            if (fx->envWhy & MP6_ENVWHY_TIMEUNKNOWN) whyTime++;
            if (fx->envWhy & MP6_ENVWHY_TOOMANY) whyTooMany++;
            if (fx->envWhy & MP6_ENVWHY_OVERLAP) whyOverlap++;
            if (fx->envWhy & MP6_ENVWHY_ADSR) whyAdsr++;
            if (fx->envWhy & MP6_ENVWHY_KEYOFF) whyKeyoff++;
            if (fx->envWhy & MP6_ENVWHY_PRESTART) whyPrestart++;
            if (fx->envWhy & MP6_ENVWHY_BRANCH) whyBranch++;
            /* Print every SE that AUTHORED a volume opcode, compiled or not:
             * a refused one is the interesting row, because its verdict is the
             * only thing distinguishing "nothing authored" from "authored
             * something this port declined to guess at". */
            if (env.segCount == 0 && !(fx->envWhy & MP6_ENVWHY_SAW_VOLOP)) continue;
            if (env.segCount != 0) compiled++;
            loopMode = mp6_se_effective_loop_mode(&info, fx->lifeMs);
            planned = env.segCount != 0 &&
                      (loopMode == 0 || mp6_msm_env_is_constant(&env)) &&
                      mp6_msm_env_plan(&env, info.sampleRateHz ? info.sampleRateHz
                                                              : MP6_MSM_OUT_RATE, &plan);
            if (env.segCount != 0) {
                if (planned) applied++;
                else if (loopMode != 0) loopBlocked++;
                else planRefused++;
            }
            mp6_env_why_str(fx->envWhy, whyBuf, sizeof(whyBuf));
            mp6_env_program_str(&env, progBuf, sizeof(progBuf));
            printf("[SEENV] seId=%d gid=%u fxId=%d samp=%d rate=%uHz len=%u loopLen=%u "
                   "lifeMs=%u segs=%d applied=%d loop=%d why=%#x %s program=%s\n",
                   i, (unsigned)gid, g_seDefs[i].fxId, (int)fx->sampleId,
                   (unsigned)info.sampleRateHz, (unsigned)info.length,
                   (unsigned)info.loopLength, (unsigned)fx->lifeMs,
                   (int)env.segCount, planned, loopMode != 0,
                   (unsigned)fx->envWhy, whyBuf, progBuf);
        }
        envRows += tmp.fxEnvCount;
        se_group_unload(&tmp);
    }
    printf("[SEENV] done -- resolved=%d withVolumeOpcode=%d compiled=%d APPLIED=%d "
           "loopBlocked=%d planRefused=%d envRows=%d (%u bytes bank-wide)\n",
           resolved, sawVolop, compiled, applied, loopBlocked, planRefused, envRows,
           (unsigned)((size_t)envRows * sizeof(MsmFxEnvRow)));
    printf("[SEENV] refusals -- unmodelled=%d timeUnknown=%d tooMany=%d overlap=%d "
           "adsr=%d keyoff=%d preStartRamp=%d branch=%d\n",
           whyUnmodelled, whyTime, whyTooMany, whyOverlap, whyAdsr, whyKeyoff,
           whyPrestart, whyBranch);
    fflush(stdout);
}

/* One macro node's MSTEP stream, decoded far enough to read the volume and
 * timing opcodes by eye. Mirrors resolve_first_sample's own object-id dispatch
 * (0x4000 keymap / 0x8000 layer) so a dump of an SE that reaches its macro
 * through an indirection shows the whole chain rather than "not a macro". */
static void mp6_se_macro_dump_obj(MsmSeGroup *grp, uint16_t objId, int key, int depth)
{
    const uint8_t *m;
    int i;
    if (depth <= 0) { printf("[SEMACRO]   (recursion limit)\n"); return; }
    if (key < 0) key = 0;
    if (key > 127) key = 127;

    switch (objId & 0xC000) {
    case 0x4000: { /* KEYMAP */
        const uint8_t *entry;
        uint16_t subId;
        m = pool_find_node(grp, 8, objId);
        if (!m) { printf("[SEMACRO]   keymap %#x NOT FOUND\n", (unsigned)objId); return; }
        entry = m + 8 + (size_t)(key & 0x7f) * 8;
        if (!group_span(grp, (uint64_t)(entry - grp->blob), 8, "KEYMAP entry")) return;
        subId = (uint16_t)be16(entry + 0);
        printf("[SEMACRO]   keymap %#x key=%d -> %#x transpose=%d\n", (unsigned)objId, key,
               (unsigned)subId, (int)(int8_t)entry[2]);
        if (subId == 0xFFFF) return;
        mp6_se_macro_dump_obj(grp, subId, key + (int8_t)entry[2], depth - 1);
        return;
    }
    case 0x8000: { /* LAYER */
        uint32_t num, r;
        m = pool_find_node(grp, 0xC, objId);
        if (!m) { printf("[SEMACRO]   layer %#x NOT FOUND\n", (unsigned)objId); return; }
        if (!group_span(grp, (uint64_t)(m - grp->blob), 12, "LAYER header")) return;
        num = be32(m + 8);
        if (num > 128) return;
        if (!group_span(grp, (uint64_t)(m - grp->blob) + 12, (uint64_t)num * 12,
                        "LAYER rows")) return;
        for (r = 0; r < num; r++) {
            const uint8_t *row = m + 12 + (size_t)r * 12;
            uint16_t subId = (uint16_t)be16(row + 0);
            if (subId == 0xFFFF || key < row[2] || key > row[3]) continue;
            printf("[SEMACRO]   layer %#x row %u -> %#x keys %u..%u transpose=%d\n",
                   (unsigned)objId, (unsigned)r, (unsigned)subId, (unsigned)row[2],
                   (unsigned)row[3], (int)(int8_t)row[4]);
            mp6_se_macro_dump_obj(grp, subId, key + (int8_t)row[4], depth - 1);
        }
        return;
    }
    case 0xC000:
        printf("[SEMACRO]   object %#x has no table\n", (unsigned)objId);
        return;
    default:
        break;
    }

    m = pool_find_node(grp, 0, objId);
    if (!m) { printf("[SEMACRO]   macro %#x NOT FOUND\n", (unsigned)objId); return; }
    printf("[SEMACRO]   macro %#x steps:\n", (unsigned)objId);
    for (i = 0; i < 64; i++) {
        uint64_t off = (uint64_t)(m + 8 - grp->blob) + (size_t)i * 8;
        uint32_t p0, p1;
        uint8_t op;
        if (off > grp->blobSize || (uint64_t)8 > (uint64_t)grp->blobSize - off) return;
        p0 = be32(grp->blob + (size_t)off);
        p1 = be32(grp->blob + (size_t)off + 4);
        op = (uint8_t)(p0 & 0x7Fu);
        printf("[SEMACRO]     %2d: op=%#04x p0=%08x p1=%08x  b1=%u b2=%u b3=%u  "
               "q0=%u q1=%u time=%u\n",
               i, (unsigned)op, (unsigned)p0, (unsigned)p1,
               (unsigned)((p0 >> 8) & 0xFFu), (unsigned)((p0 >> 16) & 0xFFu),
               (unsigned)((p0 >> 24) & 0xFFu), (unsigned)(p1 & 0xFFu),
               (unsigned)((p1 >> 8) & 0xFFu), (unsigned)((p1 >> 16) & 0xFFFFu));
        if (op == 0x0 || op == 0x1) return;
    }
}

static void mp6_se_macro_dump(void)
{
    char buf[256];
    char *tok, *next = NULL;
    size_t n;
    const char *e = getenv("MP6_AUDIO_SE_MACRO_DUMP");
    if (!(e != NULL && *e != '\0')) return;
    n = strlen(e);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, e, n);
    buf[n] = '\0';

    for (tok = strtok(buf, ","); tok; tok = next) {
        int seId = atoi(tok);
        int gi;
        int found = 0;
        next = strtok(NULL, ",");
        if (seId < 0 || seId >= g_seDefCount) {
            printf("[SEMACRO] seId=%d out of range (0..%d)\n", seId, g_seDefCount - 1);
            continue;
        }
        printf("[SEMACRO] seId=%d gid=%u fxId=%d vol=%d pan=%d\n", seId,
               (unsigned)g_seDefs[seId].gid, g_seDefs[seId].fxId,
               (int)g_seDefs[seId].vol, (int)g_seDefs[seId].pan);
        for (gi = 1; gi < g_grpInfoCount && !found; gi++) {
            MsmSeGroup tmp;
            uint32_t groupDataOff;
            const uint8_t *fxBase;
            uint16_t fxNum, k;
            if (g_grpInfo[gi].gid != g_seDefs[seId].gid) continue;
            if (se_group_materialize(&tmp, gi, 0, 0, "macrodump") != 0) continue;
            if (find_group_data(&tmp, g_grpInfo[gi].gid, &groupDataOff) &&
                group_span(&tmp, (uint64_t)tmp.projOfs + groupDataOff, 0x28, "FX GROUP_DATA")) {
                uint32_t fxTableOff = be32(tmp.blob + tmp.projOfs + groupDataOff + 0x1C);
                if (group_span(&tmp, (uint64_t)tmp.projOfs + fxTableOff, 4, "FX_DATA header")) {
                    fxBase = tmp.blob + tmp.projOfs + fxTableOff;
                    fxNum = (uint16_t)be16(fxBase + 0);
                    if (group_span(&tmp, (uint64_t)tmp.projOfs + fxTableOff + 4,
                                   (uint64_t)fxNum * 10, "FX_DATA entries")) {
                        for (k = 0; k < fxNum; k++) {
                            const uint8_t *fe = fxBase + 4 + (size_t)k * 10;
                            if ((uint16_t)be16(fe + 0) != g_seDefs[seId].fxId) continue;
                            found = 1;
                            printf("[SEMACRO]   fx entry: obj=%#x key=%u (grpIdx=%d gid=%u)\n",
                                   (unsigned)be16(fe + 2), (unsigned)fe[8], gi,
                                   (unsigned)g_grpInfo[gi].gid);
                            mp6_se_macro_dump_obj(&tmp, (uint16_t)be16(fe + 2), fe[8], 4);
                            break;
                        }
                    }
                }
            }
            se_group_unload(&tmp);
        }
        if (!found) printf("[SEMACRO]   no fx entry found for seId=%d\n", seId);
    }
    fflush(stdout);
}

static void msm_se_bank_init(const char *msmPath)
{
    uint8_t hdr[0x60];
    uint8_t infoBuf[64];
    uint32_t infoOfs, infoSize, seOfs, seSize, grpInfoOfs, grpInfoSize;
    uint32_t grpDataOfs, grpDataSize, sampOfsHdr, sampSizeHdr;
    int baseGrpNum, i;
    uint8_t baseGrpIdx[32];

    mp6_grp_lock();
    g_msmReady = 0;
    free(g_seDefs); g_seDefs = NULL; g_seDefCount = 0;
    free(g_grpInfo); g_grpInfo = NULL; g_grpInfoCount = 0;
    g_baseGrpNum = 0;
    g_msmFileSize = 0;
    g_sampSizeMaxBase = g_sampSizeMaxDyn = 0;
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (g_seGroups[i].inUse) se_group_unload(&g_seGroups[i]);
    }
    mp6_grp_unlock();

    if (!msmPath) {
        printf("[AUDIO] msm_se_bank_init: no msmPath given -- SFX stay silent\n");
        return;
    }
    {
        size_t n = strlen(msmPath);
        if (n >= sizeof(g_msmPath)) {
            fprintf(stderr, "[AUDIO] msm_se_bank_init: msmPath too long (%zu bytes) -- SFX stay silent\n", n);
            return;
        }
        memcpy(g_msmPath, msmPath, n + 1);
    }

    if (!audio_file_size(g_msmPath, &g_msmFileSize) || g_msmFileSize < sizeof(hdr)) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: .msm is missing or shorter than its header\n");
        return;
    }

    if (!msm_read_range(0, sizeof(hdr), hdr)) {
        printf("[AUDIO] msm_se_bank_init: couldn't read the .msm header -- SFX stay silent\n");
        return;
    }
    if (be32(hdr + 0) != 0x47534E44u /* "GSND" */) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: bad magic (expected \"GSND\") -- SFX stay silent\n");
        return;
    }
    if (be32(hdr + 4) != 2u) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: unexpected .msm version %u (expected 2) -- SFX stay silent\n",
                (unsigned)be32(hdr + 4));
        return;
    }
    infoOfs    = be32(hdr + 16); infoSize = be32(hdr + 20);
    grpInfoOfs = be32(hdr + 32); grpInfoSize = be32(hdr + 36);
    seOfs      = be32(hdr + 48); seSize      = be32(hdr + 52);
    grpDataOfs = be32(hdr + 56); grpDataSize = be32(hdr + 60);
    sampOfsHdr = be32(hdr + 64); sampSizeHdr = be32(hdr + 68);

    if (infoOfs < sizeof(hdr) ||
        (uint64_t)grpInfoOfs < (uint64_t)infoOfs + infoSize ||
        (uint64_t)seOfs < (uint64_t)grpInfoOfs + grpInfoSize ||
        (uint64_t)grpDataOfs < (uint64_t)seOfs + seSize ||
        (uint64_t)sampOfsHdr < (uint64_t)grpDataOfs + grpDataSize ||
        infoSize < sizeof(infoBuf) ||
        !mp6_msm_span_valid_u32(g_msmFileSize, infoOfs, infoSize) ||
        grpInfoSize < 64 || (grpInfoSize % 32) != 0 || grpInfoSize > (uint32_t)INT_MAX ||
        !mp6_msm_span_valid_u32(g_msmFileSize, grpInfoOfs, grpInfoSize) ||
        seSize == 0 || (seSize % 16) != 0 || seSize > (uint32_t)INT_MAX ||
        !mp6_msm_span_valid_u32(g_msmFileSize, seOfs, seSize) ||
        !mp6_msm_span_valid_u32(g_msmFileSize, grpDataOfs, grpDataSize) ||
        !mp6_msm_span_valid_u32(g_msmFileSize, sampOfsHdr, sampSizeHdr)) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: malformed top-level table span(s) "
                "(file=%u info=%#x+%#x grpInfo=%#x+%#x se=%#x+%#x "
                "grpData=%#x+%#x samp=%#x+%#x) -- SFX stay silent\n",
                (unsigned)g_msmFileSize, (unsigned)infoOfs, (unsigned)infoSize,
                (unsigned)grpInfoOfs, (unsigned)grpInfoSize,
                (unsigned)seOfs, (unsigned)seSize,
                (unsigned)grpDataOfs, (unsigned)grpDataSize,
                (unsigned)sampOfsHdr, (unsigned)sampSizeHdr);
        return;
    }

    if (!msm_read_range(infoOfs, sizeof(infoBuf), infoBuf)) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: failed reading MSM_INFO -- SFX stay silent\n");
        return;
    }
    baseGrpNum = infoBuf[40];
    if (baseGrpNum > (int)sizeof(infoBuf) - 41 || baseGrpNum > (int)sizeof(baseGrpIdx)) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: base-group list count %d escapes MSM_INFO\n",
                baseGrpNum);
        return;
    }
    for (i = 0; i < baseGrpNum; i++) baseGrpIdx[i] = infoBuf[41 + i];
    g_seDefCount = (int)(seSize / 16);
    g_seDefs = (MsmSeDef *)malloc((size_t)g_seDefCount * sizeof(MsmSeDef));
    if (!g_seDefs) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: out of memory allocating %d SE defs\n", g_seDefCount);
        g_seDefCount = 0;
        return;
    }
    {
        uint8_t *raw = (uint8_t *)malloc(seSize);
        if (!raw || !msm_read_range(seOfs, seSize, raw)) {
            fprintf(stderr, "[AUDIO] msm_se_bank_init: failed reading SE table -- SFX stay silent\n");
            free(raw); free(g_seDefs); g_seDefs = NULL; g_seDefCount = 0;
            return;
        }
        for (i = 0; i < g_seDefCount; i++) {
            const uint8_t *e = raw + (size_t)i * 16;
            g_seDefs[i].gid = (uint16_t)be16(e + 0);
            g_seDefs[i].fxId = (uint16_t)be16(e + 2);
            g_seDefs[i].vol = (int8_t)e[4];
            g_seDefs[i].pan = (int8_t)e[5];
        }
        free(raw);
    }

    /* Parse + KEEP the whole grpInfo directory (not just the base
     * entries) -- msmSysLoadGroup needs every entry's layout to load any
     * group on demand -- then route the base groups through the exact same
     * se_group_load path dynamic loads use. */
    {
        uint8_t *giRaw = (uint8_t *)malloc(grpInfoSize);
        int numGroupsTotal = (int)(grpInfoSize / 32);
        int loadedBase = 0;
        int baseCount;
        uint32_t baseBytes;

        if (!giRaw || !msm_read_range(grpInfoOfs, grpInfoSize, giRaw)) {
            fprintf(stderr, "[AUDIO] msm_se_bank_init: failed reading group-info table -- SFX stay silent\n");
            free(giRaw);
            return;
        }
        g_grpInfo = (MsmGrpInfo *)malloc((size_t)numGroupsTotal * sizeof(MsmGrpInfo));
        if (!g_grpInfo) {
            fprintf(stderr, "[AUDIO] msm_se_bank_init: out of memory for %d grpInfo entries -- "
                    "SFX stay silent\n", numGroupsTotal);
            free(giRaw);
            return;
        }
        for (i = 0; i < numGroupsTotal; i++) {
            const uint8_t *gi = giRaw + (size_t)i * 32;
            g_grpInfo[i].gid = (uint16_t)be16(gi + 0);
            g_grpInfo[i].stackNo = (int8_t)gi[2];
            g_grpInfo[i].subGrpId = (int8_t)gi[3];
            g_grpInfo[i].dataOfs = be32(gi + 4);
            g_grpInfo[i].dataSize = be32(gi + 8);
            g_grpInfo[i].sampOfs = be32(gi + 12);
            g_grpInfo[i].sampSize = be32(gi + 16);
            if (g_grpInfo[i].dataSize > (uint32_t)INT_MAX ||
                g_grpInfo[i].sampSize > (uint32_t)INT_MAX ||
                !mp6_msm_span_valid_u32(grpDataSize, g_grpInfo[i].dataOfs,
                                        g_grpInfo[i].dataSize) ||
                !mp6_msm_span_valid_u32(sampSizeHdr, g_grpInfo[i].sampOfs,
                                        g_grpInfo[i].sampSize)) {
                fprintf(stderr, "[AUDIO] msm_se_bank_init: grpInfo[%d] has an out-of-range "
                        "data/sample span -- SFX stay silent\n", i);
                free(giRaw);
                free(g_grpInfo); g_grpInfo = NULL; g_grpInfoCount = 0;
                free(g_seDefs); g_seDefs = NULL; g_seDefCount = 0;
                return;
            }
        }
        free(giRaw);
        g_grpInfoCount = numGroupsTotal;
        g_msmGrpDataOfs = grpDataOfs;
        g_msmSampOfsHdr = sampOfsHdr;
        g_baseGrpNum = baseGrpNum;
        for (i = 0; i < baseGrpNum && i < (int)(sizeof(g_baseGrpIdx) / sizeof(g_baseGrpIdx[0])); i++) {
            if (baseGrpIdx[i] == 0 || baseGrpIdx[i] >= numGroupsTotal) {
                fprintf(stderr, "[AUDIO] msm_se_bank_init: baseGrp[%d]=%u is outside grpInfo\n",
                        i, (unsigned)baseGrpIdx[i]);
                free(g_grpInfo); g_grpInfo = NULL; g_grpInfoCount = 0;
                free(g_seDefs); g_seDefs = NULL; g_seDefCount = 0;
                return;
            }
            g_baseGrpIdx[i] = baseGrpIdx[i];
        }

        /* Real msmSysGroupInit computes both staging-buffer maxima
         * (src/msm/msmsys.c: sys.sampSize = max BASE-group sampSize,
         * sys.sampSizeBase = max over every OTHER group) -- msmSysGetSampSize
         * below reports these honestly so the game's own staging mallocs are
         * sized exactly as on real hardware. */
        {
            int isBase, j;
            for (i = 1; i < numGroupsTotal; i++) {
                isBase = 0;
                for (j = 0; j < baseGrpNum; j++) {
                    if (baseGrpIdx[j] == i) { isBase = 1; break; }
                }
                if (isBase) {
                    if ((s32)g_grpInfo[i].sampSize > g_sampSizeMaxBase)
                        g_sampSizeMaxBase = (s32)g_grpInfo[i].sampSize;
                } else {
                    if ((s32)g_grpInfo[i].sampSize > g_sampSizeMaxDyn)
                        g_sampSizeMaxDyn = (s32)g_grpInfo[i].sampSize;
                }
            }
        }

        mp6_grp_lock();
        for (i = 0; i < baseGrpNum; i++) {
            if (se_group_load(baseGrpIdx[i], /*baseGrpF=*/1, /*dynBaseF=*/0, "init-base") == 0) {
                loadedBase++;
            }
        }
        se_group_totals(&baseCount, &baseBytes);
        g_msmReady = (loadedBase == baseGrpNum);
        if (!g_msmReady) {
            for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
                if (g_seGroups[i].inUse) se_group_unload(&g_seGroups[i]);
            }
        }
        mp6_grp_unlock();

        if (!g_msmReady) {
            fprintf(stderr, "[AUDIO] msm_se_bank_init: rejected bank because only %d/%d "
                    "base groups parsed cleanly -- SFX stay silent\n", loadedBase, baseGrpNum);
            return;
        }

        printf("[AUDIO] msm_se_bank_init: .msm bank loaded -- %d SE defs, %d/%d base group(s) resident "
               "(%u metadata bytes), %d grpInfo entries indexed for DYNAMIC loading (real stack caps "
               "A=%d B=%d), staging maxima base=%#x dyn=%#x -- SFX playback is ACTIVE\n",
               g_seDefCount, loadedBase, baseGrpNum, (unsigned)baseBytes, g_grpInfoCount,
               (int)(int8_t)infoBuf[9], (int)(int8_t)infoBuf[10],
               (unsigned)g_sampSizeMaxBase, (unsigned)g_sampSizeMaxDyn);
    }

    mp6_se_loop_census();
    mp6_se_env_census();
    mp6_se_macro_dump();
}

/* =======================================================================
 * The game-facing dynamic-group API. Semantics mirror src/msm/msmsys.c
 * minus the ARAM stack machinery -- see the "DYNAMIC GROUPS" section
 * comment above.
 * ======================================================================= */

/* Real signature from include/msm.h. `buf` is the game's own STAGING
 * buffer (DVD->ARAM bounce on real hardware) -- this port reads the .msm
 * directly and never touches it; the NULL check is kept because the real
 * engine's is load-bearing (real msmSysLoadGroup returns 0 -- success,
 * no-op -- on NULL, and the game really can pass NULL when its heap
 * malloc fails). `flag` is unused by the real implementation too. */
s32 msmSysLoadGroup(s32 grp, void *buf, BOOL flag)
{
    s32 result;

    printf("[AUDIO] msmSysLoadGroup(grp=%d, buf=%p, flag=%d)\n", (int)grp, buf, (int)flag);
    if (!g_msmReady) {
        printf("[AUDIO] msmSysLoadGroup: no .msm bank loaded on this checkout -- no-op (SFX already "
               "degraded-silent, matching msmSysInit's own never-block-boot policy)\n");
        return 0;
    }
    if (buf == NULL) {
        /* Mirrors the real engine exactly -- and deserves a loud log, since
         * it means the game-side staging malloc failed and real hardware
         * would ALSO have silently skipped this load. */
        fprintf(stderr, "[AUDIO] msmSysLoadGroup: buf==NULL (game-side staging malloc failed?) -- "
                "returning 0 without loading, exactly as the real engine does\n");
        return 0;
    }
    if (grp == 0) {
        /* grpId 0 = "(re)load the base groups" (real msmSysLoadBaseGroup) --
         * this port's base groups are permanently resident, so this is
         * honestly a no-op success (boot.c:309 calls this on the cold-boot
         * path right before HuAudSndGrpSetSet(MSM_GRP_MENU)). */
        printf("[AUDIO] msmSysLoadGroup: grp=0 (base-group reload) -- base groups are permanently "
               "resident in this port, nothing to do\n");
        return 0;
    }

    mp6_grp_lock();
    /* Real msmSysLoadGroupSub co-loads grpInfo[grp].subGrpId first (a
     * second grpInfo INDEX, e.g. a minigame group's shared common bank);
     * dedupe inside se_group_load makes this idempotent. */
    if (grp < g_grpInfoCount && grp > 0 && g_grpInfo[grp].subGrpId > 0) {
        result = se_group_load(g_grpInfo[grp].subGrpId, 0, 0, "msmSysLoadGroup-subGrp");
        if (result != 0) {
            mp6_grp_unlock();
            return result;
        }
    }
    result = se_group_load(grp, 0, 0, "msmSysLoadGroup");
    mp6_grp_unlock();
    return result;
}

s32 msmSysDelGroupAll(void)
{
    int i, removed = 0, count;
    uint32_t freedBytes = 0, bytes;

    if (!g_msmReady) return 0;
    mp6_grp_lock();
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        MsmSeGroup *G = &g_seGroups[i];
        if (G->inUse && !G->baseGrpF) {
            freedBytes += se_group_bytes(G);
            se_group_unload(G);
            removed++;
        }
    }
    se_group_totals(&count, &bytes);
    mp6_grp_unlock();
    printf("[AUDIO] msmSysDelGroupAll: unloaded %d dynamic group(s), freed %u metadata bytes -- "
           "%d group(s) remain (%u bytes)\n", removed, (unsigned)freedBytes, count, (unsigned)bytes);
    return 0;
}

/* Loads a group as an ADDITIONAL always-resident base group (the game's
 * HuAudSndCommonGrpSet path -- per-character voice banks on boards). Real
 * msmSysLoadGroupBase (msmsys.c:551) DelGroupAll()s first, dedupes against
 * the base list, then pushes with baseGrpF=1; mirrored here. NOT exercised
 * by the boot->menu flow. No header prototype exists anywhere (the real
 * game calls it via MWCC implicit declaration); signature matches the
 * decomp definition + the previous generated stub. */
s32 msmSysLoadGroupBase(s32 grpId, void *buf)
{
    s32 result;

    (void)buf; /* staging only, same as msmSysLoadGroup -- never dereferenced */
    printf("[AUDIO] msmSysLoadGroupBase(grpId=%d, buf=%p)\n", (int)grpId, buf);
    if (!g_msmReady) return 0;
    msmSysDelGroupAll();
    mp6_grp_lock();
    result = se_group_load(grpId, /*baseGrpF=*/1, /*dynBaseF=*/1, "msmSysLoadGroupBase");
    mp6_grp_unlock();
    return result;
}

/* Removes dynamically-ADDED base groups (msmSysLoadGroupBase's) -- never
 * the 5 init-time ones. grpNum==0 (or out of range) = all of them;
 * otherwise the `grpNum` most recently loaded, after a DelGroupAll(),
 * mirroring msmsys.c's own LIFO pop over sys.grpLoadId. Board-flow only,
 * like msmSysLoadGroupBase above. */
s32 msmSysDelGroupBase(s32 grpNum)
{
    int i, dynBaseCount = 0, removed = 0;
    uint32_t freedBytes = 0;

    printf("[AUDIO] msmSysDelGroupBase(grpNum=%d)\n", (int)grpNum);
    if (!g_msmReady) return 0;

    mp6_grp_lock();
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (g_seGroups[i].inUse && g_seGroups[i].dynBaseF) dynBaseCount++;
    }
    if (dynBaseCount == 0) {
        mp6_grp_unlock();
        return 0;
    }
    if (grpNum <= 0 || grpNum >= dynBaseCount) grpNum = dynBaseCount;
    mp6_grp_unlock();

    /* Real msmSysDelGroupBase clears the plain dynamic groups first when
     * popping a specific count (msmsys.c:455) -- and DelGroupAll takes the
     * same lock, so call it OUTSIDE ours. */
    msmSysDelGroupAll();

    mp6_grp_lock();
    while (removed < grpNum) {
        MsmSeGroup *newest = NULL;
        for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
            MsmSeGroup *G = &g_seGroups[i];
            if (G->inUse && G->dynBaseF && (!newest || G->loadOrder > newest->loadOrder)) {
                newest = G;
            }
        }
        if (!newest) break;
        freedBytes += se_group_bytes(newest);
        se_group_unload(newest);
        removed++;
    }
    mp6_grp_unlock();
    printf("[AUDIO] msmSysDelGroupBase: unloaded %d added-base group(s), freed %u metadata bytes\n",
           removed, (unsigned)freedBytes);
    return 0;
}

/* Real mapping (msmsys.c:405, names are CONFUSING but transcribed
 * faithfully): baseGrp!=0 -> sys.sampSizeBase == the max sampSize over
 * every NON-base group (the staging size any DYNAMIC load could need --
 * HuAudSndGrpSetSet passes the target grpSet id here, always nonzero);
 * baseGrp==0 -> sys.sampSize == the max over the BASE groups (boot.c:308
 * uses this to stage the grpId-0 base reload). Honest real values so the
 * game's own staging mallocs are sized exactly as on real hardware
 * (0x2b4560 / 0x158400 on this disc -- both well inside the game heaps
 * that real hardware already satisfied). */
s32 msmSysGetSampSize(BOOL baseGrp)
{
    s32 v = baseGrp ? g_sampSizeMaxDyn : g_sampSizeMaxBase;
    printf("[AUDIO] msmSysGetSampSize(baseGrp=%d) -> %#x\n", (int)baseGrp, (unsigned)v);
    return v;
}

/* Recorded + logged only: both real modes exist to manage a FIXED ARAM
 * window (MANUAL juggles two group stacks; boot.c:311 switches the shipped
 * game to AUTO right after the cold-boot base reload). This port's flat
 * host-RAM slot table behaves identically either way. Signature is
 * include/msm.h's (s8 -- the decomp .c says s32; the header wins here,
 * exactly as it did for the generated stub this replaces). */
void msmSysSetGroupLoadMode(s8 mode)
{
    printf("[AUDIO] msmSysSetGroupLoadMode(%d)%s\n", (int)mode,
           mode ? " (AUTO -- the real engine's own simple free-slot path, which is also "
                  "exactly what this port implements)" : " (MANUAL)");
    g_grpLoadMode = mode;
}

/* =======================================================================
 * LEAK-GATE stress hook (MP6_AUDIO_LEAKTEST_STREAM=<streamId>, BOTH build
 * modes) -- proving "buffers allocated once at stream-open, freed at
 * close" holds up under REPEATED open/close cycles, not just a single
 * manual pass. The
 * MP6_AUDIO_SELFTEST_STREAMS hook above plays each requested stream
 * exactly ONCE at init and leaves it looping forever afterward -- great
 * for the WAV-dump/call-map verification, but it would make
 * tools/leakgate.py's own 300-second sampling window trivially pass
 * without ever exercising msmStreamPlay's replay-reuse free or
 * msmStreamStop's own free path even once after startup, which isn't a
 * real leak-gate test at all.
 *
 * This spawns one background thread, opt-in, that loops
 * Play(streamId)/sleep/Stop(chan)/sleep for as long as the process lives
 * -- hundreds of real open->close cycles over a single leakgate.py run,
 * under the SAME g_mixerLock every real call site already uses (no new
 * concurrency design -- this is exactly the same shape as
 * audio_out_sdl.c's own SDL callback thread already calling into this
 * same API concurrently with the game's own thread). Available in BOTH
 * build modes (not `#ifdef MP6_HEADLESS_BUILD`) since --headless is the
 * primary leak-gate target (tools/leakgate.py's own default exe arg) but
 * a windowed run can use it too. No-op unless the env var is set.
 * ======================================================================= */
/* The never-closed thread HANDLE globals live in mp6_host_thread_start
 * (src/host/host_win32.c), which deliberately keeps a never-
 * CloseHandle accounting -- see host.h. Thread procs are plain
 * void(void*). */

static void mp6_leaktest_proc(void *arg)
{
    int streamId = (int)(intptr_t)arg;
    for (;;) {
        int chan = msmStreamPlay(streamId, NULL);
        mp6_host_sleep_ns(150u * 1000000ull); /* was Sleep(150) */
        if (chan >= 0) {
            /* A real, short fade-out (well under the 150ms sleep windows
             * either side, so the fade always completes before this same
             * channel could plausibly be reassigned), not speed=0 -- this
             * stress loop exercises the MP6_FADE_TO_STOP deferred-free
             * path, not just an immediate one. */
            msmStreamStop(chan, 60);
        }
        mp6_host_sleep_ns(150u * 1000000ull); /* was Sleep(150) */
    }
}

/* Analog of the stream stress hook just above -- MP6_AUDIO_LEAKTEST_SE=
 * <seId>, same "background open/close cycle" shape, proving msmSePlay's
 * own decode-buffer allocate + msmSeStop's own free (or, when the selected
 * sample is authored one-shot, the mixer's auto-deactivate-on-finish path)
 * doesn't grow unbounded under repeated real cycles,
 * independent of the pre-existing base-checkout headless RSS noise a
 * plain idle run alone can't distinguish from a real leak. */
static void mp6_se_leaktest_proc(void *arg)
{
    int seId = (int)(intptr_t)arg;
    for (;;) {
        int no = msmSePlay(seId, NULL);
        mp6_host_sleep_ns(150u * 1000000ull); /* was Sleep(150) */
        if (no >= 0) {
            /* Same reasoning as mp6_leaktest_proc's own comment above --
             * exercises the MP6_FADE_TO_STOP deferred-free path, not just
             * an immediate one. */
            msmSeStop(no, 60);
        }
        mp6_host_sleep_ns(150u * 1000000ull); /* was Sleep(150) */
    }
}

/* Analog of the two stress hooks above -- MP6_AUDIO_LEAKTEST_GRPSWAP=
 * "<grpIdx>,<grpIdx>,..." (grpInfo indices, e.g. "8,9" = FILESEL/MENU),
 * same "background open/close cycle" shape, proving a full dynamic-group
 * LOAD (blob malloc + file read + fx-index build) followed by
 * msmSysDelGroupAll's full unload frees everything, hundreds of real
 * cycles per leakgate run -- the direct allocate/free proof for the
 * group machinery, complementing the organic windowed scene-swap gate
 * (boot->fileselect->modeselect). */
static int g_grpSwapIds[8];
static int g_grpSwapIdCount;

static void mp6_grpswap_leaktest_proc(void *arg)
{
    /* Non-NULL staging pointer only -- msmSysLoadGroup never dereferences
     * it (see its own comment), but NULL would no-op the load, exactly as
     * on real hardware. */
    static char s_dummyStaging[16];
    int i;
    (void)arg;
    for (;;) {
        for (i = 0; i < g_grpSwapIdCount; i++) {
            msmSysLoadGroup(g_grpSwapIds[i], s_dummyStaging, FALSE);
            mp6_host_sleep_ns(75u * 1000000ull); /* was Sleep(75) */
            msmSysDelGroupAll();
            mp6_host_sleep_ns(75u * 1000000ull); /* was Sleep(75) */
        }
    }
}

static void mp6_grpswap_leaktest_maybe_start(void)
{
    const char *e = getenv("MP6_AUDIO_LEAKTEST_GRPSWAP");
    char buf[128];
    char *tok, *next = NULL;
    size_t n;

    if (!e || !e[0]) return;
    n = strlen(e);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, e, n);
    buf[n] = '\0';
    tok = strtok(buf, ",");
    while (tok && g_grpSwapIdCount < (int)(sizeof(g_grpSwapIds) / sizeof(g_grpSwapIds[0]))) {
        next = strtok(NULL, ",");
        g_grpSwapIds[g_grpSwapIdCount++] = atoi(tok);
        tok = next;
    }
    if (g_grpSwapIdCount == 0) return;
    printf("[AUDIO] MP6_AUDIO_LEAKTEST_GRPSWAP=\"%s\" -- test-only: spawning a background group "
           "load/unload stress thread (%d id(s)) for leak-gate verification\n",
           e, g_grpSwapIdCount);
    /* mp6_host_thread_start returns 0 or the OS error code (win32:
     * GetLastError()), keeping this failure line byte-identical. */
    {
        int err = mp6_host_thread_start(mp6_grpswap_leaktest_proc, NULL);
        if (err) {
            fprintf(stderr, "[AUDIO] MP6_AUDIO_LEAKTEST_GRPSWAP: failed to start the stress thread "
                    "(GetLastError=%lu)\n", (unsigned long)err);
        }
    }
}

static void mp6_se_leaktest_maybe_start(void)
{
    const char *e = getenv("MP6_AUDIO_LEAKTEST_SE");
    int seId;

    if (!e || !e[0]) return;
    seId = atoi(e);
    printf("[AUDIO] MP6_AUDIO_LEAKTEST_SE=%d -- test-only: spawning a background open/close stress "
           "thread for leak-gate verification\n", seId);
    {
        int err = mp6_host_thread_start(mp6_se_leaktest_proc, (void *)(intptr_t)seId);
        if (err) {
            fprintf(stderr, "[AUDIO] MP6_AUDIO_LEAKTEST_SE: failed to start the stress thread "
                    "(GetLastError=%lu)\n", (unsigned long)err);
        }
    }
}

static void mp6_leaktest_maybe_start(void)
{
    const char *e = getenv("MP6_AUDIO_LEAKTEST_STREAM");
    int streamId;

    if (!e || !e[0]) return;
    streamId = atoi(e);
    printf("[AUDIO] MP6_AUDIO_LEAKTEST_STREAM=%d -- test-only: spawning a background open/close stress "
           "thread for leak-gate verification\n", streamId);
    {
        int err = mp6_host_thread_start(mp6_leaktest_proc, (void *)(intptr_t)streamId);
        if (err) {
            fprintf(stderr, "[AUDIO] MP6_AUDIO_LEAKTEST_STREAM: failed to start the stress thread "
                    "(GetLastError=%lu)\n", (unsigned long)err);
        }
    }
}

/* =======================================================================
 * msmSysInit -- parses the real .pdt directory (see this file's header
 * comment for the format) and brings up the live audio backend, then
 * parses the SEPARATE .msm SE/Mus group bank (init->msmPath, see
 * msm_se_bank_init below).
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

    if (!audio_file_size(g_pdtPath, &g_pdtFileSize) || g_pdtFileSize < sizeof(hdrBuf)) {
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
            g_pdtReady = 1; /* enables get_pack for the validation pass below */
            for (i = 0; i < g_streamMax; i++) {
                PdtPack check;
                uint32_t bytes;
                int channels, ch;
                if (g_packListOfs[i] == 0) continue; /* authored removed ID */
                if (!get_pack(i, &check)) {
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
/* One census pass per GAME tick, on the game thread (MP6_AUDIO_TIMELINE=1).
 * This is the half of the timeline that no game-facing API call can report:
 * a voice that keeps sounding without any new msmSePlay. Snapshot under
 * g_mixerLock, print AFTER unlocking -- printf must never run inside the
 * lock the SDL audio callback needs. */
static void mp6_se_timeline_census(void)
{
    static int s_prevNo[MP6_MSM_MAX_SFX_VOICES];
    static int s_prevSeId[MP6_MSM_MAX_SFX_VOICES];
    static uint32_t s_prevWraps[MP6_MSM_MAX_SFX_VOICES];
    static uint64_t s_prevStart[MP6_MSM_MAX_SFX_VOICES];
    int active[MP6_MSM_MAX_SFX_VOICES];
    int no[MP6_MSM_MAX_SFX_VOICES];
    int seId[MP6_MSM_MAX_SFX_VOICES];
    int loop[MP6_MSM_MAX_SFX_VOICES];
    int firstWrap[MP6_MSM_MAX_SFX_VOICES];
    uint32_t wraps[MP6_MSM_MAX_SFX_VOICES];
    uint64_t start[MP6_MSM_MAX_SFX_VOICES];
    /* Storage above is the capacity; both passes below walk the ACTIVE cap.
     * Slots at or above it can never be allocated, so their s_prev* entries
     * stay 0 forever and walking them would only ever print nothing -- but
     * bounding by the cap keeps the retail per-tick cost, and the retail
     * [SETL] stream, exactly what they were. */
    const int cap = mp6_sfx_voice_cap();
    int i;

    mp6_lock();
    for (i = 0; i < cap; i++) {
        MsmSeVoice *v = &g_sfxVoice[i];
        active[i] = v->active;
        no[i]     = v->no;
        seId[i]   = v->seId;
        loop[i]   = v->loop;
        wraps[i]  = v->tlWraps;
        start[i]  = v->tlStartTick;
        firstWrap[i] = 0;
        if (v->active && v->tlWraps > 0 && !v->tlReported) {
            v->tlReported = 1;
            firstWrap[i] = 1;
        }
    }
    mp6_unlock();

    for (i = 0; i < cap; i++) {
        int gone = (s_prevNo[i] != 0 && (!active[i] || no[i] != s_prevNo[i]));
        if (gone) {
            printf("[SETL] end  tick=%llu slot=%d seNo=%d seId=%d age=%llu ticks wraps=%u\n",
                   (unsigned long long)mp6_tick_count, i, s_prevNo[i], s_prevSeId[i],
                   (unsigned long long)(mp6_tick_count - s_prevStart[i]),
                   (unsigned)s_prevWraps[i]);
        }
        if (firstWrap[i]) {
            printf("[SETL] wrap tick=%llu slot=%d seNo=%d seId=%d FIRST mixer loop-wrap "
                   "at age=%llu ticks -- repeating with NO new msmSePlay\n",
                   (unsigned long long)mp6_tick_count, i, no[i], seId[i],
                   (unsigned long long)(mp6_tick_count - start[i]));
        }
        if (active[i] && loop[i]) {
            uint64_t age = mp6_tick_count - start[i];
            if (age > 0 && (age % 120) == 0) {
                printf("[SETL] hold tick=%llu slot=%d seNo=%d seId=%d STILL LOOPING age=%llu "
                       "ticks wraps=%u\n", (unsigned long long)mp6_tick_count, i, no[i],
                       seId[i], (unsigned long long)age, (unsigned)wraps[i]);
            }
        }
        s_prevNo[i]    = active[i] ? no[i] : 0;
        s_prevSeId[i]  = seId[i];
        s_prevWraps[i] = wraps[i];
        s_prevStart[i] = start[i];
    }
    fflush(stdout);
}

/* Pull-side audio snapshot (include/mp6_diag_probe.h).
 *
 * Same discipline as mp6_se_timeline_census() right above, and for the same
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
            l = c->pcm[idx * MP6_MSM_OUT_CHANNELS + 0];
            r = c->pcm[idx * MP6_MSM_OUT_CHANNELS + 1];
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
    int16_t *tmpL, *tmpR;
    uint32_t f;
    int vol;
    int startPaused;
    int firstWav = 0;
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
    if (!get_pack(streamId, &pack)) return MSM_ERR_REMOVEDID;

    if (streamParam && (streamParam->flag & MSM_STREAMPARAM_CHAN)) {
        chan = streamParam->chan;
        if (chan < 0 || chan >= g_chanMax) return MP6_MSM_ERR_CHANLIMIT;
    } else {
        chan = -1; /* selected atomically under g_mixerLock after decode */
    }

    stereo = (pack.flag & MP6_PACK_FLAG_STEREO) ? 1 : 0;
    {
        uint32_t adpcmFrames = pack_adpcm_frame_count(&pack);
        if (adpcmFrames == 0 || adpcmFrames > (UINT32_MAX / 14)) { /* the /14 guard blocks the
                                                                        frames*14 multiply below
                                                                        from silently wrapping */
            printf("[AUDIO] msmStreamPlay: streamId=%d has zero-length or implausible audio -- "
                   "nothing to play\n", streamId);
            return MSM_ERR_INVALIDFILE;
        }
        frames = adpcmFrames * 14; /* PCM sample count -- see pack_adpcm_frame_count's own
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

        tmpL = (int16_t *)malloc((size_t)frames * sizeof(int16_t));
        tmpR = stereo ? (int16_t *)malloc((size_t)frames * sizeof(int16_t)) : NULL;
        if (!tmpL || (stereo && !tmpR)) {
            free(tmpL); free(tmpR); free(interleaved);
            return MSM_ERR_OUTOFMEM;
        }

        /* decode_substream's own `numFrames` parameter wants the ADPCM
         * frame count, NOT the PCM sample count -- pass adpcmFrames here,
         * never `frames` (mixing these up causes a ~14x heap buffer
         * overflow in tmpL/tmpR -- see this file's own
         * pack_adpcm_frame_count comment). */
        decodeResult = decode_substream(pack.subSampleOfs[0], pack.subCoefIdx[0], adpcmFrames, tmpL);
        if (decodeResult != 0) {
            free(tmpL); free(tmpR); free(interleaved);
            return decodeResult;
        }
        if (stereo) {
            decodeResult = decode_substream(pack.subSampleOfs[1], pack.subCoefIdx[1], adpcmFrames, tmpR);
            if (decodeResult != 0) {
                free(tmpL); free(tmpR); free(interleaved);
                return decodeResult;
            }
        }
    }
    for (f = 0; f < frames; f++) {
        interleaved[f * 2 + 0] = tmpL[f];
        interleaved[f * 2 + 1] = stereo ? tmpR[f] : tmpL[f];
    }
    free(tmpL);
    free(tmpR);

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

static MsmSeVoice *find_sfx_voice_by_no(int seNo)
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
        grp = se_group_find_by_gid(def->gid);
        if (!grp) {
            char loaded[256];
            se_group_loaded_gids_str(loaded, sizeof(loaded));
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
            const Mp6MsmSeEnv *env = se_group_fx_env(grp, fxIdx);
            if (env != NULL) fxEnv = *env;
        }
        fxKeyGroup = fx->keyGroup;
        fxKeyGroupKill = fx->keyGroupKill;
        if (!lookup_sdir(grp, sampleId, &info)) {
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
            if (!group_span(grp, coefOff, 40, "ADPCM coefficient block") ||
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

    raw = (uint8_t *)malloc(byteLen);
    if (!raw) return MSM_ERR_OUTOFMEM;
    if (!msm_read_range((uint32_t)((uint64_t)sampPoolFileOfs + info.offset), byteLen, raw)) {
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

/* =======================================================================
 * Savestate audio shadow
 * =======================================================================
 * Everything in this file is carved out of the savestate, because it is
 * owned by the SDL audio callback thread -- restoring it would race a
 * thread that is running right now, and would install the capturing
 * process's host-malloc'd PCM pointers. The consequence is that after a
 * restore the mixer keeps playing whatever the LIVE process was playing,
 * while the restored game state believes something else entirely.
 *
 * The fix is to re-establish, not to byte-restore. These two functions are
 * that: capture the mixer's own view alongside the state file, and replay
 * it afterwards.
 *
 * WHY CAPTURE THE MIXER'S VIEW RATHER THAN THE GAME'S. The playing BGM
 * stream id has no game-visible home at all -- game/audio.c's
 * HuAudSStreamChanPlay passes streamId straight into msmStreamPlay and
 * records only the channel number, so g_chan[].streamId below is the only
 * copy in the process. Capturing from here also means the restore never has
 * to read (or defeat the equality guard on) game/audio.c's file-static
 * sndGroupBak, which would have required a new decomp patch. The mixer view
 * and the restored game state agree by construction: both are snapshots of
 * the same instant. */

static int mp6_ss_bool_valid(int value)
{
    return value == 0 || value == 1;
}

static int mp6_ss_fade_valid(int active, int paused, float mul, float step, int action)
{
    if (!mp6_ss_bool_valid(active) || !mp6_ss_bool_valid(paused) ||
        !isfinite(mul) || !isfinite(step) || mul < 0.0f || mul > 1.0f ||
        step < -1.0f || step > 1.0f || action < MP6_FADE_NONE ||
        action > MP6_FADE_TO_PLAY) {
        return 0;
    }
    if (action == MP6_FADE_NONE) return step == 0.0f;
    if (!active || paused) return 0;
    if (action == MP6_FADE_TO_PLAY) return step > 0.0f;
    return step < 0.0f; /* TO_PAUSE / TO_STOP */
}

static int mp6_ss_permanent_group(int grpIdx)
{
    int i;
    for (i = 0; i < g_baseGrpNum; i++) {
        if (g_baseGrpIdx[i] == grpIdx) return 1;
    }
    return 0;
}

static int mp6_ss_permanent_gid(uint16_t gid)
{
    int i;
    for (i = 0; i < g_baseGrpNum; i++) {
        int grpIdx = g_baseGrpIdx[i];
        if (grpIdx > 0 && grpIdx < g_grpInfoCount && g_grpInfo[grpIdx].gid == gid) return 1;
    }
    return 0;
}

static int mp6_ss_shadow_has_group(const Mp6SsAudioShadow *in, int grpIdx)
{
    int i;
    for (i = 0; i < in->groupCount; i++) {
        int encoded = in->groupIdx[i];
        if (encoded != INT_MIN && (encoded < 0 ? -encoded : encoded) == grpIdx) return 1;
    }
    return 0;
}

static int mp6_ss_shadow_is_zero(const Mp6SsAudioShadow *in)
{
    Mp6SsAudioShadow zero;
    memset(&zero, 0, sizeof(zero));
    return in != NULL && memcmp(in, &zero, sizeof(zero)) == 0;
}

static uint64_t mp6_ss_shadow_fingerprint(const Mp6SsAudioShadow *in)
{
    const unsigned char *bytes = (const unsigned char *)in;
    uint64_t hash = 1469598103934665603ull; /* FNV-1a */
    size_t i;
    for (i = 0; i < sizeof(*in); i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int mp6_ss_audio_verify_enabled(void)
{
    const char *value = getenv("MP6_SAVESTATE_AUDIO_VERIFY");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void mp6_ss_log_shadow(const char *label, const Mp6SsAudioShadow *shadow)
{
    int i, voices = 0, first = -1;
    /* The whole on-disk table, not the live cap: this logs what the FILE
     * says, and a shadow being logged may have been captured by a run with a
     * different cap than this one. */
    for (i = 0; i < MP6_SS_AUDIO_MAX_VOICES; i++) {
        if (shadow->voice[i].active) {
            if (first < 0) first = i;
            voices++;
        }
    }
    if (first >= 0) {
        const Mp6SsAudioVoice *v = &shadow->voice[first];
        printf("[SAVESTATE] audio-shadow %s hash=%016llx cap=%d voices=%d "
               "first(slot=%d seId=%d no=%d pos=%llu fade=%d/%g/%g)\n",
               label, (unsigned long long)mp6_ss_shadow_fingerprint(shadow),
               (int)shadow->voiceCap, voices,
               first, v->seId, v->seNo, (unsigned long long)v->posFrac,
               v->fadeAction, (double)v->fadeMul, (double)v->fadeStep);
    } else {
        printf("[SAVESTATE] audio-shadow %s hash=%016llx cap=%d voices=0\n",
               label, (unsigned long long)mp6_ss_shadow_fingerprint(shadow),
               (int)shadow->voiceCap);
    }
    fflush(stdout);
}

static int mp6_ss_stream_position_valid(const Mp6SsAudioChan *chan)
{
    PdtPack pack;
    uint32_t adpcmFrames, totalFrames, nativeFrq, stepFrac;
    uint64_t limit;

    if (!get_pack(chan->streamId, &pack)) return 0;
    adpcmFrames = pack_adpcm_frame_count(&pack);
    if (adpcmFrames == 0 || adpcmFrames > UINT32_MAX / 14u) return 0;
    totalFrames = adpcmFrames * 14u;
    nativeFrq = pack.frq ? pack.frq : MP6_MSM_OUT_RATE;
    stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    if (stepFrac == 0) return 0;
    limit = ((uint64_t)totalFrames << 16) + stepFrac;
    /* Capture may occur at a callback boundary just after the final sample's
     * increment and before the next callback observes end-of-buffer, hence
     * the one-step half-open allowance rather than pos < total<<16. */
    return chan->posFrac < limit;
}

static int mp6_ss_voice_position_valid(const Mp6SsAudioVoice *voice)
{
    MsmSeGroup group;
    MsmSampleInfo info;
    const MsmSeDef *def;
    const MsmFxEntry *fx = NULL;
    uint32_t loopEnd = 0, endFrame, nativeFrq, stepFrac;
    uint64_t limit;
    int loopMode, i, ok = 0;

    if (voice->seId < 0 || voice->seId >= g_seDefCount ||
        voice->groupIdx <= 0 || voice->groupIdx >= g_grpInfoCount) return 0;
    def = &g_seDefs[voice->seId];
    memset(&group, 0, sizeof(group));
    if (se_group_materialize(&group, voice->groupIdx, 0, 0,
                             "savestate-position-preflight") != 0) {
        return 0;
    }
    if (group.gid != def->gid) goto done;
    for (i = 0; i < group.fxCount; i++) {
        if (group.fx[i].fxId == def->fxId) {
            fx = &group.fx[i];
            break;
        }
    }
    if (fx == NULL || !lookup_sdir(&group, fx->sampleId, &info) || info.compType != 0) goto done;
    loopMode = mp6_msm_loop_bounds(info.length, info.loopStart, info.loopLength, &loopEnd);
    if (loopMode < 0) goto done;
    /* Same authored-note cap msmSePlay applies (it flattens the loop into a
     * finite one-shot), computed through the one shared helper so the two
     * can never disagree about a legal position. */
    {
        uint32_t noteFrames = mp6_se_authored_note_frames(&info, fx->lifeMs, loopMode, loopEnd);
        if (noteFrames != 0) {
            loopMode = 0;
            info.length = noteFrames;
        }
    }
    endFrame = loopMode > 0 ? loopEnd : info.length;
    nativeFrq = info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE;
    stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    if (endFrame == 0 || stepFrac == 0) goto done;
    limit = ((uint64_t)endFrame << 16) + stepFrac;
    ok = voice->posFrac < limit;

done:
    se_group_unload(&group);
    return ok;
}

int mp6_msm_savestate_validate(const Mp6SsAudioShadow *in)
{
    int i, j, chanMax, maxVoiceNo = 0, activeVoiceCount = 0;
    int valid = 1;

    if (in == NULL) return -1;
    if (mp6_ss_shadow_is_zero(in)) return 0;
    if (!g_mixerLockInit) {
        /* Capture before msmSysInit produces a byte-zero shadow.  Requiring
         * that exact representation prevents a non-empty state from being
         * silently discarded merely because this process has no live mutex. */
        return -1;
    }

    /* All directory/base-list inputs below are immutable after msmSysInit.
     * Do not hold the mixer lock while the voice-position preflight reads a
     * temporary group blob from disk; that would stall the audio callback. */

    chanMax = g_chanMax;
    if (chanMax > MP6_SS_AUDIO_MAX_CHAN) chanMax = MP6_SS_AUDIO_MAX_CHAN;
    /* voiceCap is checked BEFORE it is used to bound anything: a corrupt
     * capacity must not become a loop bound over the on-disk table. It is
     * deliberately NOT required to equal this run's cap -- a capture made at
     * the other size is a supported input, see Mp6SsAudioShadow's comment. */
    if (in->chanCount != chanMax || in->groupCount < 0 ||
        in->groupCount > MP6_SS_AUDIO_MAX_GROUPS ||
        !mp6_msm_voice_cap_valid(in->voiceCap) ||
        in->masterVol < 0 || in->masterVol > 127 ||
        in->seMasterVol < 0 || in->seMasterVol > 127 ||
        in->seNoCounter < 1 || in->seNoCounter >= INT_MAX) {
        valid = 0;
    }

    for (i = 0; valid && i < in->groupCount; i++) {
        int encoded = in->groupIdx[i];
        int grpIdx;
        MsmSeGroup checkGroup;
        if (!g_msmReady || encoded == 0 || encoded == INT_MIN) {
            valid = 0;
            break;
        }
        grpIdx = encoded < 0 ? -encoded : encoded;
        if (grpIdx <= 0 || grpIdx >= g_grpInfoCount || mp6_ss_permanent_group(grpIdx) ||
            mp6_ss_permanent_gid(g_grpInfo[grpIdx].gid)) {
            valid = 0;
            break;
        }
        for (j = 0; j < i; j++) {
            int prev = in->groupIdx[j];
            int prevIdx = prev < 0 ? -prev : prev;
            if (prevIdx == grpIdx || g_grpInfo[prevIdx].gid == g_grpInfo[grpIdx].gid) {
                valid = 0;
                break;
            }
        }
        if (valid) {
            memset(&checkGroup, 0, sizeof(checkGroup));
            if (se_group_materialize(&checkGroup, grpIdx, encoded < 0, encoded < 0,
                                     "savestate-group-preflight") != 0) {
                valid = 0;
            } else {
                se_group_unload(&checkGroup);
            }
        }
    }

    for (i = 0; valid && i < in->chanCount; i++) {
        const Mp6SsAudioChan *c = &in->chan[i];
        if (!mp6_ss_fade_valid(c->active, c->paused, c->fadeMul,
                               c->fadeStep, c->fadeAction)) {
            valid = 0;
            break;
        }
        if (!c->active) {
            if (c->paused != 0 || c->streamId != 0 || c->posFrac != 0 || c->vol != 0 ||
                c->fadeMul != 0.0f || c->fadeStep != 0.0f || c->fadeAction != MP6_FADE_NONE) {
                valid = 0;
            }
        } else {
            if (!g_pdtReady || c->streamId < 0 || c->streamId >= g_streamMax ||
                c->vol < 0 || c->vol > 127 || !mp6_ss_stream_position_valid(c)) {
                valid = 0;
            }
        }
    }

    /* Every slot of the on-disk table is inspected, including the ones above
     * the capture's own capacity: those must be canonical zero holes. A
     * capture claiming a voice in a slot it could not reach is corrupt, and
     * catching that here is what lets the apply path treat "active && slot >=
     * live cap" as purely a capacity difference rather than possible garbage. */
    for (i = 0; valid && i < MP6_SS_AUDIO_MAX_VOICES; i++) {
        const Mp6SsAudioVoice *v = &in->voice[i];
        if (!mp6_msm_voice_slot_restorable(i, in->voiceCap) && v->active) {
            valid = 0;
            break;
        }
        if (!mp6_ss_fade_valid(v->active, v->paused, v->fadeMul,
                               v->fadeStep, v->fadeAction)) {
            valid = 0;
            break;
        }
        if (!v->active) {
            if (v->paused != 0 || v->seId != 0 || v->groupIdx != 0 || v->seNo != 0 ||
                v->posFrac != 0 || v->vol != 0 || v->pan != 0 || v->fadeMul != 0.0f ||
                v->fadeStep != 0.0f || v->fadeAction != MP6_FADE_NONE) {
                valid = 0;
            }
            continue;
        }
        if (!g_msmReady || v->seId < 0 || v->seId >= g_seDefCount ||
            v->groupIdx <= 0 || v->groupIdx >= g_grpInfoCount ||
            (v->posFrac >> 16) > UINT32_MAX ||
            g_seDefs[v->seId].gid == 0xFFFFu ||
            g_seDefs[v->seId].gid != g_grpInfo[v->groupIdx].gid ||
            (!mp6_ss_permanent_group(v->groupIdx) &&
             !mp6_ss_shadow_has_group(in, v->groupIdx)) ||
            v->seNo <= 0 || v->seNo >= in->seNoCounter ||
            v->vol < 0 || v->vol > 127 || v->pan < 0 || v->pan > 127 ||
            !mp6_ss_voice_position_valid(v)) {
            valid = 0;
            break;
        }
        for (j = 0; j < i; j++) {
            if (in->voice[j].active && in->voice[j].seNo == v->seNo) {
                valid = 0;
                break;
            }
        }
        activeVoiceCount++;
        if (v->seNo > maxVoiceNo) maxVoiceNo = v->seNo;
    }
    if (valid && (in->seNoCounter <= maxVoiceNo ||
                  in->seNoCounter > INT_MAX - activeVoiceCount)) {
        valid = 0;
    }

    return valid ? 0 : -1;
}

int mp6_msm_savestate_capture(Mp6SsAudioShadow *out)
{
    int groupSlot[MP6_MSM_MAX_SE_GROUPS];
    int i, j, n, groupN = 0;
    int failed = 0;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    /* Savestates may be requested at an unusually early frame boundary.
     * A zero shadow honestly represents audio that has not initialized yet;
     * never EnterCriticalSection/pthread_mutex_lock zero-filled storage. */
    if (!g_mixerLockInit) return 0;

    /* One atomic group+voice view.  Group first is the established lock
     * order; the callback takes only the mixer lock. */
    mp6_grp_lock();
    mp6_lock();
    out->seNoCounter = g_seNoCounter;
    out->masterVol = g_masterVol;
    out->seMasterVol = g_seMasterVol;
    /* Record the capacity this capture was taken at. The table written below
     * is always the full MP6_SS_AUDIO_MAX_VOICES entries wide; this is what
     * says how many of them were reachable, and it is the only thing that
     * makes a restore under the OTHER size well-defined. */
    out->voiceCap = mp6_sfx_voice_cap();
    n = g_chanMax;
    if (n > MP6_SS_AUDIO_MAX_CHAN) {
        failed = 1;
        n = MP6_SS_AUDIO_MAX_CHAN;
    }
    out->chanCount = n;
    for (i = 0; i < n; i++) {
        if (!g_chan[i].active) continue; /* output was zeroed: canonical hole */
        out->chan[i].active     = 1;
        out->chan[i].paused     = g_chan[i].paused;
        out->chan[i].streamId   = g_chan[i].streamId;
        out->chan[i].posFrac    = g_chan[i].posFrac;
        out->chan[i].vol        = g_chan[i].vol;
        out->chan[i].fadeMul    = g_chan[i].fadeMul;
        out->chan[i].fadeStep   = g_chan[i].fadeStep;
        out->chan[i].fadeAction = g_chan[i].fadeAction;
    }

    /* Only immutable init-time bases can be omitted.  Preserve load order,
     * not slot order: deleted/reused slots otherwise invert the LIFO order
     * observed later by msmSysDelGroupBase. */
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (g_seGroups[i].inUse && (!g_seGroups[i].baseGrpF || g_seGroups[i].dynBaseF)) {
            if (groupN >= MP6_SS_AUDIO_MAX_GROUPS) {
                fprintf(stderr, "[SAVESTATE] refusing capture: more than %d non-permanent SE groups "
                                "are resident and the audio shadow cannot represent them exactly\n",
                        MP6_SS_AUDIO_MAX_GROUPS);
                failed = 1;
                break;
            }
            groupSlot[groupN++] = i;
        }
    }
    for (i = 1; i < groupN; i++) {
        int slot = groupSlot[i];
        j = i;
        while (j > 0 && g_seGroups[groupSlot[j - 1]].loadOrder > g_seGroups[slot].loadOrder) {
            groupSlot[j] = groupSlot[j - 1];
            j--;
        }
        groupSlot[j] = slot;
    }
    out->groupCount = groupN;
    for (i = 0; i < groupN; i++) {
        const MsmSeGroup *grp = &g_seGroups[groupSlot[i]];
        out->groupIdx[i] = grp->dynBaseF ? -grp->grpIdx : grp->grpIdx;
    }

    /* Bounded by the cap, not the capacity: slots at or above it cannot be
     * active (nothing can allocate them), and leaving them as the memset's
     * zeros is exactly the canonical hole the preflight demands there. */
    for (i = 0; i < out->voiceCap; i++) {
        const MsmSeVoice *v = &g_sfxVoice[i];
        Mp6SsAudioVoice *dst = &out->voice[i];
        if (!v->active) continue; /* canonical zero hole */
        dst->active = 1;
        dst->paused = v->paused;
        dst->seId = v->seId;
        dst->groupIdx = v->grpIdx;
        dst->seNo = v->no;
        dst->posFrac = v->posFrac;
        dst->vol = v->vol;
        dst->pan = v->pan;
        dst->fadeMul = v->fadeMul;
        dst->fadeStep = v->fadeStep;
        dst->fadeAction = v->fadeAction;
    }
    mp6_unlock();
    mp6_grp_unlock();

    if (failed || mp6_msm_savestate_validate(out) != 0) {
        fprintf(stderr, "[SAVESTATE] refusing capture: audio shadow is not exactly replayable\n");
        fflush(stderr);
        return -1;
    }
    if (mp6_ss_audio_verify_enabled()) mp6_ss_log_shadow("snapshot", out);
    return 0;
}

static int mp6_msm_savestate_restore_voice(const Mp6SsAudioVoice *in, int targetSlot)
{
    MsmPcmRetirement retired = {0};
    MSM_SEPARAM param;
    MsmSeVoice *created;
    MsmSeVoice *target;
    int temporaryNo;

    memset(&param, 0, sizeof(param));
    param.flag = MSM_SEPARAM_VOL | MSM_SEPARAM_PAN;
    param.vol = in->vol;
    param.pan = in->pan;
    temporaryNo = msmSePlay(in->seId, &param);
    if (temporaryNo < 0) return temporaryNo;

    mp6_lock();
    created = find_sfx_voice_by_no(temporaryNo);
    target = &g_sfxVoice[targetSlot];
    if (created == NULL || (target != created && target->active)) {
        if (created != NULL) {
            created->active = 0;
            mp6_pcm_detach(&retired, &created->pcm);
        }
        mp6_unlock();
        mp6_pcm_release(&retired);
        return MSM_ERR_PLAYFAIL;
    }
    if (target != created) {
        mp6_pcm_detach(&retired, &target->pcm);
        *target = *created; /* transfers ownership of the private PCM */
        memset(created, 0, sizeof(*created));
    }
    target->paused = in->paused;
    target->posFrac = in->posFrac;
    target->vol = in->vol;
    target->pan = in->pan;
    target->gainL = (127 - in->pan) / 127.0f;
    target->gainR = in->pan / 127.0f;
    target->no = in->seNo;
    target->fadeMul = in->fadeMul;
    target->fadeStep = in->fadeStep;
    target->fadeAction = in->fadeAction;
    mp6_unlock();
    mp6_pcm_release(&retired);
    return 0;
}

static void mp6_msm_savestate_apply_fatal(const char *what, int id, int result)
{
    /* Guest memory is already committed when apply runs.  Returning would
     * let a logically loaded state continue with missing streams/voices (and
     * game-visible handles that never resolve).  All malformed-data cases
     * were preflighted; a remaining error is resource/I/O failure, so fail
     * stop loudly instead of publishing a partial restore. */
    mp6_lock();
    g_savestateMixerMuted = 0;
    mp6_unlock();
    fprintf(stderr, "[SAVESTATE] FATAL: post-commit audio replay failed (%s=%d, result=%d)\n",
            what, id, result);
    fflush(stderr);
    /* Do not run atexit handlers or restored CRT teardown state: guest memory
     * has already been replaced, while g_wasRestored is intentionally not
     * published until this function succeeds. */
    _Exit(EXIT_FAILURE);
}

void mp6_msm_savestate_apply(const Mp6SsAudioShadow *in)
{
    int i, voiceCount = 0, voicesDropped = 0;
    const int liveVoiceCap = mp6_sfx_voice_cap();
    /* msmSysLoadGroup rejects a NULL staging buffer (mirroring the real
     * engine, whose game-side malloc really can fail) but never dereferences
     * it -- this port reads the .msm directly. A non-NULL dummy is therefore
     * sufficient and avoids allocating a real staging buffer here. */
    static int dummyStagingBuf;

    if (in == NULL || !g_mixerLockInit) {
        return; /* audio never came up in this process -- nothing to re-sync */
    }
    if (mp6_ss_audio_verify_enabled()) mp6_ss_log_shadow("apply-input", in);

    /* Gate the callback before stopping anything and keep it gated through
     * every decode/publish below.  This makes the multi-voice transaction
     * audibly atomic even though file I/O cannot hold g_mixerLock. */
    mp6_lock();
    g_savestateMixerMuted = 1;
    mp6_unlock();

    /* 1-2. Silence everything the LIVE process had going. speed 0 takes the
     * immediate path (frees each channel's pcm, clears in-flight fades)
     * rather than starting a ramp we would then fight. checkGrp=FALSE so
     * base-group SE voices are cut too, instead of being left ringing. */
    msmStreamStopAll(0);
    msmSeStopAll(FALSE, 0);
    if (mp6_ss_shadow_is_zero(in)) {
        /* A capture made before msmSysInit has no host audio resources to
         * replay.  Silence this process's later-initialized mixer and restore
         * the static next-handle default; a restored cold-boot path may then
         * call msmSysInit normally. */
        mp6_lock();
        g_seNoCounter = 1;
        g_savestateMixerMuted = 0;
        mp6_unlock();
        return;
    }
    /* Replay allocates temporary handles through the ordinary msmSePlay
     * path.  Start at the captured next-handle value, which validation proved
     * has headroom for every active slot.  Temporary handles are therefore
     * disjoint from every captured handle already installed into an earlier
     * slot; starting from 1 made find_sfx_voice_by_no select/destroy the wrong
     * voice whenever those ranges overlapped. */
    mp6_lock();
    g_seNoCounter = in->seNoCounter;
    mp6_unlock();

    /* 3. SE banks: drop both classes of live dynamic group and load the ones
     * that were resident at capture, so a restored scene's sound effects
     * resolve against the bank the captured moment actually had. */
    msmSysDelGroupBase(0);
    msmSysDelGroupAll();
    /* C5 (review): clamp -- groupCount arrives from the file header, and an
     * unclamped corrupt value walked this loop gigabytes past the 16-entry
     * array on the caller's stack. The restore path validates the shadow
     * too; this bound keeps the function safe on its own terms. */
    /* Added-base groups first: msmSysLoadGroupBase deliberately clears plain
     * dynamics on each call. No ordinary group has been restored yet, so
     * repeated base loads retain one another without erasing later work. */
    for (i = 0; i < in->groupCount && i < MP6_SS_AUDIO_MAX_GROUPS; i++) {
        if (in->groupIdx[i] < 0 && in->groupIdx[i] != INT_MIN) {
            int result = msmSysLoadGroupBase(-in->groupIdx[i], &dummyStagingBuf);
            if (result != 0) mp6_msm_savestate_apply_fatal("base-group", -in->groupIdx[i], result);
        }
    }
    for (i = 0; i < in->groupCount && i < MP6_SS_AUDIO_MAX_GROUPS; i++) {
        if (in->groupIdx[i] > 0) {
            int result = msmSysLoadGroup(in->groupIdx[i], &dummyStagingBuf, FALSE);
            if (result != 0) mp6_msm_savestate_apply_fatal("group", in->groupIdx[i], result);
        }
    }

    /* 4. Master volumes (plain scalars here with no game-side mirror). */
    msmStreamSetMasterVolume(in->masterVol);
    msmSeSetMasterVolume(in->seMasterVol);

    /* 5. Replay each captured stream, then SEEK it back to where it was.
     * msmStreamPlay unconditionally resets posFrac to 0, so replay alone
     * would restart every track from the top -- audible as the BGM jumping
     * to its intro on every load. Writing the captured position back under
     * the mixer lock resumes mid-phrase instead.
     *
     * C20 (review): the stream is replayed PAUSED (MSM_STREAMPARAM_PAUSE),
     * then unpaused inside the write-back lock below. Without that, the SDL
     * callback could run in the window between msmStreamPlay's own locked
     * publish (active, pos 0, full volume) and this write-back -- rendering
     * an audible intro-blip at full volume, worst for a channel that was
     * captured paused or mid-fade-out. Paused publish renders silence for
     * that window instead.
     *
     * The counts are clamped defensively even though the restore path
     * validates the shadow before calling here (C5): this function's
     * contract must not depend on every caller re-checking. */
    for (i = 0; i < in->chanCount && i < g_chanMax && i < MP6_SS_AUDIO_MAX_CHAN; i++) {
        MSM_STREAMPARAM param;
        if (!in->chan[i].active) {
            continue;
        }
        memset(&param, 0, sizeof(param));
        param.flag = MSM_STREAMPARAM_CHAN | MSM_STREAMPARAM_VOL | MSM_STREAMPARAM_PAUSE;
        param.chan = i;
        param.vol = in->chan[i].vol;
        {
            int result = msmStreamPlay(in->chan[i].streamId, &param);
            if (result < 0) mp6_msm_savestate_apply_fatal("stream", in->chan[i].streamId, result);
        }
        mp6_lock();
        if (g_chan[i].active) {
            g_chan[i].posFrac    = in->chan[i].posFrac;
            g_chan[i].paused     = in->chan[i].paused;
            g_chan[i].fadeMul    = in->chan[i].fadeMul;
            g_chan[i].fadeStep   = in->chan[i].fadeStep; /* C14: without the step, a captured fade never completes */
            g_chan[i].fadeAction = in->chan[i].fadeAction;
        }
        mp6_unlock();
    }

    /* 6. Recreate every active SFX in its original mixer slot and restore
     * the exact game-visible handle.  Validator proved each owner group is
     * permanent or present in the captured dynamic group set, so msmSePlay
     * resolves the same immutable sample and re-derives loop/rate/baseVol.
     *
     * CROSS-CAPACITY RULE (mp6_msm_voice_slot_restorable, msm_safe.h). The
     * cap is host configuration and is carved out of the restore, so THIS
     * process's cap decides -- not the capturing process's. A captured slot
     * this run cannot hold is dropped and named, never remapped into a
     * different slot: slot index is the voice's identity in the mixer order
     * and in the handle the game already holds. Dropping is safe because
     * seNoCounter is still restored exactly below, so the dropped handle is
     * retired rather than recycled, and msmSeGetStatus answers MSM_SE_DONE
     * for it -- indistinguishable from a one-shot that finished a tick
     * earlier, which is a thing the game already has to tolerate. */
    for (i = 0; i < MP6_SS_AUDIO_MAX_VOICES; i++) {
        if (!in->voice[i].active) continue;
        if (!mp6_msm_voice_slot_restorable(i, liveVoiceCap)) {
            printf("[SAVESTATE] SFX slot %d (seId=%d seNo=%d) captured with a %d-slot "
                   "voice table cannot be restored into this run's %d slots -- dropped; "
                   "its handle is retired, not reused\n",
                   i, (int)in->voice[i].seId, (int)in->voice[i].seNo,
                   (int)in->voiceCap, liveVoiceCap);
            voicesDropped++;
            continue;
        }
        {
            int result = mp6_msm_savestate_restore_voice(&in->voice[i], i);
            if (result != 0) mp6_msm_savestate_apply_fatal("SFX slot", i, result);
        }
        voiceCount++;
    }

    /* 7. Exact next handle, not the old +4096 invalidation heuristic.  Live
     * voices now carry their captured `no` values, and validation proved the
     * captured counter is greater than all of them. */
    mp6_lock();
    g_seNoCounter = in->seNoCounter;
    mp6_unlock();

    printf("[SAVESTATE] audio re-synced: %d channel slot(s), %d SE group(s), %d SFX voice(s)\n",
           (int)in->chanCount, (int)in->groupCount, voiceCount);
    if (voicesDropped > 0) {
        printf("[SAVESTATE] %d SFX voice(s) dropped: captured with %d voice slots, this run "
               "has %d\n", voicesDropped, (int)in->voiceCap, liveVoiceCap);
    }
    fflush(stdout);
    if (mp6_ss_audio_verify_enabled()) {
        Mp6SsAudioShadow live;
        int captured = mp6_msm_savestate_capture(&live) == 0;
        /* Exactness is only claimable for a SAME-capacity restore: across a
         * capacity change the live shadow legitimately differs (voiceCap
         * itself, plus any dropped slot). The counter above already reports
         * what was lost; this stays a strict byte compare so it cannot be
         * quietly weakened for the same-capacity case the gates check. */
        int exact = captured && memcmp(&live, in, sizeof(live)) == 0;
        printf("[SAVESTATE] audio-shadow apply-check expected=%016llx live=%016llx exact=%d\n",
               (unsigned long long)mp6_ss_shadow_fingerprint(in),
               captured ? (unsigned long long)mp6_ss_shadow_fingerprint(&live) : 0ull,
               exact);
        fflush(stdout);
    }
    mp6_lock();
    g_savestateMixerMuted = 0;
    mp6_unlock();
}
