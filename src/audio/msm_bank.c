/* MP6 native port -- MP6_SND.msm sound-effect bank: header/SE-table
 * parsing, per-group fx->sample resolution (the macro walk that recovers each
 * fx's sample, authored note length, key group and volume envelope), and the
 * dynamic group load/unload core behind msmSysLoadGroup & co.
 *
 * Part of the msm engine split across msm_bridge.c (mixer, streams, SFX
 * playback, game-facing API), msm_bank.c (this file), msm_diag.c (opt-in
 * diagnostics/stress hooks) and msm_savestate.c. Shared state and helpers are
 * declared in msm_internal.h; the format writeup lives with the MsmSeGroup
 * and MsmFxEntry declarations there. */

#include "msm_internal.h"
#include "mp6_dvd_files.h" /* mp6_dvd_prefetch_range: a group's sample pool is fetched ahead */
#include "mp6_host_section.h" /* savestate carve-out -- see msm_bridge.c */

static char g_msmPath[512];
int g_msmReady;
uint32_t g_msmFileSize;
MsmSeDef *g_seDefs;
int g_seDefCount;
MsmGrpInfo *g_grpInfo;
int g_grpInfoCount;
static uint32_t g_msmGrpDataOfs;  /* header.grpDataOfs */
static uint32_t g_msmSampOfsHdr;  /* header.sampOfs */
int g_baseGrpIdx[32];
int g_baseGrpNum;
static s32 g_sampSizeMaxBase;     /* max base-group sampSize == real sys.sampSize */
static s32 g_sampSizeMaxDyn;      /* max NON-base sampSize == real sys.sampSizeBase */

static int g_grpLoadMode;         /* msmSysSetGroupLoadMode value -- recorded + logged;
                                     this port's flat slot table behaves identically in
                                     both modes (no ARAM stack layout to juggle) */

MsmSeGroup g_seGroups[MP6_MSM_MAX_SE_GROUPS];
static int g_grpLoadCounter = 1;
Mp6Mutex g_grpLock;

void mp6_grp_lock(void) { mp6_host_mutex_lock(&g_grpLock); }

void mp6_grp_unlock(void) { mp6_host_mutex_unlock(&g_grpLock); }

/* =======================================================================
 * MP6_SND.msm bank access + fx->sample resolution. See the
 * MsmSeGroup/MsmFxEntry declarations above for the full format writeup.
 * ======================================================================= */
BOOL msm_read_range(uint32_t offset, uint32_t length, void *dst)
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

BOOL msm_read_sample(uint16_t gid, int grpIdx, uint32_t poolFileOfs, uint32_t offset,
                     uint32_t length, void *dst)
{
    MsmSeGroup *G;
    mp6_grp_lock();
    G = msm_se_group_find_by_gid(gid);
    if (G != NULL && G->grpIdx == grpIdx && G->samplePool != NULL &&
        mp6_msm_span_valid_u32(G->poolFilled, offset, length)) {
        memcpy(dst, G->samplePool + offset, length);
        mp6_grp_unlock();
        return TRUE;
    }
    mp6_grp_unlock();
    return msm_read_range(poolFileOfs + offset, length, dst);
}

/* =======================================================================
 * Sound-effect decode-ahead.
 *
 * Starting a sound decodes its whole sample on the game thread; a long one
 * (a board event's 4 s sting is 131k samples) took up to 3.9 ms on the S22+
 * at the moment it played. Once a group's sample pool is in memory, its
 * samples of at least MP6_SE_DECODE_AHEAD_MIN frames are decoded ahead, a
 * slice of MP6_SE_DECODE_AHEAD_SLICE ADPCM frames per tick, into
 * MsmSeGroup.decoded; msmSePlay copies a decoded sample instead of decoding
 * it. Shorter samples decode at play time as before (well under 0.5 ms).
 * Same decoder, same zero starting history, so the PCM is identical. About
 * 12 MB for the five base groups.
 * ======================================================================= */
#define MP6_SE_DECODE_AHEAD_MIN 32768u
#define MP6_SE_DECODE_AHEAD_SLICE 2048u

/* The coefficient table of an SDIR entry, or FALSE when it is not a
 * decodable ADPCM sample of the pool. */
