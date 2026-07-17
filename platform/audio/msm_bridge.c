/* MP6 native port -- real streamed-music and sound-effect playback,
 * replacing tools/gen_shims.py's generated logging no-op for the msm
 * STREAM + SE (sound effect) families + msmSysInit/msmSysRegularProc +
 * the AI family. This header comment covers the scope/design decisions
 * actually load-bearing for the code below.
 *
 * SCOPE -- what this file takes over (see tools/gen_shims.py's
 * MANUAL_SYMBOLS, which excludes exactly these from generation):
 *   msmSysInit, msmSysRegularProc, msmStreamPlay, msmStreamStop,
 *   msmStreamPauseAll, msmStreamPause, msmStreamSetParam,
 *   msmStreamGetStatus, msmStreamStopAll, msmStreamSetMasterVolume,
 *   msmSePlay, msmSeStop, msmSeStopAll, msmSeGetStatus, msmSePauseAll,
 *   msmSeSetParam, msmSeSetMasterVolume, msmSysLoadGroup{,Base},
 *   msmSysDelGroup{All,Base}, msmSysGetSampSize, msmSysSetGroupLoadMode,
 *   AIGetDMAStartAddr, AIInitDMA, AIRegisterDMACallback,
 *   AISetStreamPlayState, AISetStreamVolLeft, AISetStreamVolRight, AIStartDMA.
 *
 * What this file deliberately does NOT take over (left exactly as the
 * auto-generated logging no-op in platform/null/shims_{generated,
 * generated_aurora}.c): msmSysSetOutputMode/msmSysSetAux/msmSysCheckInit
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
 * unconditional -- not gated on any caller-supplied pan at all). Bit1
 * (`stereoF`) controls a real-hardware loop-restart/pause-timing
 * refinement (see msmStreamPauseOn/msmStreamDvdCallback) entirely about
 * avoiding a transient glitch across the DOUBLE-BUFFERED async DVD refill
 * boundary -- moot for this file's own design (full upfront decode, no
 * double-buffering at all -- see next section), so it's read from the
 * file but never actually consulted below. MP6_PACK_FLAG_STEREO (this
 * file) is therefore bit0, matching the header name and the real asm,
 * NOT the bit msmStreamSlotInit's own stereoF field happens to read.
 *
 * DESIGN SIMPLIFICATIONS (deliberate):
 *   - Real hardware streams incrementally via double-buffered ASYNC DVD
 *     reads (msmStreamData/msmStreamDvdCallback's whole ping-pong dance)
 *     because a real DVD read takes real milliseconds and audio can't
 *     wait on it. This port's own DVDReadPrio (platform/dvd/dvd_files.c)
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
#include "mp6_audio_out.h"
#include "host.h" /* mp6_host_mutex_* (mixer/group locks), mp6_host_thread_start
                   * + mp6_host_sleep_ns (the opt-in MP6_AUDIO_LEAKTEST_*
                   * stress threads) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

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
    MP6_FADE_TO_STOP,  /* fading OUT; on completion, fully deactivate + free pcm (msmStreamStop/msmSeStop) */
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
 *       simple one-shot UI SFX (every macro this game actually uses is
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
 *   - Only the macro's FIRST StartSample opcode is honored -- no ADSR
 *     envelope shaping, no vibrato/pitch-sweep/portamento. A real, audible
 *     one-shot plays; the many expressive-synthesis opcodes MuSyX supports
 *     do not run. Resolution also handles KEYMAP/LAYER indirection (an
 *     fx's object id can name those, not just a macro -- see
 *     resolve_first_sample), but a multi-row layer still starts only its
 *     FIRST covering row: one voice per SE, skipped rows logged.
 *   - No 3D positional emitters (MSM_SEPARAM_POS / msmSeSetListener family
 *     stay the existing auto-generated no-op) -- flat stereo pan only,
 *     matching the dry-stereo scope used for streams above.
 * ======================================================================= */
#define MP6_MSM_MAX_SFX_VOICES 16
/* Cap on ALL simultaneously-loaded groups: 5 init-time base groups + the
 * real engine's own dynamic-stack caps (MSM_INFO.stackDepthA/B = 0/8 on
 * this disc, logged at init) + msmSysLoadGroupBase's own extra-base-group
 * headroom (real cap: baseGrpNo < 0xF, msmsys.c) still fit with margin. */
#define MP6_MSM_MAX_SE_GROUPS 24

static char g_msmPath[512];
static int g_msmReady;
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
} MsmFxEntry;

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
    int32_t poolOfs, projOfs, sdirOfs;
    uint32_t sampPoolFileOfs; /* header.sampOfs + this group's own grpInfo.sampOfs --
                                  absolute .msm file offset of byte 0 of this group's
                                  OWN sample pool (SDIR_DATA.offset is relative to this) */
    MsmFxEntry *fx;      /* this group's OWN fx->sample index, malloc'd; freed on unload */
    int fxCount;
    int fxCap;
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
    int16_t *pcm;        /* MONO, malloc'd, totalFrames samples (one-shot, never loops) */
    uint32_t totalFrames;
    uint64_t posFrac;    /* Q16.16, in the sample's own native-rate frames */
    uint32_t stepFrac;   /* Q16.16 per-output-sample advance, native rate -> MP6_MSM_OUT_RATE */
    int baseVol, vol;    /* 0-127 each, same authored*runtime*master formula as g_chan */
    float gainL, gainR;  /* from pan (0-127, 64==center), simple linear pan law */
    int no;              /* unique, ever-increasing handle -- matches the REAL msmSePlay's
                          * own player->no contract (msmSeSearchEntry-by-.no in the decomp) */
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
} MsmSeVoice;

