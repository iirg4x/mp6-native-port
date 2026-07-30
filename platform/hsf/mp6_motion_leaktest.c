/* mp6_motion_leaktest.c -- standalone motion-bank ownership stress probe.
 *
 * WHY THIS EXISTS. docs/research/hsf_stub_audit.md D4/D5 found two
 * ownership holes on the standalone-motion path: Hu3DMotionKill freed only
 * the outer HSF_DATA and leaked the native graph's tagged sub-allocations
 * (fixed in patches/decomp/src/game/hsfmotion.c.patch), and
 * ResolveTrackCurve's three curve arrays were allocated UNTAGGED, so no
 * bulk free could ever reclaim them (fixed in hsf_load_native.c). Both
 * leaks are structurally invisible to tools/leakgate.py: they churn inside
 * the pre-reserved game arena, which never moves process RSS. The honest
 * observable is the game's own heap accounting -- the exact numbers the
 * MP6_ALLOC_CENSUS machinery (platform/os/malloc_direct.c) prints -- so
 * this probe drives real create/kill cycles and prints those numbers
 * around each one.
 *
 * WHAT IT DOES. Armed by MP6_MOTION_LEAKTEST="dataNumHex[,startTick
 * [,cycles]]" (a complete no-op otherwise, preserving the ua1 log-diff
 * contract byte-for-byte). From startTick on, one cycle per tick, on the
 * game thread (called from mp6_tick_advance(), the same per-tick choke
 * point the alloc census and RSS watchdog ride):
 *
 *     read the motion bank  (HuDataSelHeapReadNum, own probe tag)
 *     Hu3DMotionCreate      (real LoadHSF -> native graph)
 *     Hu3DMotionKill        (the code under test)
 *     free the file buffer  (HuMemDirectFreeNum by the probe tag)
 *
 * and prints HEAP_MODEL used-bytes/active-blocks before and after. Cycle 1
 * additionally pays one-time costs (directory read + read-status slot), so
 * the verdict line reports the STEADY-STATE net across cycles 2..N --
 * exactly 0 bytes / 0 blocks iff every allocation of a full
 * create/kill cycle has an owner. tools/motion_census_gate.py asserts
 * that. Default bank: 0x990012 = DATANUM(DATA_mdsel, 0x12), a real
 * motion-only joint-motion file (mode-select character motion; the class
 * the disc carries 5,561 of).
 *
 * The probe deliberately allocates NOTHING itself on the game heaps and
 * frees by its own tag every cycle, so its measurements are of the engine
 * path alone. Same test-only precedent as msm_bridge.c's
 * MP6_AUDIO_LEAKTEST_* stress hooks, but tick-scheduled on the game thread
 * (Hu3D APIs are game-thread state machines, not thread-safe like the
 * audio entry points those hooks call). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/hu3d.h"    /* Hu3DMotionCreate/Hu3DMotionKill */
#include "game/data.h"    /* HuDataSelHeapReadNum/HuDataReadChk/HuDataDirClose */
#include "game/memory.h"  /* HuMemDirectFreeNum, HuMemUsedMalloc*Get, HuMemHeapPtrGet */

#include "mp6_boot.h"     /* mp6_tick_count + this TU's own prototype */

/* No game code tags with this value (game tags are small constants like
 * HU_MEMNUM_OVL or model-pointer tags, which are heap addresses). */
#define MP6_MOTION_LEAKTEST_TAG 0x4D4C544Bu /* 'MLTK' */

static int  g_state = -1;        /* -1 env unparsed, 0 disabled, 1 armed, 2 finished */
static int  g_dataNum = 0x990012;
static long g_startTick = 600;
static int  g_cycles = 32;
static int  g_cyclesDone;
static int  g_dirWasOpen;        /* leave the dir cached iff the game had it open */
static s32  g_steadyUsedBase;    /* HEAP_MODEL used bytes after cycle 1 */
static s32  g_steadyBlkBase;     /* HEAP_MODEL active blocks after cycle 1 */

