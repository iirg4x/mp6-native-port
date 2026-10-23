/* MP6 native port -- savestate audio shadow for the msm engine: capture,
 * validation and re-sync of the stream channels, SFX voices and loaded
 * groups (Mp6SsAudioShadow in include/mp6_savestate.h).
 *
 * Part of the msm engine -- see msm_internal.h and msm_bridge.c. */

#include "msm_internal.h"
#include "mp6_host_section.h" /* savestate carve-out -- see msm_bridge.c */

/* =======================================================================
 * Savestate audio shadow
 * =======================================================================
 * Every msm_*.c file is carved out of the savestate, because its state is
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

    if (!msm_pdt_get_pack(chan->streamId, &pack)) return 0;
    adpcmFrames = msm_pdt_pack_frame_count(&pack);
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
    if (msm_se_group_materialize(&group, voice->groupIdx, 0, 0,
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
    if (fx == NULL || !msm_lookup_sdir(&group, fx->sampleId, &info) || info.compType != 0) goto done;
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
    msm_se_group_unload(&group);
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
            if (msm_se_group_materialize(&checkGroup, grpIdx, encoded < 0, encoded < 0,
                                     "savestate-group-preflight") != 0) {
                valid = 0;
            } else {
                msm_se_group_unload(&checkGroup);
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
            mp6_msm_stream_finish_decode(i); /* the position below may be anywhere in the track */
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
