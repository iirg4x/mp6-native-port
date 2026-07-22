/* MP6 native port -- Unlocked FPS MODEL-level interpolation (P1).
 *
 * shim/include/mp6_fi_model.h carries the full design contract. This file is
 * the mechanism: per-tick transform/camera snapshots, the idle-window
 * advance-and-re-run-Hu3DExec body, and the exact tick-N restore that keeps
 * the game desync-free (GATE A). The in-between pose runs FORWARD past the
 * newest snapshot -- see "DIRECTION OF THE IN-BETWEEN" below, the v212
 * on-device jitter fix -- and replays are suppressed entirely while a screen
 * wipe is up, see "Wipe gate".
 *
 * Lives in platform/hsf/ (like mp6_freecam.c / mp6_widescreen_extrude.c)
 * because it needs game/hu3d.h for Hu3DData[]/Hu3DCamera[]/Hu3DExec, compiled
 * with COMMON_FLAGS in BOTH build modes. Its ONE replay caller
 * (frame_interp.c, aurora-only) only ever runs windowed; in headless nothing
 * calls mp6_fi_model_replay, so the re-run path is dead code there and the
 * headless log stays byte-identical.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "game/hu3d.h"
#include "game/wipe.h" /* wipeData.mode -- the replay gate for the wipe overlay */
#include "mp6_fi_model.h"

/* SAVESTATE CARVE-OUT: the snapshot buffers below are
 * host-owned render state that belongs to the RUNNING process, not to captured
 * game state -- a restored .state must neither reinstate a pre-restore
 * snapshot nor interpolate across the state discontinuity. Must sit AFTER this
 * TU's own includes and at preprocessor TOP LEVEL, exactly like mp6_freecam.c
 * and frame_interp.c. Registered in tools/build.py HOST_STATE_SECTION_SOURCES. */
#include "mp6_host_section.h"

/* Per-pair snap gates, expressed on the model's own pos/rot fields (the
 * deleted stream path had the same intent expressed on raw matrices, but with
 * no identity to snap ON). A slot whose transform moved further than this in one tick is
 * a spawn/teleport/menu-flip discontinuity: interpolating it would smear or
 * step backward, so that slot is presented at its tick-N pose verbatim
 * (snapped, i.e. it stays at the tick rate) while smoothly-moving slots
 * interpolate. Per-identity (slot index), so the snap never touches a
 * neighbour -- the whole reason this path exists. */
#define FI_TRANS_SNAP_U   50.0f   /* |delta pos| >= this (u/tick) -> snap the slot */
#define FI_ROT_SNAP_DEG   10.0f   /* |delta rot| >= this (deg/tick, any axis) -> snap */
#define FI_CAM_SNAP_U     50.0f   /* camera pos/target cut -> snap the camera */

/* DIRECTION OF THE IN-BETWEEN (the v212 jitter fix).
 *
 * The original P1 brief said to INTERPOLATE t in [0,1] between snapshots N-1
 * and N, "accepting ~1 tick latency". That was wrong, and it is exactly what
 * made every smoothly-moving model judder on device: the game's OWN real frame
 * is untouched by this path -- Hu3DExec runs on the tick-N pose and
 * aurora_end_frame presents pose N -- so with backward interpolation the
 * PRESENTED sequence inside one tick window is
 *
 *     [real] N -> [replay] ~N-1 -> ~N-0.75 -> ... -> [real] N+1
 *
 * i.e. every tick the picture steps ~one full tick BACKWARD and then catches
 * up. That is a sawtooth at the tick rate on every moving object -- worse than
 * no interpolation at all. Latency was never actually being bought either: the
 * real frame kept showing N regardless.
 *
 * So the replays EXTRAPOLATE forward instead: t = 1 + alpha, alpha in [0,1),
 * continuing along the N-1 -> N delta past pose N. The presented sequence
 * becomes monotonic --
 *
 *     [real] N -> [replay] N+0.25 -> N+0.5 -> N+0.75 -> [real] N+1
 *
 * -- with no added latency and no backward step. Overshoot is bounded by the
 * per-slot snap gates above (a slot that moved >50u or >10deg in one tick is
 * presented at pose N verbatim), which is precisely what the now-deleted stream
 * path lacked: it extrapolated the same way but had no object identity to snap
 * ON, so a flipping menu portrait smeared. Here a fast/non-linear mover snaps
 * and only near-linear movers extrapolate, over a single 16ms tick.
 *
 * MP6_FI_MODEL_BACKLERP=1 restores the old backward t in [0,1] behavior. It
 * exists only so the sawtooth can be re-demonstrated from the SAME binary
 * (see the MP6_FI_POSELOG instrument at the bottom of this file); nothing
 * ships with it set. */

