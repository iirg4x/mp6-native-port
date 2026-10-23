/* MP6 native port -- internals of the msm audio engine, shared by the
 * four translation units that implement it:
 *   msm_bridge.c    -- the mixer, stream and SFX playback, game-facing msm API
 *   msm_bank.c      -- MP6_SND.msm parsing, fx resolution, dynamic groups
 *   msm_diag.c      -- opt-in censuses, macro dump, leak-stress threads
 *   msm_savestate.c -- the savestate audio shadow
 * Not a public header. Everything declared here is owned by the mixer/group
 * locks described below; the design and format notes live in msm_bridge.c's
 * header comment. */
#ifndef MP6_MSM_INTERNAL_H
#define MP6_MSM_INTERNAL_H

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

#include "mp6_savestate.h" /* W3: Mp6SsAudioShadow -- the audio re-sync shadow */
#include "mp6_boot.h"      /* mp6_tick_count -- the regular-proc diag keys off the GAME tick (see its comment) */
#include "mp6_events.h"    /* se.play -- "the UI just reacted to input", see msmSePlay */
#include "mp6_diag_probe.h" /* mp6_diag_audio_snapshot -- lock-copy, format outside */
#include "mp6_console.h"    /* the MP6_AUDIO_TIMELINE runtime lever */
#include "mp6_enhancements.h" /* mp6_enh_sfx_voices -- the 16/32 voice-table size */
#include "mp6_load_trace.h"   /* MP6_LOAD_TRACE: group loads and stream decode */

/* ---- error codes not visible via the msm TUs' preamble (top-level
 * "msm.h", the SAME header tools/gen_shims.py's probe resolves for these
 * symbols -- see msm_bridge.c's header comment on why matching that
 * exact preamble matters). include/game/msm.h has these two, transcribed
 * directly. */
#define MP6_MSM_ERR_RANGE_STREAM (-140)
#define MP6_MSM_ERR_CHANLIMIT    (-110)

/* Real header value (include/game/msm_data.h's MSM_PDT_FILE_VERSION) --
 * transcribed locally rather than pulling in game/msm_data.h (the msm TUs'
 * preamble deliberately mirrors shims_generated.c's exactly, see
 * msm_bridge.c's header comment). */
#define MSM_PDT_FILE_VERSION_LOCAL 1u

/* See msm_bridge.c's header comment, "FLAG BIT GROUND TRUTH". */
#define MP6_PACK_FLAG_STEREO 0x1
#define MP6_PACK_FLAG_LOOP   0x2

/* Not visible via the msm TUs' resolved "msm.h" (mirrors shims_generated.c's
 * preamble -- resolves to the TOP-LEVEL include/msm.h, not include/game/msm.h;
 * see msm_bridge.c's header comment's discussion of the two competing headers).
 * MSM_ERR_CHANLIMIT/RANGE_STREAM above have this exact same gap; value
 * transcribed directly from include/game/msm.h. */
#define MP6_MSM_ERR_INVALIDSE (-111)

/* Same "not visible via the resolved msm.h" gap as the two
 * above; values transcribed directly from include/game/msm.h (lines
 * 51-52). MSM_ERR_64 is the real engine's own "bad group id" result
 * (msmSysLoadGroupBase's grpId<1 / >=grpMax reject); STACK_OVERFLOW is
 * its "no free group slot". */
#define MP6_MSM_ERR_64             (-100)
#define MP6_MSM_ERR_STACK_OVERFLOW (-101)

/* Game-facing entry points the resolved top-level msm.h does not declare but
 * which another msm TU calls (the savestate re-sync in msm_savestate.c). */
s32 msmSysLoadGroupBase(s32 grpId, void *buf);
void msmStreamSetMasterVolume(s32 arg0);
void msmSeSetMasterVolume(s32 vol);

#define MP6_MSM_MAX_CHAN 8
#define MP6_RENDER_SCRATCH_FRAMES 4096
#define MP6_WAV_CAP_SECONDS 30
#define MP6_WAV_CAP_FRAMES (MP6_MSM_OUT_RATE * MP6_WAV_CAP_SECONDS)

/* =======================================================================
 * .pdt directory state -- parsed once by msmSysInit, kept for the whole
 * process lifetime (a few KB: streamMax*4 + numCoef*32 + packBytes bytes;
 * the real file's own numbers are ~7.5KB total).
 * ======================================================================= */
extern int g_pdtReady;
extern int g_streamMax;
extern int g_chanMax;
extern int g_masterVol;

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
 * PCM, not an incremental ring buffer -- see msm_bridge.c's header comment.
 * ======================================================================= */