static BOOL msm_sdir_adpcm(MsmSeGroup *G, uint32_t entry, uint32_t *offsetOut,
                           uint32_t *framesOut, MP6AdpcmCoefTable *coef)
{
    const uint8_t *s = G->blob + G->sdirOfs + (size_t)entry * 32;
    uint32_t hdrLength = be32(s + 0x10);
    uint32_t length = hdrLength & 0xFFFFFFu;
    uint32_t offset = be32(s + 4);
    uint64_t coefOff = (uint64_t)G->sdirOfs + be32(s + 0x1C);
    uint32_t frames = (length + 13) / 14;
    const uint8_t *coefBytes;
    int p;
    if ((hdrLength >> 24) != 0 || length < MP6_SE_DECODE_AHEAD_MIN ||
        !mp6_msm_span_valid_u32(G->sampPoolSize, offset, frames * 8) ||
        coefOff + 40 > G->blobSize || be16(G->blob + (size_t)coefOff) != 8u) {
        return FALSE;
    }
    coefBytes = G->blob + (size_t)coefOff + 8;
    for (p = 0; p < 8; p++) {
        coef->coef[p][0] = (int16_t)be16(coefBytes + (size_t)p * 4 + 0);
        coef->coef[p][1] = (int16_t)be16(coefBytes + (size_t)p * 4 + 2);
    }
    *offsetOut = offset;
    *framesOut = frames;
    return TRUE;
}

/* Bytes of sample pool read per tick, across all groups: the five base
 * groups' 6.2 MB are in within about 12 ticks of boot. */
#define MP6_SE_POOL_FILL_PER_TICK (512u * 1024u)

void msm_se_pool_fill(void)
{
    uint32_t budget = MP6_SE_POOL_FILL_PER_TICK;
    int g;
    mp6_grp_lock();
    for (g = 0; g < MP6_MSM_MAX_SE_GROUPS && budget > 0; g++) {
        MsmSeGroup *G = &g_seGroups[g];
        uint32_t n;
        if (!G->inUse || G->samplePool == NULL || G->poolFilled >= G->sampPoolSize) continue;
        n = G->sampPoolSize - G->poolFilled;
        if (n > budget) n = budget;
        if (!msm_read_range(G->sampPoolFileOfs + G->poolFilled, n, G->samplePool + G->poolFilled)) {
            free(G->samplePool); /* sounds of this group read the disc instead */
            G->samplePool = NULL;
            G->poolFilled = 0;
            continue;
        }
        G->poolFilled += n;
        budget -= n;
    }
    mp6_grp_unlock();
}

void msm_se_decode_ahead(void)
{
    uint32_t budget = MP6_SE_DECODE_AHEAD_SLICE;
    int g;
    mp6_grp_lock();
    for (g = 0; g < MP6_MSM_MAX_SE_GROUPS && budget > 0; g++) {
        MsmSeGroup *G = &g_seGroups[g];
        /* Only once the whole pool is in: entries are visited once. */
        if (!G->inUse || G->decoded == NULL || G->samplePool == NULL ||
            G->poolFilled < G->sampPoolSize) {
            continue;
        }
        while (budget > 0 && G->decodeEntry < G->sdirCount) {
            MP6AdpcmCoefTable coef;
            MP6AdpcmState state;
            uint32_t offset, frames, n;
            if (G->decoded[G->decodeEntry] != NULL ||
                !msm_sdir_adpcm(G, G->decodeEntry, &offset, &frames, &coef)) {
                G->decodeEntry++;
                continue;
            }
            if (G->decodeBuf == NULL) {
                G->decodeBuf = (int16_t *)malloc((size_t)frames * 14u * sizeof(int16_t));
                if (G->decodeBuf == NULL) { /* try again next tick */
                    budget = 0;
                    break;
                }
                G->decodeFrame = 0;
                G->decodeHist[0] = G->decodeHist[1] = 0;
            }
            n = frames - G->decodeFrame;
            if (n > budget) n = budget;
            state.hist1 = G->decodeHist[0];
            state.hist2 = G->decodeHist[1];
            mp6_dspadpcm_decode(G->samplePool + offset + (size_t)G->decodeFrame * 8, n, &coef, &state,
                                G->decodeBuf + (size_t)G->decodeFrame * 14u);
            G->decodeHist[0] = state.hist1;
            G->decodeHist[1] = state.hist2;
            G->decodeFrame += n;
            budget -= n;
            if (G->decodeFrame == frames) {
                G->decoded[G->decodeEntry++] = G->decodeBuf;
                G->decodeBuf = NULL;
            }
        }
    }
    mp6_grp_unlock();
}