int mp6_fi_replay_pass = 0;

/* Hu3DExec globals that a re-run writes but that belong to tick N (game logic
 * outside Hu3DExec can read them -- e.g. Hu3DCameraMtx for GXProject-style
 * position read-back). A re-run leaves them at the LAST replay's interp value
 * (alpha/timing-dependent), so they must be saved before and restored after. */
extern Mtx Hu3DCameraMtx;
extern Mtx Hu3DCameraMtxXPose;
extern u32 totalPolyCnt, totalMatCnt, totalTexCnt, totalTexCacheCnt;
extern u32 Hu3DMallocNo;

typedef struct {
    int  live;          /* model->hsf != NULL at snapshot time */
    Vec  pos, rot, scale;
    Mtx  mtx;           /* snapshot only -- not interpolated in P1; used to
                         * assert Hu3DExec left it untouched on a re-run */
    u32  motAttr;       /* the one non-transform field a MOT_SLOW re-run can
                         * touch; restored so the tick-N assert is exact */
    u8   tick;          /* diagnostic: re-run must not advance the per-model tick */
    float motTime, motOvlTime, motShiftTime, motShapeTime; /* anim clocks */
} FiModelSnap;

typedef struct {
    int  valid;         /* Hu3DCamera[i].fov != -1 at snapshot time */
    Vec  pos, up, target;
} FiCamSnap;

static FiModelSnap s_prev[HU3D_MODEL_MAX];
static FiModelSnap s_cur[HU3D_MODEL_MAX];
static FiCamSnap   s_camPrev[HU3D_CAM_MAX];
static FiCamSnap   s_camCur[HU3D_CAM_MAX];
static int         s_haveCur;
static int         s_havePrev;

/* ------------------------------------------------------------------ */

static void fnv_f32(unsigned int *h, float f); /* fwd */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Scale gets the same t as pos/rot, but extrapolation past t=1 can drive a
 * shrinking axis through zero and out the other side (an inside-out model for
 * one frame). Never let the extrapolated axis cross zero relative to its own
 * tick-N value: on a crossing, present the tick-N scale for that axis. */
static float lerp_scale(float a, float b, float t)
{
    float v = a + (b - a) * t;
    if (v * b <= 0.0f) return b;
    return v;
}

/* Shortest-arc Euler interpolation (mtxRot takes degrees). Wraps the per-axis
 * delta into [-180,180] so a 359deg -> 1deg step lerps the 2deg short way, not
 * the 358deg long way. */
static float wrap180(float d)
{
    d = fmodf(d, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

static float lerp_angle(float a, float b, float t) { return a + wrap180(b - a) * t; }

static float ang_delta(float a, float b)
{
    float d = wrap180(b - a);
    return d < 0.0f ? -d : d;
}

/* ------------------------------------------------------------------
 * MP6_FI_MODEL_BACKLERP -- diagnostic A/B lever (see the FI_TRANS_SNAP_U
 * block): 1 restores the pre-fix backward interpolation so the sawtooth can
 * be reproduced from the shipped binary. Unset = the shipped forward path.
 * ------------------------------------------------------------------ */
static int fi_backlerp_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MP6_FI_MODEL_BACKLERP");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s;
}

/* ------------------------------------------------------------------
 * MP6_FI_POSELOG -- per-PRESENTED-FRAME pose instrument (the BUG-1 proof).
 *
 * One line per frame that actually reaches the screen, in presentation order:
 * the REAL frame (logged from mp6_fi_model_snapshot, which runs immediately
 * after aurora_end_frame presented pose N) and every replay frame after it.
 * `u` is measured from the APPLIED Vec, not from t: it is the position's own
 * parameter along the N-1 -> N segment, so u=1 is exactly pose N. `abs` is
 * u expressed on a global tick axis (tick + u - 1) -- it MUST be monotonically
 * non-decreasing across the whole run. Backward interpolation makes it saw.
 *
 * The tracked slot is re-chosen every tick (the live-in-both slot with the
 * largest sub-snap-threshold motion) and printed, so a post-pass can group by
 * it. Silent (one cached getenv) when unset.
 * ------------------------------------------------------------------ */
static int s_poseSlot = -1;
static long s_poseTick;

static int fi_poselog_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MP6_FI_POSELOG");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s;
}