typedef struct {
    int active;
    int paused;
    int loop;
    int streamId;
    int16_t *pcm;             /* interleaved stereo, malloc'd, totalFrames*2 samples */
    uint32_t totalFrames;
    uint32_t decodedFrames;    /* frames of pcm decoded so far (streamed decode, msm_bridge.c) */
    uint32_t loopStartFrame;
    uint64_t posFrac;          /* Q16.16 position within pcm, in the stream's OWN native-rate frames */
    uint32_t stepFrac;         /* Q16.16 per-output-sample advance = (nativeFrq<<16)/MP6_MSM_OUT_RATE */
    int baseVol;                /* 0-127, authored (pack->vol) */
    int vol;                    /* 0-127, runtime (MSM_STREAMPARAM.vol, default MSM_VOL_MAX) */
    /* Fade envelope (see the enum comment above and msm_bridge.c's
     * header comment, "Fade envelopes" -- mp6_msm_render applies these). */
    float fadeMul;               /* current envelope multiplier, always in [0,1] */
    float fadeStep;              /* per-output-frame delta; 0 = no fade in progress */
    int fadeAction;              /* one of the MP6_FADE_* values above */
} MsmChan;

extern MsmChan g_chan[MP6_MSM_MAX_CHAN];

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
 *   pad -- the engine only reads gid/fxId/vol/pan (matching the dry
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
 *     msm_resolve_first_sample), but a multi-row layer still starts only its
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

extern int g_msmReady;
extern uint32_t g_msmFileSize;
extern int g_seMasterVol;

typedef struct {
    uint16_t gid, fxId;
    int8_t vol, pan;
} MsmSeDef;

extern MsmSeDef *g_seDefs;
extern int g_seDefCount;

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

extern MsmGrpInfo *g_grpInfo;
extern int g_grpInfoCount;
extern int g_baseGrpIdx[32];      /* MSM_INFO.baseGrp[] (grpInfo indices) */
extern int g_baseGrpNum;

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
    uint8_t *samplePool; /* the pool's ADPCM bytes [sampPoolSize] -- what the real engine
                          * DMAs to ARAM -- so starting a sound copies its sample from
                          * memory instead of reading the disc. Filled in the ticks after
                          * the group loads (msm_se_pool_fill), front to back, so a load
                          * does not wait for megabytes; poolFilled bytes are in. NULL
                          * for a group materialized without it (savestate preflight) or
                          * when a read failed; msm_read_sample then reads the disc.
                          * malloc'd, freed on unload. */
    uint32_t poolFilled;
    int16_t **decoded;   /* [sdirCount]: long samples' PCM (adpcmFrames*14, decoded from
                          * zero history exactly as msmSePlay would), filled a slice per
                          * tick by msm_se_decode_ahead; NULL entries are not (yet)
                          * decoded. Only with samplePool. malloc'd, freed on unload. */
    uint32_t sdirCount;
    uint32_t decodeEntry; /* decode-ahead cursor: SDIR entry in progress, */
    uint32_t decodeFrame; /* ADPCM frames of it done, */
    int16_t *decodeBuf;   /* its buffer until complete, */
    int16_t decodeHist[2];/* and the decoder history between slices */
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

extern MsmSeGroup g_seGroups[MP6_MSM_MAX_SE_GROUPS];

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
    uint32_t sdirIndex;       /* the entry's position in its group's SDIR table */
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
     * that struct's own comment and msm_bridge.c's header comment. */
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

extern MsmSeVoice g_sfxVoice[MP6_MSM_MAX_SFX_VOICES];
extern int g_seNoCounter;

/* Read the latch, resolving it on first use if msmSysInit has not run yet.
 * The lazy path exists only so a caller that runs before init (nothing does
 * today -- msmSePlay needs g_msmReady and the mixer needs g_mixerLockInit)
 * cannot read a zero cap and silently behave as if there were no voices. The
 * store is idempotent: mp6_enh_sfx_voices() is a pure function of the
 * process environment/config, so a benign race writes the same value. */
int mp6_sfx_voice_cap(void);

extern Mp6Mutex g_mixerLock; /* The host seam's caller-storage mutex (win32
                              * backend is the same CRITICAL_SECTION inside) */
extern int g_mixerLockInit;
/* Restore-only render gate.  SFX replay decodes outside g_mixerLock, so the
 * callback must emit silence until every recreated voice has had its exact
 * slot/position/fade/handle published; otherwise it can play frame zero in
 * the short window after msmSePlay publishes the temporary voice. */
extern int g_savestateMixerMuted;

void mp6_lock(void);
void mp6_unlock(void);

/* A control transaction detaches at most every voice/channel plus one
 * replacement. Ownership moves under the mixer lock; heap work never does. */