int16_t *msm_copy_decoded(uint16_t gid, int grpIdx, uint32_t sdirIndex, uint32_t adpcmFrames,
                          uint8_t *firstByte)
{
    MsmSeGroup *G;
    int16_t *copy = NULL;
    uint32_t offset, frames;
    MP6AdpcmCoefTable coef;
    mp6_grp_lock();
    G = msm_se_group_find_by_gid(gid);
    if (G != NULL && G->grpIdx == grpIdx && G->decoded != NULL && sdirIndex < G->sdirCount &&
        G->decoded[sdirIndex] != NULL &&
        msm_sdir_adpcm(G, sdirIndex, &offset, &frames, &coef) && frames == adpcmFrames) {
        size_t bytes = (size_t)frames * 14u * sizeof(int16_t);
        copy = (int16_t *)malloc(bytes);
        if (copy != NULL) {
            memcpy(copy, G->decoded[sdirIndex], bytes);
            *firstByte = G->samplePool[offset];
        }
    }
    mp6_grp_unlock();
    return copy;
}

BOOL msm_group_span(MsmSeGroup *grp, uint64_t offset, uint64_t size, const char *what)
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
const Mp6MsmSeEnv *msm_se_group_fx_env(const MsmSeGroup *grp, int fxIndex)
{
    int i;
    for (i = 0; i < grp->fxEnvCount; i++) {
        if (grp->fxEnv[i].fxIndex == fxIndex) return &grp->fxEnv[i].env;
    }
    return NULL;
}

/* GROUP_DATA linked list @ blob+projOfs -- nextOff is BASE-relative to
 * projOfs (see msm_internal.h's MsmSeGroup format comment). Finds the entry whose own
 * `id` field matches targetGid; *outOff receives its offset (relative to
 * blob+projOfs, i.e. the absolute address is blob+projOfs+*outOff). */