static void fi_poselog_frame(int isReal, const Vec *applied)
{
    static long s_seq;
    float ax, ay, az, bx, by, bz, den, u;
    int i = s_poseSlot;

    if (!fi_poselog_enabled() || i < 0) return;
    ax = s_cur[i].pos.x - s_prev[i].pos.x;
    ay = s_cur[i].pos.y - s_prev[i].pos.y;
    az = s_cur[i].pos.z - s_prev[i].pos.z;
    den = ax * ax + ay * ay + az * az;
    if (den <= 0.0f) return;
    bx = applied->x - s_prev[i].pos.x;
    by = applied->y - s_prev[i].pos.y;
    bz = applied->z - s_prev[i].pos.z;
    u = (ax * bx + ay * by + az * bz) / den;
    fprintf(stderr, "[FI-POSE] seq=%ld kind=%s tick=%ld slot=%d u=%.4f abs=%.4f pos=%.4f,%.4f,%.4f\n",
            ++s_seq, isReal ? "real" : "replay", s_poseTick, i, (double)u,
            (double)s_poseTick + (double)u - 1.0,
            (double)applied->x, (double)applied->y, (double)applied->z);
    fflush(stderr);
}

/* Pick the tick's tracked slot: the live-in-both, non-snapping slot that moved
 * the most. Called from the snapshot, after s_prev/s_cur have rotated. */
static void fi_poselog_pick(void)
{
    float best = 0.0f;
    s16 i;

    s_poseSlot = -1;
    if (!fi_poselog_enabled() || !s_havePrev || !s_haveCur) return;
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        float dx, dy, dz, d2;
        if (!s_cur[i].live || !s_prev[i].live) continue;
        dx = s_cur[i].pos.x - s_prev[i].pos.x;
        dy = s_cur[i].pos.y - s_prev[i].pos.y;
        dz = s_cur[i].pos.z - s_prev[i].pos.z;
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= 1e-6f || d2 >= FI_TRANS_SNAP_U * FI_TRANS_SNAP_U) continue;
        if (ang_delta(s_prev[i].rot.x, s_cur[i].rot.x) >= FI_ROT_SNAP_DEG ||
            ang_delta(s_prev[i].rot.y, s_cur[i].rot.y) >= FI_ROT_SNAP_DEG ||
            ang_delta(s_prev[i].rot.z, s_cur[i].rot.z) >= FI_ROT_SNAP_DEG) continue;
        if (d2 > best) { best = d2; s_poseSlot = i; }
    }
}

/* =======================================================================
 * Wipe gate (BUG 2).
 *
 * The screen wipe/transition is drawn by WipeExecAlways() in the main loop
 * (src/game/main.c), AFTER Hu3DExec() and before the present -- it is NOT part
 * of the command stream a replay re-runs. So on every replay frame the wipe's
 * full-screen quad (and, in WIPE_MODE_END, the load animation) is simply
 * ABSENT: the real frame shows the faded/covered screen, the replays after it
 * show the raw uncovered scene, and the transition strobes at the
 * refresh-minus-tick beat -- the same class of bug as the Android touch
 * overlay in a6ed8dc, but on a full-screen element.
 *
 * Re-emitting it on replay frames is not viable cheaply: the fade functions
 * are not draw-only. They advance wipeData.time, malloc/free the capture
 * buffer, and several types (CROSS / DISSOLVE / VIEW_SHIFT / WAVE) run
 * Hu3DFbCopyExec -- an EFB copy, exactly the class of side effect every other
 * replay guard exists to suppress. Calling them under a replay would need a
 * new decomp patch threading mp6_fi_replay_pass through ~10 sites in wipe.c,
 * each of which could corrupt the transition.
 *
 * So: SUPPRESS replays while the wipe is drawing anything. WIPE_MODE_DUMMY is
 * the steady state in which WipeExecAlways draws nothing at all (its switch
 * has no DUMMY case), i.e. all of normal gameplay; IN/OUT/END all draw. Those
 * ticks fall back to the plain tick-rate present, which is what the build did
 * before this feature existed -- a wipe is a sub-second transition where extra
 * smoothness is least noticeable, and skipping cannot corrupt anything.
 * ======================================================================= */