typedef struct {
    int16_t *pcm[MP6_MSM_MAX_SFX_VOICES + MP6_MSM_MAX_CHAN + 1];
    size_t count;
} MsmPcmRetirement;

void mp6_pcm_detach(MsmPcmRetirement *retired, int16_t **pcm);

void mp6_pcm_release(MsmPcmRetirement *retired);

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
extern Mp6Mutex g_grpLock; /* Same CRITICAL_SECTION-backed mutex as g_mixerLock above */

void mp6_grp_lock(void);
void mp6_grp_unlock(void);

/* Everything ONE macro walk recovers: the note length, the volume envelope,
 * the key group, and the diagnostic bitmasks explaining each. Filled by msm_bank.c's
 * find_macro_first_sample, whose header comment is the ground truth for every
 * field; shared here because msm_diag.c's censuses read it too. */
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

/* ---- defined in msm_bridge.c */
void mp6_msm_stream_decode_ahead(void);
void mp6_msm_stream_finish_decode(int ch);
BOOL msm_audio_file_size(const char *path, uint32_t *sizeOut);
BOOL msm_pdt_get_pack(int streamId, PdtPack *out);
uint32_t msm_pdt_pack_frame_count(const PdtPack *pack);
MsmSeVoice *find_sfx_voice_by_no(int seNo);

/* ---- defined in msm_bank.c */
BOOL msm_read_range(uint32_t offset, uint32_t length, void *dst);
/* Game thread, once per tick (msmSysRegularProc): read the next piece of
 * the loaded groups' sample pools, then decode the next slice of their long
 * samples into MsmSeGroup.decoded. */
void msm_se_pool_fill(void);
void msm_se_decode_ahead(void);
/* Copies [offset, offset+length) of loaded group gid's sample pool (grpIdx
 * must still be the group in that slot) into dst, from its resident pool, or
 * from the disc at poolFileOfs when the pool is not resident. Takes
 * g_grpLock itself. */
BOOL msm_read_sample(uint16_t gid, int grpIdx, uint32_t poolFileOfs, uint32_t offset,
                     uint32_t length, void *dst);
/* A malloc'd copy of loaded group gid's (slot grpIdx) SDIR entry sdirIndex as
 * decoded ahead (adpcmFrames*14 samples), with the sample's first ADPCM byte
 * in *firstByte; NULL when it has not been decoded ahead. Takes g_grpLock. */
int16_t *msm_copy_decoded(uint16_t gid, int grpIdx, uint32_t sdirIndex, uint32_t adpcmFrames,
                          uint8_t *firstByte);
BOOL msm_group_span(MsmSeGroup *grp, uint64_t offset, uint64_t size, const char *what);
const Mp6MsmSeEnv *msm_se_group_fx_env(const MsmSeGroup *grp, int fxIndex);
BOOL msm_find_group_data(MsmSeGroup *grp, uint16_t targetGid, uint32_t *outOff);
const uint8_t *msm_pool_find_node(MsmSeGroup *grp, uint32_t listOfsField, uint16_t nodeId);
BOOL msm_resolve_first_sample(MsmSeGroup *grp, uint16_t objId, int key, int depth,
                          uint16_t *outSampleId, MsmMacroScan *outLife);
int mp6_se_macro_env_disabled(void);
void mp6_se_install_env(const Mp6MsmSeEnv *env, const MsmSampleInfo *info,
                        int loopMode, Mp6MsmEnvPlan *out);
uint32_t mp6_se_authored_note_frames(const MsmSampleInfo *info, uint32_t lifeMs,
                                     int loopMode, uint32_t loopEndFrame);
BOOL msm_lookup_sdir(MsmSeGroup *grp, uint16_t sampleId, MsmSampleInfo *out);
MsmSeGroup *msm_se_group_find_by_gid(uint16_t gid);
void msm_se_group_loaded_gids_str(char *buf, size_t bufSize);
void msm_se_group_unload(MsmSeGroup *G);
s32 msm_se_group_materialize(MsmSeGroup *G, int grpIdx, int baseGrpF,
                         int dynBaseF, const char *why);
void msm_se_bank_init(const char *msmPath);

/* ---- defined in msm_diag.c */
void mp6_se_loop_census(void);
void mp6_env_program_str(const Mp6MsmSeEnv *env, char *buf, size_t bufSize);
void mp6_se_env_census(void);
void mp6_se_macro_dump(void);
void mp6_grpswap_leaktest_maybe_start(void);
void mp6_se_leaktest_maybe_start(void);
void mp6_leaktest_maybe_start(void);
void mp6_se_timeline_census(void);

#endif /* MP6_MSM_INTERNAL_H */