BOOL msm_find_group_data(MsmSeGroup *grp, uint16_t targetGid, uint32_t *outOff)
{
    uint32_t off = 0;
    int guard = 0;

    for (;;) {
        uint64_t abs = (uint64_t)grp->projOfs + off;
        const uint8_t *g;
        if (!msm_group_span(grp, abs, 0x28, "GROUP_DATA")) return FALSE;
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
 * macros -- see msm_resolve_first_sample below. listOfsField is one of
 * 0/4/8/0xC; a 0 stored offset means "this group has no such list" (e.g.
 * gid 176's keymapOff/curveOff are both 0). Node ids are stored WITH
 * their type tag (s_data.c's InsertData does `id |= 0x4000/0x8000` before
 * the lookup). */
const uint8_t *msm_pool_find_node(MsmSeGroup *grp, uint32_t listOfsField, uint16_t nodeId)
{
    const uint8_t *poolBase;
    uint32_t off;
    int guard = 0;

    if (listOfsField > 0xC || !msm_group_span(grp, grp->poolOfs, 16, "POOL_DATA")) return NULL;
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
        if (!msm_group_span(grp, abs, 8, "MEM_DATA")) return NULL;
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
    const uint8_t *m = msm_pool_find_node(grp, 0 /* POOL_DATA.macroOff */, macroId);
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
             * must be silent and side-effect free: msm_group_span sets the
             * group's sticky parseError, which would reject an otherwise
             * perfectly loadable group just because a macro's step array
             * runs to the very end of the blob. Stop quietly instead --
             * the fx entry is already earned, only lifeMs is lost. */
            if (stepOff > grp->blobSize || (uint64_t)8 > (uint64_t)grp->blobSize - stepOff) {
                sc.why |= MP6_LIFEWHY_NOEND;
                break;
            }
        } else if (!msm_group_span(grp, stepOff, 8, "macro MSTEP")) {
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
BOOL msm_resolve_first_sample(MsmSeGroup *grp, uint16_t objId, int key, int depth,
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
        const uint8_t *m = msm_pool_find_node(grp, 8 /* POOL_DATA.keymapOff */, objId);
        const uint8_t *entry;
        uint16_t subId;
        int8_t transpose;
        if (!m) return FALSE;
        entry = m + 8 + (size_t)(key & 0x7f) * 8;
        if (!msm_group_span(grp, (uint64_t)(entry - grp->blob), 8, "KEYMAP entry")) return FALSE;
        subId = (uint16_t)be16(entry + 0);
        transpose = (int8_t)entry[2];
        if (subId == 0xFFFF || (subId & 0xC000) == 0x4000) return FALSE; /* StartKeymap's own guard */
        return msm_resolve_first_sample(grp, subId, key + transpose, depth - 1, outSampleId,
                                    outLife);
    }

    case 0x8000: { /* LAYER */
        const uint8_t *m = msm_pool_find_node(grp, 0xC /* POOL_DATA.layerOff */, objId);
        uint32_t num, i;
        int matched = 0;
        BOOL resolved = FALSE;
        if (!m) return FALSE;
        if (!msm_group_span(grp, (uint64_t)(m - grp->blob), 12, "LAYER header")) return FALSE;
        num = be32(m + 8);
        if (num > 128) {
            grp->parseError = 1;
            return FALSE; /* implausible -- refuse to walk garbage */
        }
        if (!msm_group_span(grp, (uint64_t)(m - grp->blob) + 12,
                        (uint64_t)num * 12, "LAYER rows")) return FALSE;
        for (i = 0; i < num; i++) {
            const uint8_t *row = m + 12 + (size_t)i * 12;
            uint16_t subId = (uint16_t)be16(row + 0);
            uint8_t keyLow = row[2], keyHigh = row[3];
            int8_t transpose = (int8_t)row[4];
            if (subId == 0xFFFF || key < keyLow || key > keyHigh) continue;
            matched++;
            if (!resolved) {
                resolved = msm_resolve_first_sample(grp, subId, key + transpose, depth - 1,
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

    if (!msm_group_span(grp, grp->projOfs, 1, "project base") ||
        !msm_group_span(grp, (uint64_t)grp->projOfs + groupDataOff, 0x28, "FX GROUP_DATA")) return;
    prjBase = grp->blob + grp->projOfs;
    g = prjBase + groupDataOff;
    type = (uint16_t)be16(g + 6);
    if (type != 1) return; /* song/sequencer group, not an FX group -- no FX_DATA to read */
    fxTableOff = be32(g + 0x1C);
    if (!msm_group_span(grp, (uint64_t)grp->projOfs + fxTableOff, 4, "FX_DATA header")) return;
    fxBase = prjBase + fxTableOff;
    fxNum = (uint16_t)be16(fxBase + 0);
    if (!msm_group_span(grp, (uint64_t)grp->projOfs + fxTableOff + 4,
                    (uint64_t)fxNum * 10, "FX_DATA entries")) return;
    for (i = 0; i < fxNum; i++) {
        const uint8_t *e = fxBase + 4 + (size_t)i * 10;
        uint16_t fid = (uint16_t)be16(e + 0);
        uint16_t objId = (uint16_t)be16(e + 2); /* macro OR keymap/layer -- see msm_resolve_first_sample */
        uint8_t key = e[8];                     /* FX_TAB.key -- selects keymap entries/layer rows */
        uint16_t sampleId;
        MsmMacroScan life;
        memset(&life, 0, sizeof(life));
        if (msm_resolve_first_sample(grp, objId, key, 4, &sampleId, &life)) {
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
int mp6_se_macro_env_disabled(void)
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
void mp6_se_install_env(const Mp6MsmSeEnv *env, const MsmSampleInfo *info,
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
uint32_t mp6_se_authored_note_frames(const MsmSampleInfo *info, uint32_t lifeMs,
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
BOOL msm_lookup_sdir(MsmSeGroup *grp, uint16_t sampleId, MsmSampleInfo *out)
{
    uint32_t off = 0;

    for (;;) {
        uint64_t abs = (uint64_t)grp->sdirOfs + off;
        const uint8_t *s;
        if (!msm_group_span(grp, abs, 2, "SDIR terminator/entry")) return FALSE;
        s = grp->blob + grp->sdirOfs;
        uint16_t id = (uint16_t)be16(s + off + 0);
        if (id == 0xFFFFu) return FALSE;
        if (!msm_group_span(grp, abs, 32, "SDIR entry")) return FALSE;
        if (id == sampleId) {
            uint32_t hdrInfo = be32(s + off + 0xC);
            out->sdirIndex = off / 32;
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

        if (!msm_group_span(grp, abs, 2, "SDIR terminator/entry")) return FALSE;
        s = grp->blob + (size_t)abs;
        id = (uint16_t)be16(s + 0);
        if (id == 0xFFFFu) return TRUE;
        if (!msm_group_span(grp, abs, 32, "SDIR entry")) return FALSE;

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
            if (!msm_group_span(grp, coefOff, 40, "ADPCM coefficient block")) return FALSE;
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
MsmSeGroup *msm_se_group_find_by_gid(uint16_t gid)
{
    int i;
    for (i = 0; i < MP6_MSM_MAX_SE_GROUPS; i++) {
        if (g_seGroups[i].inUse && g_seGroups[i].gid == gid) return &g_seGroups[i];
    }
    return NULL;
}

/* g_grpLock must be held. Formats "gid,gid,..." of every loaded group into
 * buf (for the msmSePlay unresolved-id diagnostic + load/unload logs). */
void msm_se_group_loaded_gids_str(char *buf, size_t bufSize)
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
 * msm_se_group_unload frees. One helper because three call sites report this
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
void msm_se_group_unload(MsmSeGroup *G)
{
    uint32_t i;
    for (i = 0; G->decoded != NULL && i < G->sdirCount; i++) free(G->decoded[i]);
    free(G->decoded);
    free(G->decodeBuf);
    free(G->blob);
    free(G->samplePool);
    free(G->fx);
    free(G->fxEnv);
    memset(G, 0, sizeof(*G));
}

/* Materialize one grpInfo entry into caller-owned storage without publishing
 * it in g_seGroups.  Runtime loads use this and then publish a slot; savestate
 * preflight uses the same parser on a temporary group to validate an active
 * voice's exact sample/loop bounds even when its captured scene group is not
 * resident in the later live process. */
s32 msm_se_group_materialize(MsmSeGroup *G, int grpIdx, int baseGrpF,
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
        msm_se_group_unload(G);
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
        !msm_group_span(G, G->poolOfs, 16, "POOL_DATA") ||
        !msm_group_span(G, G->projOfs, 0x28, "project GROUP_DATA") ||
        !msm_group_span(G, G->sdirOfs, 2, "SDIR table") ||
        !validate_sdir(G)) {
        msm_se_group_unload(G);
        return MSM_ERR_INVALIDFILE;
    }
    if (msm_find_group_data(G, gi->gid, &groupDataOff)) {
        resolve_fx_table(G, groupDataOff);
    }
    if (G->parseError) {
        msm_se_group_unload(G);
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

    existing = msm_se_group_find_by_gid(gi->gid);
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
    {
        uint64_t traceStart = mp6_load_trace_begin();
        materializeResult = msm_se_group_materialize(G, grpIdx, baseGrpF, dynBaseF, why);
        if (materializeResult == 0 && G->sampPoolSize != 0 &&
            mp6_msm_span_valid_u32(g_msmFileSize, G->sampPoolFileOfs, G->sampPoolSize)) {
            /* The sample pool comes in with the group, as the real engine
             * loads it into ARAM, but over the next ticks (msm_se_pool_fill)
             * rather than inside this load; where reads are round trips (the
             * web) the whole range is fetched ahead now. Until a sample's
             * bytes are in, starting it reads the disc as before. */
            G->samplePool = (uint8_t *)malloc(G->sampPoolSize);
            G->poolFilled = 0;
            if (G->samplePool != NULL) {
                mp6_dvd_prefetch_range(g_msmPath, G->sampPoolFileOfs, G->sampPoolSize);
            }
        }
        if (G->samplePool != NULL) {
            const uint8_t *sdir = G->blob + G->sdirOfs;
            while (be16(sdir + (size_t)G->sdirCount * 32) != 0xFFFFu) G->sdirCount++; /* validate_sdir walked it */
            G->decoded = (int16_t **)calloc(G->sdirCount ? G->sdirCount : 1, sizeof(int16_t *));
        }
        mp6_load_trace_end(traceStart, MP6_LOAD_SEGROUP, gi->dataSize);
    }
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

void msm_se_bank_init(const char *msmPath)
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
        if (g_seGroups[i].inUse) msm_se_group_unload(&g_seGroups[i]);
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

    if (!msm_audio_file_size(g_msmPath, &g_msmFileSize) || g_msmFileSize < sizeof(hdr)) {
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
                if (g_seGroups[i].inUse) msm_se_group_unload(&g_seGroups[i]);
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
            msm_se_group_unload(G);
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
        msm_se_group_unload(newest);
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