int mp6_fi_model_wipe_active(void)
{
    return wipeData.mode != WIPE_MODE_DUMMY;
}

/* =======================================================================
 * Snapshot (called at note_frame_end each real tick, model mode + enabled).
 * ======================================================================= */
void mp6_fi_model_snapshot(void)
{
    s16 i;
    HU3D_MODEL *m;

    if (!Hu3DData) {
        return; /* pre-Hu3DInit: nothing to snapshot */
    }

    /* Rotate N -> N-1, then capture the live tick-N state as N. memcpy of the
     * whole array (fixed size) is cheaper than a live-only walk and keeps the
     * dead slots' `live=0` from the previous capture out of the way. */
    memcpy(s_prev, s_cur, sizeof(s_prev));
    memcpy(s_camPrev, s_camCur, sizeof(s_camPrev));
    s_havePrev = s_haveCur;

    for (m = &Hu3DData[0], i = 0; i < HU3D_MODEL_MAX; i++, m++) {
        if (m->hsf) {
            s_cur[i].live = 1;
            s_cur[i].pos = m->pos;
            s_cur[i].rot = m->rot;
            s_cur[i].scale = m->scale;
            memcpy(s_cur[i].mtx, m->mtx, sizeof(Mtx));
            s_cur[i].motAttr = m->motAttr;
            s_cur[i].tick = m->tick;
            s_cur[i].motTime = m->motWork.time;
            s_cur[i].motOvlTime = m->motOvlWork.time;
            s_cur[i].motShiftTime = m->motShiftWork.time;
            s_cur[i].motShapeTime = m->motShapeWork.time;
        } else {
            s_cur[i].live = 0;
        }
    }
    for (i = 0; i < HU3D_CAM_MAX; i++) {
        if (Hu3DCamera[i].fov != -1.0f) {
            s_camCur[i].valid = 1;
            s_camCur[i].pos = Hu3DCamera[i].pos;
            s_camCur[i].up = Hu3DCamera[i].up;
            s_camCur[i].target = Hu3DCamera[i].target;
        } else {
            s_camCur[i].valid = 0;
        }
    }
    s_haveCur = 1;

    /* MP6_FI_POSELOG: this runs immediately after aurora_end_frame() presented
     * the REAL tick-N frame, so pose N is exactly what the screen just showed.
     * Log it FIRST in the window, then the replays that follow. */
    s_poseTick++;
    fi_poselog_pick();
    if (s_poseSlot >= 0) {
        fi_poselog_frame(1, &s_cur[s_poseSlot].pos);
    }
}

int mp6_fi_model_ready(void)
{
    return s_haveCur && s_havePrev;
}

void mp6_fi_model_reset(void)
{
    s_haveCur = 0;
    s_havePrev = 0;
    s_poseSlot = -1;
    /* The arrays keep their bytes; havePrev/haveCur gate all reads, and the
     * next snapshot overwrites cur before it is ever paired. */
}

/* =======================================================================
 * Replay: interpolate -> re-run Hu3DExec (guarded) -> restore tick-N exactly.
 * ======================================================================= */

/* Debug instrument (env MP6_FI_MODEL_ASSERT=1): after the guarded re-run and
 * BEFORE the restore, verify Hu3DExec left every live slot's mtx -- and the
 * pos/rot/scale of slots we did NOT overwrite -- exactly at tick N. A nonzero
 * count means the re-run mutated a transform we do not restore, i.e. a latent
 * desync source. Prints a throttled line; never changes behavior. */
static int fi_assert_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MP6_FI_MODEL_ASSERT");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s;
}

/* Hash every live model's HSF object mesh.curr transforms -- the per-object
 * pose the re-run must leave frozen at tick N. */
static unsigned int fi_mesh_hash(void)
{
    unsigned int h = 2166136261u;
    s16 i, j;
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        HU3D_MODEL *m = &Hu3DData[i];
        if (!s_cur[i].live || !m->hsf) continue;
        for (j = 0; j < m->hsf->objectNum; j++) {
            HSF_TRANSFORM *t = &m->hsf->object[j].mesh.curr;
            fnv_f32(&h, t->pos.x); fnv_f32(&h, t->pos.y); fnv_f32(&h, t->pos.z);
            fnv_f32(&h, t->rot.x); fnv_f32(&h, t->rot.y); fnv_f32(&h, t->rot.z);
            fnv_f32(&h, t->scale.x); fnv_f32(&h, t->scale.y); fnv_f32(&h, t->scale.z);
        }
    }
    return h;
}