static void mp6_motion_leaktest_parse_env(void)
{
    const char *e = getenv("MP6_MOTION_LEAKTEST");
    char *end = NULL;
    long v;

    g_state = 0;
    if (e == NULL || e[0] == '\0') {
        return;
    }
    v = strtol(e, &end, 16);
    if (end == e || v <= 0 || v > 0x7FFFFFFFL) {
        fprintf(stderr, "[MOTION-LEAKTEST] invalid MP6_MOTION_LEAKTEST value "
                "\"%s\" (want dataNumHex[,startTick[,cycles]]) -- disabled\n", e);
        fflush(stderr);
        return;
    }
    g_dataNum = (int)v;
    if (*end == ',') {
        v = strtol(end + 1, &end, 10);
        if (v > 0) g_startTick = v;
    }
    if (*end == ',') {
        v = strtol(end + 1, &end, 10);
        if (v > 0 && v <= 100000) g_cycles = (int)v;
    }
    g_state = 1;
    printf("[MOTION-LEAKTEST] armed: dataNum=0x%06x startTick=%ld cycles=%d "
           "(one real Hu3DMotionCreate/Hu3DMotionKill cycle per tick)\n",
           (unsigned)g_dataNum, g_startTick, g_cycles);
    fflush(stdout);
}

void mp6_motion_leaktest_tick(void)
{
    s32 usedBefore, blkBefore, usedAfter, blkAfter;
    void *buf;
    HU3D_MOTIONID motId;

    if (g_state == 0 || g_state == 2) return;
    if (g_state == -1) {
        mp6_motion_leaktest_parse_env();
        if (g_state != 1) return;
    }
    if (mp6_tick_count < g_startTick) return;
    if (HuMemHeapPtrGet(HEAP_MODEL) == NULL) return; /* heaps not up yet */

    if (g_cyclesDone == 0) {
        g_dirWasOpen = HuDataReadChk(g_dataNum) >= 0;
    }

    usedBefore = HuMemUsedMallocSizeGet(HEAP_MODEL);
    blkBefore = HuMemUsedMallocBlockGet(HEAP_MODEL);

    buf = HuDataSelHeapReadNum(g_dataNum, MP6_MOTION_LEAKTEST_TAG, HEAP_MODEL);
    if (buf == NULL) {
        fprintf(stderr, "[MOTION-LEAKTEST] HuDataSelHeapReadNum(0x%06x) returned NULL "
                "-- disabled (no verdict)\n", (unsigned)g_dataNum);
        fflush(stderr);
        g_state = 0;
        return;
    }
    motId = Hu3DMotionCreate(buf);
    if (motId == HU3D_MOTIONID_NONE) {
        fprintf(stderr, "[MOTION-LEAKTEST] Hu3DMotionCreate failed (motion slots "
                "exhausted?) -- disabled (no verdict)\n");
        fflush(stderr);
        HuMemDirectFreeNum(HEAP_MODEL, MP6_MOTION_LEAKTEST_TAG);
        g_state = 0;
        return;
    }
    if (!Hu3DMotionKill(motId)) {
        fprintf(stderr, "[MOTION-LEAKTEST] Hu3DMotionKill(%d) refused -- disabled "
                "(no verdict)\n", (int)motId);
        fflush(stderr);
        g_state = 0;
        return;
    }
    /* The file buffer is probe-owned (the graph's strings point into it, so
     * it must outlive the kill; on retail the buffer WAS the model and the
     * kill itself freed it). */
    HuMemDirectFreeNum(HEAP_MODEL, MP6_MOTION_LEAKTEST_TAG);

    usedAfter = HuMemUsedMallocSizeGet(HEAP_MODEL);
    blkAfter = HuMemUsedMallocBlockGet(HEAP_MODEL);
    g_cyclesDone++;
    printf("[MOTION-LEAKTEST] cycle=%d/%d tick=%ld HEAP_MODEL used=%d->%d (%+d B) "
           "blocks=%d->%d (%+d)\n",
           g_cyclesDone, g_cycles, mp6_tick_count,
           (int)usedBefore, (int)usedAfter, (int)(usedAfter - usedBefore),
           (int)blkBefore, (int)blkAfter, (int)(blkAfter - blkBefore));
    fflush(stdout);

    if (g_cyclesDone == 1) {
        g_steadyUsedBase = usedAfter;
        g_steadyBlkBase = blkAfter;
    }
    if (g_cyclesDone >= g_cycles) {
        s32 netUsed = usedAfter - g_steadyUsedBase;
        s32 netBlk = blkAfter - g_steadyBlkBase;
        printf("[MOTION-LEAKTEST] verdict: steady-state net over cycles 2..%d: "
               "%+d B, %+d block(s) -- %s\n",
               g_cycles, (int)netUsed, (int)netBlk,
               (netUsed == 0 && netBlk == 0) ? "ZERO-GROWTH" : "LEAK");
        fflush(stdout);
        if (!g_dirWasOpen) {
            /* Retire the probe's own one-time directory read; harmless if
             * already closed. Skipped when the game itself had the dir
             * open (never yank a live directory out from under it). */
            HuDataDirClose(g_dataNum);
        }
        g_state = 2;
    }
}