static MsmSeVoice g_sfxVoice[MP6_MSM_MAX_SFX_VOICES];
static int g_seNoCounter = 1;

static Mp6Mutex g_mixerLock; /* The host seam's caller-storage mutex (win32
                              * backend is the same CRITICAL_SECTION inside) */
static int g_mixerLockInit;

static void mp6_lock(void) { mp6_host_mutex_lock(&g_mixerLock); }
static void mp6_unlock(void) { mp6_host_mutex_unlock(&g_mixerLock); }

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

static void mp6_wav_capture(const int16_t *out, uint32_t frames)
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

    if (!g_wavPath[0] || g_wavDone || !g_wavAccum || !g_wavArmed) return;

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
 * platform/audio/audio_out_sdl.c provides for the default (aurora) build
 * instead -- see mp6_audio_out.h's own header comment for the split.
 *
 * mp6_headless_drain_wav_dump exists because msmSysRegularProc() further
 * below (the REAL per-frame pump src/game/pad.c's PadReadVSync calls) is
 * UNREACHABLE dead code in this build mode: PadReadVSync only ever runs
 * via the VI post-retrace callback game/pad.c's HuPadInit registers
 * through VISetPostRetraceCallback -- and this build mode's own VI shims
 * (platform/null/shims_generated.c's AUTO-GENERATED VISetPostRetraceCallback
 * -- a bare `MP6_LOG_ONCE(...); return 0;`, never storing the callback at
 * all -- and platform/null/shims_manual.c's hand-written VIWaitForRetrace,
 * which never invokes one) never actually fire it. The REGISTRATION does
 * happen (the boot log's own "[SDK] VI.VISetPostRetraceCallback(...)"
 * line proves that much) -- it's purely the invocation side that's
 * missing. This is a real, pre-existing, cross-cutting platform/null
 * VI-callback gap outside platform/audio/'s own scope -- reported, not
 * patched here.
 *
 * Rather than reach into platform/null/*.c to fix that at the root, this
 * file drives its OWN mixer pump instead, entirely within platform/audio/:
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
 * (platform/os/dll_bridge.c + platform/dvd/dvd_files.c already back these
 * for real, over the extracted disc tree -- reused as-is rather than
 * reaching around them, exactly matching how the real src/msm/msmfio.c
 * itself only ever calls DVDOpen/DVDReadPrio/DVDClose). Every read here
 * completes fully before returning (this port's own DVDReadPrio always
 * does), so no callback/async plumbing is needed at all.
 * ======================================================================= */
static BOOL pdt_read_range(uint32_t offset, uint32_t length, void *dst)
{
    DVDFileInfo fi;
    BOOL ok;

    if (length == 0) return TRUE;
    if (!DVDOpen(g_pdtPath, &fi)) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDOpen(\"%s\") failed\n", g_pdtPath);
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

static void decode_substream(uint32_t sampleOfs, int16_t coefIdx, uint32_t numFrames, int16_t *outMono)
{
    uint8_t *raw;
    MP6AdpcmState state;

    if (numFrames == 0) return;
    raw = (uint8_t *)malloc((size_t)numFrames * 8);
    if (!raw) {
        fprintf(stderr, "[AUDIO] msm_bridge: out of memory decoding %u ADPCM frames\n", (unsigned)numFrames);
        memset(outMono, 0, (size_t)numFrames * 14 * sizeof(int16_t));
        return;
    }
    if (!pdt_read_range(sampleOfs, numFrames * 8, raw)) {
        memset(outMono, 0, (size_t)numFrames * 14 * sizeof(int16_t));
        free(raw);
        return;
    }
    state.hist1 = 0;
    state.hist2 = 0;
    if (coefIdx < 0 || coefIdx >= g_numCoef) {
        static MP6AdpcmCoefTable s_silentCoef;
        fprintf(stderr, "[AUDIO] msm_bridge: adpcmParamIdx %d out of range (have %d coef tables) -- "
                "decoding silence instead of garbage\n", (int)coefIdx, g_numCoef);
        mp6_dspadpcm_decode(raw, numFrames, &s_silentCoef, &state, outMono);
    } else {
        mp6_dspadpcm_decode(raw, numFrames, &g_coef[coefIdx], &state, outMono);
    }
    free(raw);
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
    ok = DVDReadPrio(&fi, dst, (s32)length, (s32)offset, 2);
    DVDClose(&fi);
    if (!ok) {
        fprintf(stderr, "[AUDIO] msm_bridge: DVDReadPrio (msm) failed (offset=%u length=%u)\n",
                (unsigned)offset, (unsigned)length);
    }
    return ok;
}

/* Appends to the OWNING GROUP's fx index (freed wholesale with the
 * group on unload -- see MsmFxEntry's own comment for why per-group). */
static void add_fx_entry(MsmSeGroup *grp, uint16_t fxId, uint16_t sampleId)
{
    if (grp->fxCount >= grp->fxCap) {
        int newCap = grp->fxCap ? grp->fxCap * 2 : 64;
        MsmFxEntry *n = (MsmFxEntry *)realloc(grp->fx, (size_t)newCap * sizeof(MsmFxEntry));
        if (!n) return;
        grp->fx = n;
        grp->fxCap = newCap;
    }
    grp->fx[grp->fxCount].fxId = fxId;
    grp->fx[grp->fxCount].sampleId = sampleId;
    grp->fxCount++;
}

/* GROUP_DATA linked list @ blob+projOfs -- nextOff is BASE-relative to
 * projOfs (see this file's header comment). Finds the entry whose own
 * `id` field matches targetGid; *outOff receives its offset (relative to
 * blob+projOfs, i.e. the absolute address is blob+projOfs+*outOff). */
static BOOL find_group_data(const MsmSeGroup *grp, uint16_t targetGid, uint32_t *outOff)
{
    const uint8_t *prjBase = grp->blob + grp->projOfs;
    uint32_t off = 0;
    int guard = 0;

    for (;;) {
        const uint8_t *g = prjBase + off;
        uint32_t nextOff = be32(g + 0);
        uint16_t id = (uint16_t)be16(g + 4);
        if (id == targetGid) {
            *outOff = off;
            return TRUE;
        }
        if (nextOff == 0xFFFFFFFFu || ++guard > 64) return FALSE;
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
static const uint8_t *pool_find_node(const MsmSeGroup *grp, uint32_t listOfsField, uint16_t nodeId)
{
    const uint8_t *poolBase = grp->blob + grp->poolOfs;
    uint32_t off = be32(poolBase + listOfsField);
    int guard = 0;

    if (off == 0) return NULL; /* absent list */
    for (;;) {
        const uint8_t *m = poolBase + off;
        uint32_t nextOff = be32(m + 0);
        uint16_t id = (uint16_t)be16(m + 4);
        if (id == nodeId) return m;
        if (nextOff == 0xFFFFFFFFu || ++guard > 4096) return NULL;
        off += nextOff; /* self-relative -- accumulates */
    }
}

/* Scans a macro node's MSTEP array for the first StartSample (0x10)
 * opcode; *outSampleId receives (para[0]>>8)&0xffff from that step. */
static BOOL find_macro_first_sample(const MsmSeGroup *grp, uint16_t macroId, uint16_t *outSampleId)
{
    const uint8_t *m = pool_find_node(grp, 0 /* POOL_DATA.macroOff */, macroId);
    const uint8_t *steps;
    int i;

    if (!m) return FALSE;
    steps = m + 8;
    for (i = 0; i < 64; i++) {
        uint32_t p0 = be32(steps + (size_t)i * 8);
        uint8_t opcode = (uint8_t)(p0 & 0x7Fu);
        if (opcode == 0x10) {
            *outSampleId = (uint16_t)((p0 >> 8) & 0xFFFFu);
            return TRUE;
        }
        if (opcode == 0x0) break; /* end-of-macro, no StartSample found */
    }
    return FALSE;
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
static BOOL resolve_first_sample(const MsmSeGroup *grp, uint16_t objId, int key, int depth,
                                 uint16_t *outSampleId)
{
    if (depth <= 0) return FALSE;
    if (key < 0) key = 0;
    if (key > 127) key = 127;

    switch (objId & 0xC000) {
    case 0x0000:
        return find_macro_first_sample(grp, objId, outSampleId);

    case 0x4000: { /* KEYMAP */
        const uint8_t *m = pool_find_node(grp, 8 /* POOL_DATA.keymapOff */, objId);
        const uint8_t *entry;
        uint16_t subId;
        int8_t transpose;
        if (!m) return FALSE;
        entry = m + 8 + (size_t)(key & 0x7f) * 8;
        subId = (uint16_t)be16(entry + 0);
        transpose = (int8_t)entry[2];
        if (subId == 0xFFFF || (subId & 0xC000) == 0x4000) return FALSE; /* StartKeymap's own guard */
        return resolve_first_sample(grp, subId, key + transpose, depth - 1, outSampleId);
    }

    case 0x8000: { /* LAYER */
        const uint8_t *m = pool_find_node(grp, 0xC /* POOL_DATA.layerOff */, objId);
        uint32_t num, i;
        int matched = 0;
        BOOL resolved = FALSE;
        if (!m) return FALSE;
        num = be32(m + 8);
        if (num > 128) return FALSE; /* implausible -- refuse to walk garbage */
        for (i = 0; i < num; i++) {
            const uint8_t *row = m + 12 + (size_t)i * 12;
            uint16_t subId = (uint16_t)be16(row + 0);
            uint8_t keyLow = row[2], keyHigh = row[3];
            int8_t transpose = (int8_t)row[4];
            if (subId == 0xFFFF || key < keyLow || key > keyHigh) continue;
            matched++;
            if (!resolved) {
                resolved = resolve_first_sample(grp, subId, key + transpose, depth - 1, outSampleId);
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
    const uint8_t *prjBase = grp->blob + grp->projOfs;
    const uint8_t *g = prjBase + groupDataOff;
    uint16_t type = (uint16_t)be16(g + 6);
    uint32_t fxTableOff;
    const uint8_t *fxBase;
    uint16_t fxNum, i;

    if (type != 1) return; /* song/sequencer group, not an FX group -- no FX_DATA to read */
    fxTableOff = be32(g + 0x1C);
    fxBase = prjBase + fxTableOff;
    fxNum = (uint16_t)be16(fxBase + 0);
    for (i = 0; i < fxNum; i++) {
        const uint8_t *e = fxBase + 4 + (size_t)i * 10;
        uint16_t fid = (uint16_t)be16(e + 0);
        uint16_t objId = (uint16_t)be16(e + 2); /* macro OR keymap/layer -- see resolve_first_sample */
        uint8_t key = e[8];                     /* FX_TAB.key -- selects keymap entries/layer rows */
        uint16_t sampleId;
        if (resolve_first_sample(grp, objId, key, 4, &sampleId)) {
            add_fx_entry(grp, fid, sampleId);
        }
    }
}

/* SDIR_DATA linear list @ blob+sdirOfs, terminated by id==0xffff. */
static BOOL lookup_sdir(const MsmSeGroup *grp, uint16_t sampleId, MsmSampleInfo *out)
{
    const uint8_t *s = grp->blob + grp->sdirOfs;
    uint32_t off = 0;

    for (;;) {
        uint16_t id = (uint16_t)be16(s + off + 0);
        if (id == 0xFFFFu) return FALSE;
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
        off += 32;
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

/* g_grpLock must be held. Totals for the memory-discipline logs: count +
 * malloc'd metadata bytes of loaded groups. */
static void se_group_totals(int *outCount, uint32_t *outBytes)
{
    int i, n = 0;
    uint32_t b = 0;
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (!g_seGroups[i].inUse) continue;
        n++;
        b += g_seGroups[i].blobSize + (uint32_t)g_seGroups[i].fxCap * (uint32_t)sizeof(MsmFxEntry);
    }
    *outCount = n;
    *outBytes = b;
}

/* g_grpLock must be held. Frees everything the slot owns. */
static void se_group_unload(MsmSeGroup *G)
{
    free(G->blob);
    free(G->fx);
    memset(G, 0, sizeof(*G));
}

/* g_grpLock must be held. Loads grpInfo[grpIdx] into a free slot (dedup'd
 * by gid, matching the real msmSysCheckLoadGroupID). Returns 0 or a real
 * MSM_ERR_* value. `why` tags the log line with the caller. */
static s32 se_group_load(int grpIdx, int baseGrpF, int dynBaseF, const char *why)
{
    const MsmGrpInfo *gi;
    MsmSeGroup *G;
    MsmSeGroup *existing;
    uint32_t groupDataOff;
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
    if (gi->dataSize == 0) {
        fprintf(stderr, "[AUDIO] se_group_load(%s): grpIdx=%d gid=%u has an empty data blob -- "
                "MSM_ERR_64\n", why, grpIdx, (unsigned)gi->gid);
        return MP6_MSM_ERR_64;
    }

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
    memset(G, 0, sizeof(*G));
    G->blob = (uint8_t *)malloc(gi->dataSize);
    if (!G->blob) {
        fprintf(stderr, "[AUDIO] se_group_load(%s): out of memory for grpIdx=%d's %u-byte blob\n",
                why, grpIdx, (unsigned)gi->dataSize);
        return MSM_ERR_OUTOFMEM;
    }
    if (!msm_read_range(g_msmGrpDataOfs + gi->dataOfs, gi->dataSize, G->blob)) {
        fprintf(stderr, "[AUDIO] se_group_load(%s): failed reading grpIdx=%d gid=%u data blob\n",
                why, grpIdx, (unsigned)gi->gid);
        free(G->blob);
        G->blob = NULL;
        return MSM_ERR_READFAIL;
    }
    G->blobSize = gi->dataSize;
    G->poolOfs = (int32_t)be32(G->blob + 0);
    G->projOfs = (int32_t)be32(G->blob + 4);
    G->sdirOfs = (int32_t)be32(G->blob + 8);
    G->sampPoolFileOfs = g_msmSampOfsHdr + gi->sampOfs;
    G->gid = gi->gid;
    G->grpIdx = grpIdx;
    G->baseGrpF = baseGrpF;
    G->dynBaseF = dynBaseF;
    G->loadOrder = g_grpLoadCounter++;

    if (find_group_data(G, gi->gid, &groupDataOff)) {
        resolve_fx_table(G, groupDataOff);
    } else {
        /* A group whose own GROUP_DATA list has no entry for its own gid
         * would be a song-only (sequencer) group -- loadable, just no FX. */
        printf("[AUDIO] se_group_load(%s): grpIdx=%d gid=%u has no own GROUP_DATA entry "
               "(song-only group?) -- loaded with 0 fx\n", why, grpIdx, (unsigned)gi->gid);
    }
    G->inUse = 1;

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
static void msm_se_bank_init(const char *msmPath)
{
    uint8_t hdr[0x60];
    uint8_t infoBuf[64];
    uint32_t infoOfs, seOfs, seSize, grpInfoOfs, grpInfoSize, grpDataOfs, sampOfsHdr;
    int baseGrpNum, i;
    uint8_t baseGrpIdx[32];

    mp6_grp_lock();
    g_msmReady = 0;
    free(g_seDefs); g_seDefs = NULL; g_seDefCount = 0;
    free(g_grpInfo); g_grpInfo = NULL; g_grpInfoCount = 0;
    g_baseGrpNum = 0;
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
    infoOfs    = be32(hdr + 16);
    grpInfoOfs = be32(hdr + 32); grpInfoSize = be32(hdr + 36);
    seOfs      = be32(hdr + 48); seSize      = be32(hdr + 52);
    grpDataOfs = be32(hdr + 56);
    sampOfsHdr = be32(hdr + 64);

    if (!msm_read_range(infoOfs, sizeof(infoBuf), infoBuf)) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: failed reading MSM_INFO -- SFX stay silent\n");
        return;
    }
    baseGrpNum = infoBuf[40];
    if (baseGrpNum > (int)sizeof(baseGrpIdx)) baseGrpNum = (int)sizeof(baseGrpIdx);
    for (i = 0; i < baseGrpNum; i++) baseGrpIdx[i] = infoBuf[41 + i];

    if (seSize == 0 || (seSize % 16) != 0) {
        fprintf(stderr, "[AUDIO] msm_se_bank_init: implausible SE table size %u -- SFX stay silent\n",
                (unsigned)seSize);
        return;
    }
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
        }
        free(giRaw);
        g_grpInfoCount = numGroupsTotal;
        g_msmGrpDataOfs = grpDataOfs;
        g_msmSampOfsHdr = sampOfsHdr;
        g_baseGrpNum = baseGrpNum;
        for (i = 0; i < baseGrpNum && i < (int)(sizeof(g_baseGrpIdx) / sizeof(g_baseGrpIdx[0])); i++) {
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
        g_msmReady = 1;
        mp6_grp_unlock();

        printf("[AUDIO] msm_se_bank_init: .msm bank loaded -- %d SE defs, %d/%d base group(s) resident "
               "(%u metadata bytes), %d grpInfo entries indexed for DYNAMIC loading (real stack caps "
               "A=%d B=%d), staging maxima base=%#x dyn=%#x -- SFX playback is ACTIVE\n",
               g_seDefCount, loadedBase, baseGrpNum, (unsigned)baseBytes, g_grpInfoCount,
               (int)(int8_t)infoBuf[9], (int)(int8_t)infoBuf[10],
               (unsigned)g_sampSizeMaxBase, (unsigned)g_sampSizeMaxDyn);
    }
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
            freedBytes += G->blobSize + (uint32_t)G->fxCap * (uint32_t)sizeof(MsmFxEntry);
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
        freedBytes += newest->blobSize + (uint32_t)newest->fxCap * (uint32_t)sizeof(MsmFxEntry);
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
 * (platform/host/host_win32.c), which deliberately keeps a never-
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
 * own decode-buffer allocate + msmSeStop's own free (or the mixer's own
 * auto-deactivate-on-finish path, exercised here since a one-shot SFX
 * naturally finishes on its own within one 150ms sleep for every real SFX
 * used by this game) doesn't grow unbounded under repeated real cycles,
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

    printf("[AUDIO] msmSysInit(msmPath=\"%s\", pdtPath=\"%s\", heapSize=%u)\n",
           init->msmPath ? init->msmPath : "(null)",
           init->pdtPath ? init->pdtPath : "(null)",
           (unsigned)init->heapSize);

    g_pdtReady = 0;
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
    if (g_streamMax <= 0 || packOfs < adpcmOfs || sampOfs < packOfs || adpcmOfs < packListOfs) {
        fprintf(stderr, "[AUDIO] msmSysInit: implausible .pdt header offsets -- streaming stays silent\n");
        return 0;
    }

    g_chanMax = (int)chanMaxU;
    if (g_chanMax <= 0) g_chanMax = 1;
    if (g_chanMax > MP6_MSM_MAX_CHAN) g_chanMax = MP6_MSM_MAX_CHAN;

    packListBytes = adpcmOfs - packListOfs;
    coefBytes = packOfs - adpcmOfs;
    packBytes = sampOfs - packOfs;

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
        uint8_t *rawList = (uint8_t *)malloc(packListBytes);
        uint8_t *rawCoef = (uint8_t *)malloc((size_t)g_numCoef * 32);
        BOOL ok = (rawList != NULL) && (rawCoef != NULL || g_numCoef == 0);

        if (ok) ok = pdt_read_range(packListOfs, packListBytes, rawList);
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
            g_pdtReady = 1;
            printf("[AUDIO] msmSysInit: .pdt tables loaded (%d streams, %d ADPCM coef tables, chanMax=%d) "
                   "-- real streamed-music playback is ACTIVE\n", g_streamMax, g_numCoef, g_chanMax);
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
     * clearly test-only" spirit as platform/gx/aurora_bridge.c's existing
     * MP6_AUTO_START_TICKS/--input-script mechanisms. Exists because
     * --headless's own boot flow never reaches ANY real msmStreamPlay
     * call site at all: src/game/pad.c's PadReadVSync (the ONLY call site
     * anywhere in the decomp for msmSysRegularProc, and the function that
     * actually populates HuPadBtnDown[] the warning screen's own
     * MAX_INPUT_WAIT_FRAMES=3510 wait loop reads) only ever runs via the
     * VI post-retrace callback game/pad.c's HuPadInit registers -- and
     * --headless's own VI shims (platform/null/shims_generated.c's
     * auto-generated VISetPostRetraceCallback; platform/null/
     * shims_manual.c's hand-written VIWaitForRetrace) never store or
     * invoke that callback at all. A real, pre-existing, cross-cutting
     * platform/null VI-callback gap, outside platform/audio/'s own scope
     * -- reported, not patched. This hook is what makes it possible to
     * still verify the REAL .pdt/decode/mix/loop/WAV-dump pipeline
     * end-to-end against real disc streams in --headless despite that
     * external blocker.
     *
     * The DEFAULT (aurora/windowed) build does NOT have this problem --
     * platform/gx/aurora_bridge.c's own VISetPostRetraceCallback/
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
 * is driven by platform/audio/audio_out_sdl.c's own SDL callback instead
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
void msmSysRegularProc(void)
{
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
    if (s_ticks == 1 || (s_ticks % 500) == 0) {
        printf("[AUDIO-DIAG] msmSysRegularProc tick #%llu, g_wavArmed=%d\n",
               (unsigned long long)s_ticks, g_wavArmed);
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
    uint32_t f;

    memset(out, 0, (size_t)frames * MP6_MSM_OUT_CHANNELS * sizeof(int16_t));

    mp6_lock();
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
                uint64_t over = c->posFrac - ((uint64_t)c->totalFrames << 16);
                c->posFrac = ((uint64_t)c->loopStartFrame << 16) + over;
                idx = (uint32_t)(c->posFrac >> 16);
                if (idx >= c->totalFrames) { /* pathological -- already guarded at Play
                                                 time, stay defensive rather than loop forever */
                    c->active = 0;
                    break;
                }
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
                        free(c->pcm);
                        c->pcm = NULL;
                        c->fadeAction = MP6_FADE_NONE;
                        c->fadeStep = 0.0f;
                        break; /* pcm is gone -- stop touching this channel now */
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

    /* SFX voices -- same shape as the BGM channel loop above, MONO source
     * (dual-panned into stereo via gainL/gainR) and, critically, NO loop
     * wraparound: a one-shot SFX simply deactivates itself once it runs
     * past its own totalFrames. */
    for (i = 0; i < MP6_MSM_MAX_SFX_VOICES; i++) {
        MsmSeVoice *v = &g_sfxVoice[i];
        float gain;

        if (!v->active || v->paused || !v->pcm || v->totalFrames == 0) continue;
        gain = (v->baseVol / 127.0f) * (v->vol / 127.0f) * (g_seMasterVol / 127.0f);

        for (f = 0; f < frames; f++) {
            uint32_t idx = (uint32_t)(v->posFrac >> 16);
            int32_t s, l, r;
            float fadedGain;

            if (idx >= v->totalFrames) {
                v->active = 0;
                break;
            }
            fadedGain = gain * v->fadeMul;
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
                        free(v->pcm);
                        v->pcm = NULL;
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
    mp6_unlock();

    mp6_wav_capture(out, frames);
}

/* =======================================================================
 * msmStreamPlay / Stop / Pause / PauseAll / SetParam / GetStatus / StopAll /
 * SetMasterVolume -- the game-facing stream control API (game/audio.c's
 * HuAudSStream*, HuAudBGM*, HuAudJingle* wrappers).
 * ======================================================================= */
int msmStreamPlay(int streamId, MSM_STREAMPARAM *streamParam)
{
    PdtPack pack;
    int chan;
    int stereo;
    uint32_t frames;
    int16_t *interleaved;
    int16_t *tmpL, *tmpR;
    uint32_t f;
    int vol;
    int startPaused;

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
        for (chan = 0; chan < g_chanMax; chan++) {
            if (!g_chan[chan].active) break;
        }
        if (chan == g_chanMax) return MP6_MSM_ERR_CHANLIMIT;
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
        decode_substream(pack.subSampleOfs[0], pack.subCoefIdx[0], adpcmFrames, tmpL);
        if (stereo) {
            decode_substream(pack.subSampleOfs[1], pack.subCoefIdx[1], adpcmFrames, tmpR);
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

    mp6_lock();
    if (g_chan[chan].pcm) free(g_chan[chan].pcm);
    g_chan[chan].active = 1;
    g_chan[chan].paused = startPaused;
    g_chan[chan].streamId = streamId;
    g_chan[chan].pcm = interleaved;
    g_chan[chan].totalFrames = frames;
    g_chan[chan].loopStartFrame = (pack.loopStartByte / 8) * 14;
    if (g_chan[chan].loopStartFrame >= frames) g_chan[chan].loopStartFrame = 0;
    g_chan[chan].posFrac = 0;
    {
        uint32_t nativeFrq = pack.frq ? pack.frq : MP6_MSM_OUT_RATE;
        g_chan[chan].stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    }
    g_chan[chan].baseVol = pack.vol;
    g_chan[chan].vol = vol;
    /* A freshly (re)started channel never inherits a stale fade from
     * whatever this slot was doing before -- the `free(pcm)` 2 lines above
     * already unconditionally discards any in-flight MP6_FADE_TO_STOP's
     * own target buffer, so wiping the fade state here (not left to the
     * mixer to notice) is both correct and avoids the mixer ever touching
     * a pointer this function just freed and replaced. */
    g_chan[chan].fadeAction = MP6_FADE_NONE;
    g_chan[chan].fadeStep = 0.0f;
    g_chan[chan].fadeMul = startPaused ? 0.0f : 1.0f;
    mp6_unlock();

    if (!g_wavArmed) {
        /* Arms the WAV-dump capture window (see its own header comment) --
         * "the first ~30s of rendered audio" means the first 30 seconds
         * starting from the first real note, not from process start. */
        g_wavArmed = 1;
        printf("[AUDIO] msmStreamPlay: first real playback -- MP6_AUDIO_WAV_DUMP capture window (if "
               "enabled) starts now\n");
    }

    printf("[AUDIO] msmStreamPlay: streamId=%d -> chan=%d %s frq=%uHz authoredVol=%d duration=%.1fs "
           "(%u frames) loopStart=%.1fs%s\n",
           streamId, chan, stereo ? "STEREO" : "mono", (unsigned)pack.frq, (int)pack.vol,
           (double)frames / (pack.frq ? pack.frq : MP6_MSM_OUT_RATE), (unsigned)frames,
           (double)g_chan[chan].loopStartFrame / (pack.frq ? pack.frq : MP6_MSM_OUT_RATE),
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
    if (speed <= 0) return 0.0f;
    fadeFrames = (uint32_t)(((uint64_t)(uint32_t)speed * MP6_MSM_OUT_RATE) / 1000u);
    if (fadeFrames == 0) fadeFrames = 1; /* sub-1-frame request: still a real (if
                                             tiny) fade, not a divide-by-zero */
    return 1.0f / (float)fadeFrames;
}

s32 msmStreamStop(int streamNo, s32 speed)
{
    float step;
    printf("[AUDIO] msmStreamStop(streamNo=%d, speed=%d)\n", streamNo, (int)speed);
    if (streamNo < 0 || streamNo >= g_chanMax) return MP6_MSM_ERR_RANGE_STREAM;
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    if (step <= 0.0f || !g_chan[streamNo].active || !g_chan[streamNo].pcm) {
        /* immediate -- exact previous behavior, byte-for-byte, whenever
         * speed<=0 (every existing call site) or there's nothing to fade. */
        g_chan[streamNo].active = 0;
        if (g_chan[streamNo].pcm) { free(g_chan[streamNo].pcm); g_chan[streamNo].pcm = NULL; }
        g_chan[streamNo].fadeAction = MP6_FADE_NONE;
        g_chan[streamNo].fadeStep = 0.0f;
    } else {
        /* Fade out over `speed` ms first -- mp6_msm_render's own
         * MP6_FADE_TO_STOP case deactivates + frees pcm once it completes. */
        g_chan[streamNo].paused = 0; /* still audibly rendering (fading) until the ramp completes */
        g_chan[streamNo].fadeAction = MP6_FADE_TO_STOP;
        g_chan[streamNo].fadeStep = -step;
    }
    mp6_unlock();
    return 0;
}

void msmStreamStopAll(s32 speed)
{
    int i;
    float step;
    printf("[AUDIO] msmStreamStopAll(speed=%d)\n", (int)speed);
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < g_chanMax; i++) {
        if (step <= 0.0f || !g_chan[i].active || !g_chan[i].pcm) {
            g_chan[i].active = 0;
            if (g_chan[i].pcm) { free(g_chan[i].pcm); g_chan[i].pcm = NULL; }
            g_chan[i].fadeAction = MP6_FADE_NONE;
            g_chan[i].fadeStep = 0.0f;
        } else {
            g_chan[i].paused = 0;
            g_chan[i].fadeAction = MP6_FADE_TO_STOP;
            g_chan[i].fadeStep = -step;
        }
    }
    mp6_unlock();
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
    if (!g_chan[streamNo].active) status = MSM_STREAM_DONE;
    else if (g_chan[streamNo].fadeAction == MP6_FADE_TO_PAUSE ||
             g_chan[streamNo].fadeAction == MP6_FADE_TO_STOP) status = MSM_STREAM_PAUSEIN;
    else if (g_chan[streamNo].fadeAction == MP6_FADE_TO_PLAY) status = MSM_STREAM_PAUSEOUT;
    else if (g_chan[streamNo].paused) status = MSM_STREAM_PAUSEIN;
    else status = MSM_STREAM_PLAY;
    return status;
}

void msmStreamSetMasterVolume(s32 arg0)
{
    printf("[AUDIO] msmStreamSetMasterVolume(%d)\n", (int)arg0);
    g_masterVol = (int)(arg0 & 127);
}

/* =======================================================================
 * msmSePlay / Stop / StopAll / GetStatus / PauseAll / SetParam /
 * SetMasterVolume -- the game-facing SFX control API (game/audio.c's
 * HuSePlay/HuAudFXPlay* wrappers -- see this file's own big SFX header
 * comment above msm_se_bank_init for the full bank-format writeup).
 * ======================================================================= */
static MsmSeVoice *find_sfx_voice_by_no(int seNo)
{
    int i;
    for (i = 0; i < MP6_MSM_MAX_SFX_VOICES; i++) {
        if (g_sfxVoice[i].active && g_sfxVoice[i].no == seNo) return &g_sfxVoice[i];
    }
    return NULL;
}

int msmSePlay(int seId, MSM_SEPARAM *param)
{
    MsmSeDef *def;
    int slot, i, vol, pan;
    MsmSampleInfo info;
    uint32_t adpcmFrames, byteLen;
    uint32_t sampPoolFileOfs;
    uint16_t sampleId;
    uint8_t initialPS;
    uint8_t *raw;
    int16_t *mono;
    MP6AdpcmCoefTable coef;
    MP6AdpcmState state;
    int psCheckOk;

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
        for (i = 0; i < grp->fxCount; i++) {
            if (grp->fx[i].fxId == def->fxId) { fx = &grp->fx[i]; break; }
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
        if (!lookup_sdir(grp, sampleId, &info)) {
            mp6_grp_unlock();
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
            const uint8_t *coefSub = grp->blob + grp->sdirOfs + info.coefTableRelOfs;
            const uint8_t *coefBytes = coefSub + 8;
            int p;
            for (p = 0; p < 8; p++) {
                coef.coef[p][0] = (int16_t)be16(coefBytes + (size_t)p * 4 + 0);
                coef.coef[p][1] = (int16_t)be16(coefBytes + (size_t)p * 4 + 2);
            }
            initialPS = coefSub[2];
        }
        sampPoolFileOfs = grp->sampPoolFileOfs;
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
    byteLen = adpcmFrames * 8;

    raw = (uint8_t *)malloc(byteLen);
    if (!raw) return MSM_ERR_OUTOFMEM;
    if (!msm_read_range(sampPoolFileOfs + info.offset, byteLen, raw)) {
        free(raw);
        return MSM_ERR_READFAIL;
    }

    mono = (int16_t *)malloc((size_t)adpcmFrames * 14 * sizeof(int16_t));
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

    vol = (param && (param->flag & MSM_SEPARAM_VOL)) ? param->vol : MSM_VOL_MAX;
    pan = (param && (param->flag & MSM_SEPARAM_PAN)) ? param->pan : def->pan;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;

    mp6_lock();
    for (slot = 0; slot < MP6_MSM_MAX_SFX_VOICES; slot++) {
        if (!g_sfxVoice[slot].active) break;
    }
    if (slot == MP6_MSM_MAX_SFX_VOICES) {
        mp6_unlock();
        free(mono);
        printf("[AUDIO] msmSePlay: seId=%d -- all %d SFX voice slots busy, dropped\n",
               seId, MP6_MSM_MAX_SFX_VOICES);
        return MP6_MSM_ERR_CHANLIMIT;
    }

    if (g_sfxVoice[slot].pcm) free(g_sfxVoice[slot].pcm);
    g_sfxVoice[slot].active = 1;
    g_sfxVoice[slot].paused = 0;
    g_sfxVoice[slot].pcm = mono;
    /* Play EXACTLY the authored sample count, not the whole-frame roundup
     * (`adpcmFrames * 14` -- up to 13 extra decoded padding nibbles from
     * the final partial frame; the mono[] buffer still holds them, they
     * just never play). */
    g_sfxVoice[slot].totalFrames = info.length;
    g_sfxVoice[slot].posFrac = 0;
    {
        uint32_t nativeFrq = info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE;
        g_sfxVoice[slot].stepFrac = (uint32_t)(((uint64_t)nativeFrq << 16) / MP6_MSM_OUT_RATE);
    }
    g_sfxVoice[slot].baseVol = def->vol;
    g_sfxVoice[slot].vol = vol;
    g_sfxVoice[slot].gainL = (127 - pan) / 127.0f;
    g_sfxVoice[slot].gainR = pan / 127.0f;
    g_sfxVoice[slot].no = g_seNoCounter++;
    g_sfxVoice[slot].gid = def->gid; /* msmSeStopAll(checkGrp) needs it, see MsmSeVoice */
    /* See msmStreamPlay's own identical comment on why this reset
     * belongs here, not left to the mixer. */
    g_sfxVoice[slot].fadeAction = MP6_FADE_NONE;
    g_sfxVoice[slot].fadeStep = 0.0f;
    g_sfxVoice[slot].fadeMul = 1.0f;
    mp6_unlock();

    if (!g_wavArmed) {
        g_wavArmed = 1;
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
           (int)def->vol, pan, (double)g_sfxVoice[slot].totalFrames /
           (info.sampleRateHz ? info.sampleRateHz : MP6_MSM_OUT_RATE),
           (unsigned)g_sfxVoice[slot].totalFrames, (unsigned)adpcmFrames,
           (unsigned)info.loopStart, (unsigned)info.loopLength,
           psCheckOk ? "" : " -- WARNING initialPS!=frame0-PS, offsets suspect");

    return g_sfxVoice[slot].no;
}

s32 msmSeStop(int seNo, s32 speed)
{
    MsmSeVoice *v;
    float step;
    printf("[AUDIO] msmSeStop(seNo=%d, speed=%d)\n", seNo, (int)speed);
    mp6_lock();
    v = find_sfx_voice_by_no(seNo);
    if (!v) { mp6_unlock(); return MP6_MSM_ERR_INVALIDSE; }
    step = mp6_fade_step_from_speed(speed);
    if (step <= 0.0f || !v->pcm) {
        /* immediate -- exact previous behavior (unchanged for speed<=0) */
        v->active = 0;
        if (v->pcm) { free(v->pcm); v->pcm = NULL; }
        v->fadeAction = MP6_FADE_NONE;
        v->fadeStep = 0.0f;
    } else {
        /* Same fade-then-reclaim shape as msmStreamStop above. */
        v->paused = 0;
        v->fadeAction = MP6_FADE_TO_STOP;
        v->fadeStep = -step;
    }
    mp6_unlock();
    return 0;
}

void msmSeStopAll(BOOL checkGrp, s32 speed)
{
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
    for (i = 0; i < MP6_MSM_MAX_SFX_VOICES; i++) {
        if (checkGrp) {
            int isBase = 0;
            for (j = 0; j < baseGidCount; j++) {
                if (g_sfxVoice[i].gid == baseGids[j]) { isBase = 1; break; }
            }
            if (isBase) continue; /* base-group voice -- keeps playing, real-engine behavior */
        }
        if (step <= 0.0f || !g_sfxVoice[i].pcm) {
            g_sfxVoice[i].active = 0;
            if (g_sfxVoice[i].pcm) { free(g_sfxVoice[i].pcm); g_sfxVoice[i].pcm = NULL; }
            g_sfxVoice[i].fadeAction = MP6_FADE_NONE;
            g_sfxVoice[i].fadeStep = 0.0f;
        } else {
            g_sfxVoice[i].paused = 0;
            g_sfxVoice[i].fadeAction = MP6_FADE_TO_STOP;
            g_sfxVoice[i].fadeStep = -step;
        }
    }
    mp6_unlock();
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

s32 msmSePauseAll(BOOL pause, s32 speed)
{
    int i;
    float step;
    printf("[AUDIO] msmSePauseAll(pause=%d, speed=%d)\n", (int)pause, (int)speed);
    mp6_lock();
    step = mp6_fade_step_from_speed(speed);
    for (i = 0; i < MP6_MSM_MAX_SFX_VOICES; i++) {
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
    g_seMasterVol = (int)(vol & 127);
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
           "(see platform/null/shims_manual.c's THPInit)\n");
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