void mp6_fi_model_replay(double alpha)
{
    s16 i;
    HU3D_MODEL *m;
    float a = (float)alpha;
    float t;
    HU3D_CAMERA camSave[HU3D_CAM_MAX];

    if (!Hu3DData || !s_haveCur || !s_havePrev) {
        return;
    }
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    /* FORWARD extrapolation past pose N (see the FI_TRANS_SNAP_U block above):
     * t = 1 + alpha, so the presented pose only ever advances. alpha is the
     * fraction of the tick period elapsed since pose N was presented, so
     * pose(t) is literally "where this slot is, alpha of a tick after N" --
     * and the next REAL frame lands on N+1, continuing the same line. */
    t = 1.0f + a;
    if (fi_backlerp_enabled()) t = a; /* diagnostic A/B: the old backward lerp */

    /* Save the WHOLE camera array (every field, every slot) before we touch
     * anything: a camera-driven model's re-run re-runs Hu3DCameraMotionExec ->
     * SetObjCameraMotion, which derives camera pos/target/fov/roll from the
     * (now interpolated) model pos/scale and can even make a dead camera live.
     * Restoring only pos/up/target would leave those other fields corrupted for
     * tick N+1's game logic to read -> desync. A full-array save/restore is
     * exact regardless of which fields the re-run wrote. */
    memcpy(camSave, Hu3DCamera, sizeof(camSave));

    /* --- 1. write pose(N-1 -> N, t) into every smoothly-moving live slot.
     * Spawn (live in N only), teleport (|dpos| >= FI_TRANS_SNAP_U) and fast
     * flip (|drot| >= FI_ROT_SNAP_DEG on any axis) are left at their tick-N
     * value verbatim -- per slot, so a flipping portrait snaps while a settling
     * one advances. mtx is NOT interpolated in P1 (rigid pos/rot/scale
     * only). */
    for (m = &Hu3DData[0], i = 0; i < HU3D_MODEL_MAX; i++, m++) {
        float dx, dy, dz;
        if (!m->hsf || !s_cur[i].live || !s_prev[i].live) {
            continue; /* not live / spawn -> verbatim tick-N */
        }
        dx = s_cur[i].pos.x - s_prev[i].pos.x;
        dy = s_cur[i].pos.y - s_prev[i].pos.y;
        dz = s_cur[i].pos.z - s_prev[i].pos.z;
        if (dx * dx + dy * dy + dz * dz >= FI_TRANS_SNAP_U * FI_TRANS_SNAP_U) {
            continue; /* teleport -> snap */
        }
        if (ang_delta(s_prev[i].rot.x, s_cur[i].rot.x) >= FI_ROT_SNAP_DEG ||
            ang_delta(s_prev[i].rot.y, s_cur[i].rot.y) >= FI_ROT_SNAP_DEG ||
            ang_delta(s_prev[i].rot.z, s_cur[i].rot.z) >= FI_ROT_SNAP_DEG) {
            continue; /* fast flip/spin -> snap */
        }
        m->pos.x = lerpf(s_prev[i].pos.x, s_cur[i].pos.x, t);
        m->pos.y = lerpf(s_prev[i].pos.y, s_cur[i].pos.y, t);
        m->pos.z = lerpf(s_prev[i].pos.z, s_cur[i].pos.z, t);
        m->rot.x = lerp_angle(s_prev[i].rot.x, s_cur[i].rot.x, t);
        m->rot.y = lerp_angle(s_prev[i].rot.y, s_cur[i].rot.y, t);
        m->rot.z = lerp_angle(s_prev[i].rot.z, s_cur[i].rot.z, t);
        m->scale.x = lerp_scale(s_prev[i].scale.x, s_cur[i].scale.x, t);
        m->scale.y = lerp_scale(s_prev[i].scale.y, s_cur[i].scale.y, t);
        m->scale.z = lerp_scale(s_prev[i].scale.z, s_cur[i].scale.z, t);
        if (i == s_poseSlot) fi_poselog_frame(0, &m->pos); /* MP6_FI_POSELOG */
    }

    /* --- cameras: same rule, injected exactly like freecam (direct field
     * write into Hu3DCamera[], consumed by Hu3DCameraSet at draw time). A
     * camera driven by Hu3DCameraMotionExec (HU3D_ATTR_CAMERA models) is
     * re-derived at tick-N time inside the re-run and thus effectively snapped
     * -- fine for P1 (menus set their camera directly). */
    for (i = 0; i < HU3D_CAM_MAX; i++) {
        float dx, dy, dz;
        if (Hu3DCamera[i].fov == -1.0f || !s_camCur[i].valid || !s_camPrev[i].valid) {
            continue;
        }
        dx = s_camCur[i].pos.x - s_camPrev[i].pos.x;
        dy = s_camCur[i].pos.y - s_camPrev[i].pos.y;
        dz = s_camCur[i].pos.z - s_camPrev[i].pos.z;
        if (dx * dx + dy * dy + dz * dz >= FI_CAM_SNAP_U * FI_CAM_SNAP_U) {
            continue; /* camera cut -> snap */
        }
        /* Same forward t as the models: a camera that lagged one tick behind
         * the objects it frames would shear the whole scene every window. */
        Hu3DCamera[i].pos.x = lerpf(s_camPrev[i].pos.x, s_camCur[i].pos.x, t);
        Hu3DCamera[i].pos.y = lerpf(s_camPrev[i].pos.y, s_camCur[i].pos.y, t);
        Hu3DCamera[i].pos.z = lerpf(s_camPrev[i].pos.z, s_camCur[i].pos.z, t);
        Hu3DCamera[i].up.x = lerpf(s_camPrev[i].up.x, s_camCur[i].up.x, t);
        Hu3DCamera[i].up.y = lerpf(s_camPrev[i].up.y, s_camCur[i].up.y, t);
        Hu3DCamera[i].up.z = lerpf(s_camPrev[i].up.z, s_camCur[i].up.z, t);
        Hu3DCamera[i].target.x = lerpf(s_camPrev[i].target.x, s_camCur[i].target.x, t);
        Hu3DCamera[i].target.y = lerpf(s_camPrev[i].target.y, s_camCur[i].target.y, t);
        Hu3DCamera[i].target.z = lerpf(s_camPrev[i].target.z, s_camCur[i].target.z, t);
    }

    /* --- 2. re-run render with every game-state advance guarded off. The
     * hsfman.c patch reads mp6_fi_replay_pass to skip tick++, Hu3DMotionNext,
     * HuSprFinish, Hu3DAnimExec, and the shadow/reflect EFB brackets; the
     * vertex-deform block self-skips (its MOT_EXEC gate is still set from the
     * real run, cleared only by Hu3DPreProc, which does not run here). */
    {
      Mtx camMtxSave, camMtxXPoseSave;
      u32 polySave = totalPolyCnt, matSave = totalMatCnt, texSave = totalTexCnt,
          texCacheSave = totalTexCacheCnt, mallocSave = Hu3DMallocNo;
      memcpy(camMtxSave, Hu3DCameraMtx, sizeof(Mtx));
      memcpy(camMtxXPoseSave, Hu3DCameraMtxXPose, sizeof(Mtx));

    { static unsigned int s_meshBefore; if (fi_assert_enabled()) s_meshBefore = fi_mesh_hash();
      mp6_fi_replay_pass = 1;
      Hu3DExec();
      mp6_fi_replay_pass = 0;
      if (fi_assert_enabled()) { static long s_mc, s_mv; s_mc++; if (fi_mesh_hash() != s_meshBefore) s_mv++;
          if ((s_mc % 4096) == 0) { fprintf(stderr, "[FI-MESH] mesh.curr mutated by re-run: %ld / %ld\n", s_mv, s_mc); fflush(stderr); } }
    }

    /* --- 3. GATE-A integrity check (MP6_FI_MODEL_ASSERT=1, before restore).
     * This in-process, deterministic check is THE reliable GATE-A proof: it
     * verifies every game-state field the re-run does NOT overwrite (tick, the
     * four anim clocks, mtx, motAttr -- and mesh.curr in the block above) is
     * byte-identical to tick N, i.e. the guarded re-run advanced nothing.
     * (A windowed ON-vs-OFF anim-digest diff is NOT reliable here: windowed
     * game state has an intermittent boot/present-timing race -- OFF itself is
     * not always reproducible across processes, and the since-deleted stream
     * path showed the same variation -- so this assert, not a cross-process
     * digest, is what proves no desync.) pos/rot/scale are the interp targets and are
     * restored in step 4. Per-field counters localize any leak. */
    if (fi_assert_enabled()) {
        static long s_checks, s_vMtx, s_vTick, s_vMot, s_vAttr;
        for (i = 0; i < HU3D_MODEL_MAX; i++) {
            HU3D_MODEL *cm = &Hu3DData[i];
            if (!s_cur[i].live || !cm->hsf) continue;
            s_checks++;
            if (memcmp(cm->mtx, s_cur[i].mtx, sizeof(Mtx)) != 0) s_vMtx++;
            if (cm->tick != s_cur[i].tick) s_vTick++;
            if (cm->motAttr != s_cur[i].motAttr) s_vAttr++;
            if (cm->motWork.time != s_cur[i].motTime ||
                cm->motOvlWork.time != s_cur[i].motOvlTime ||
                cm->motShiftWork.time != s_cur[i].motShiftTime ||
                cm->motShapeWork.time != s_cur[i].motShapeTime) s_vMot++;
        }
        if ((s_checks % 4096) == 0) {
            fprintf(stderr, "[FI-MODEL-ASSERT] checks=%ld leaks: mtx=%ld tick=%ld motTime=%ld motAttr=%ld\n",
                    s_checks, s_vMtx, s_vTick, s_vMot, s_vAttr);
            fflush(stderr);
        }
    }

    /* --- 4. restore tick-N exactly. Hu3DExec never writes pos/rot/scale/mtx
     * (it reads them to build the draw matrix); it can touch motAttr only on
     * the MOT_SLOW path, idempotently. Restoring every live-cur slot's
     * pos/rot/scale + motAttr (superset of what we overwrote) guarantees
     * Hu3DData is byte-identical to tick N before N+1's game logic runs. */
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        if (!s_cur[i].live) continue;
        m = &Hu3DData[i];
        m->pos = s_cur[i].pos;
        m->rot = s_cur[i].rot;
        m->scale = s_cur[i].scale;
        m->motAttr = s_cur[i].motAttr;
    }
    memcpy(Hu3DCamera, camSave, sizeof(camSave)); /* full tick-N camera restore */

      memcpy(Hu3DCameraMtx, camMtxSave, sizeof(Mtx));
      memcpy(Hu3DCameraMtxXPose, camMtxXPoseSave, sizeof(Mtx));
      totalPolyCnt = polySave; totalMatCnt = matSave; totalTexCnt = texSave;
      totalTexCacheCnt = texCacheSave; Hu3DMallocNo = mallocSave;
    } /* close the Hu3DExec-globals save/restore scope */
}

