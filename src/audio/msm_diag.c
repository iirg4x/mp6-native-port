/* MP6 native port -- opt-in msm audio diagnostics: the disc-wide
 * one-shot/loop and volume-envelope censuses, the raw macro dumper, the
 * MP6_AUDIO_LEAKTEST_* stress threads and the MP6_AUDIO_TIMELINE per-tick
 * census. Every entry point here is gated by its own environment variable
 * (or console lever) and is read-only with respect to playback.
 *
 * Part of the msm engine -- see msm_internal.h and msm_bridge.c. */

#include "msm_internal.h"
#include "mp6_host_section.h" /* savestate carve-out -- see msm_bridge.c */

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
 * load uses (msm_se_group_materialize into caller-owned storage -- never
 * published in g_seGroups), so it cannot disagree with what msmSePlay will
 * actually do. */
void mp6_se_loop_census(void)
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
        if (msm_se_group_materialize(&tmp, gi, 0, 0, "census") != 0) {
            printf("[SECENSUS] grpIdx=%d gid=%u UNPARSEABLE -- skipped\n", gi, (unsigned)gid);
            continue;
        }
        /* An earlier grpInfo entry may already carry this gid (the directory
         * has duplicates); attribute each seId to the FIRST gid match only,
         * which is what msm_se_group_find_by_gid would hand msmSePlay. */
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
            if (!fx || !msm_lookup_sdir(&tmp, fx->sampleId, &info) || info.compType != 0) continue;
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
        msm_se_group_unload(&tmp);
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

void mp6_env_program_str(const Mp6MsmSeEnv *env, char *buf, size_t bufSize)
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

void mp6_se_env_census(void)
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
        if (msm_se_group_materialize(&tmp, gi, 0, 0, "envcensus") != 0) continue;
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
            if (!fx || !msm_lookup_sdir(&tmp, fx->sampleId, &info) || info.compType != 0) continue;
            resolved++;
            /* Resolved the way msmSePlay resolves it -- by fx INDEX, with an
             * absent row meaning segCount 0 -- so a census verdict cannot
             * disagree with the program playback would actually install. */
            memset(&env, 0, sizeof(env));
            envp = msm_se_group_fx_env(&tmp, fxIdx);
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
        msm_se_group_unload(&tmp);
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
 * timing opcodes by eye. Mirrors msm_resolve_first_sample's own object-id dispatch
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
        m = msm_pool_find_node(grp, 8, objId);
        if (!m) { printf("[SEMACRO]   keymap %#x NOT FOUND\n", (unsigned)objId); return; }
        entry = m + 8 + (size_t)(key & 0x7f) * 8;
        if (!msm_group_span(grp, (uint64_t)(entry - grp->blob), 8, "KEYMAP entry")) return;
        subId = (uint16_t)be16(entry + 0);
        printf("[SEMACRO]   keymap %#x key=%d -> %#x transpose=%d\n", (unsigned)objId, key,
               (unsigned)subId, (int)(int8_t)entry[2]);
        if (subId == 0xFFFF) return;
        mp6_se_macro_dump_obj(grp, subId, key + (int8_t)entry[2], depth - 1);
        return;
    }
    case 0x8000: { /* LAYER */
        uint32_t num, r;
        m = msm_pool_find_node(grp, 0xC, objId);
        if (!m) { printf("[SEMACRO]   layer %#x NOT FOUND\n", (unsigned)objId); return; }
        if (!msm_group_span(grp, (uint64_t)(m - grp->blob), 12, "LAYER header")) return;
        num = be32(m + 8);
        if (num > 128) return;
        if (!msm_group_span(grp, (uint64_t)(m - grp->blob) + 12, (uint64_t)num * 12,
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

    m = msm_pool_find_node(grp, 0, objId);
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

void mp6_se_macro_dump(void)
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
            if (msm_se_group_materialize(&tmp, gi, 0, 0, "macrodump") != 0) continue;
            if (msm_find_group_data(&tmp, g_grpInfo[gi].gid, &groupDataOff) &&
                msm_group_span(&tmp, (uint64_t)tmp.projOfs + groupDataOff, 0x28, "FX GROUP_DATA")) {
                uint32_t fxTableOff = be32(tmp.blob + tmp.projOfs + groupDataOff + 0x1C);
                if (msm_group_span(&tmp, (uint64_t)tmp.projOfs + fxTableOff, 4, "FX_DATA header")) {
                    fxBase = tmp.blob + tmp.projOfs + fxTableOff;
                    fxNum = (uint16_t)be16(fxBase + 0);
                    if (msm_group_span(&tmp, (uint64_t)tmp.projOfs + fxTableOff + 4,
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
            msm_se_group_unload(&tmp);
        }
        if (!found) printf("[SEMACRO]   no fx entry found for seId=%d\n", seId);
    }
    fflush(stdout);
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

void mp6_grpswap_leaktest_maybe_start(void)
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

void mp6_se_leaktest_maybe_start(void)
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

void mp6_leaktest_maybe_start(void)
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


/* One census pass per GAME tick, on the game thread (MP6_AUDIO_TIMELINE=1).
 * This is the half of the timeline that no game-facing API call can report:
 * a voice that keeps sounding without any new msmSePlay. Snapshot under
 * g_mixerLock, print AFTER unlocking -- printf must never run inside the
 * lock the SDL audio callback needs. */
void mp6_se_timeline_census(void)
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