/* =======================================================================
 * GATE-A anim-drift instrument (env MP6_FI_ANIMLOG=1).
 * ======================================================================= */
static int fi_animlog_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("MP6_FI_ANIMLOG");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s;
}

/* FNV-1a over the raw bytes of a float (deterministic; equal state -> equal
 * digest with the feature on vs off iff the re-runs advanced nothing). */
static void fnv_f32(unsigned int *h, float f)
{
    unsigned char *p = (unsigned char *)&f;
    int k;
    for (k = 0; k < 4; k++) {
        *h ^= p[k];
        *h *= 16777619u;
    }
}

void mp6_fi_model_animlog(long tick)
{
    unsigned int h = 2166136261u;
    int live = 0;
    s16 i;
    HU3D_MODEL *m;

    if (!fi_animlog_enabled() || !Hu3DData) {
        return;
    }
    for (m = &Hu3DData[0], i = 0; i < HU3D_MODEL_MAX; i++, m++) {
        if (!m->hsf) continue;
        live++;
        h ^= (unsigned int)m->tick; h *= 16777619u;
        fnv_f32(&h, m->motWork.time);
        fnv_f32(&h, m->motOvlWork.time);
        fnv_f32(&h, m->motShiftWork.time);
        fnv_f32(&h, m->motShapeWork.time);
        fnv_f32(&h, m->pos.x); fnv_f32(&h, m->pos.y); fnv_f32(&h, m->pos.z);
        fnv_f32(&h, m->rot.x); fnv_f32(&h, m->rot.y); fnv_f32(&h, m->rot.z);
        fnv_f32(&h, m->scale.x); fnv_f32(&h, m->scale.y); fnv_f32(&h, m->scale.z);
    }
    fprintf(stderr, "[FI-ANIMLOG] tick=%ld live=%d digest=%08x\n", tick, live, h);
    fflush(stderr);
}
