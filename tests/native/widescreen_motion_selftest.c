#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/hu3d.h"

/* The production helper is intentionally a small, linkable C seam.  Keep the
 * fixture on the real HSF structs so changes to the native ABI cannot hide
 * behind a look-alike test model. */
HU3D_MODEL *Hu3DData;

static float g_scale = 1.0f;

float mp6_widescreen_scale_factor(void)
{
    return g_scale;
}

void mp6_widescreen_motion_x(int16_t modelId, const char *objName,
                             float pivotX);
void mp6_widescreen_motion_reapply(void);
void mp6_widescreen_motion_forget(int16_t modelId);

static HU3D_MODEL g_models[HU3D_MODEL_MAX];
static HSF_DATA g_hsf;
static HSF_MOTION g_motion;
static HSF_OBJECT g_objects[2];
static HSF_TRACK g_tracks[6];

static HSF_LINEAR_KEY g_stepKeys[2] = {{0.0f, 10.0f}, {10.0f, 20.0f}};
static HSF_LINEAR_KEY g_linearKeys[2] = {{0.0f, 5.0f}, {10.0f, 15.0f}};
static HSF_BEZIER_KEY g_bezierKeys[2] = {
    {0.0f, 8.0f, 1.25f, -2.0f},
    {10.0f, 18.0f, 3.0f, 4.5f},
};
static HSF_LINEAR_KEY g_yKeys[2] = {{0.0f, 33.0f}, {10.0f, 44.0f}};
static HSF_LINEAR_KEY g_otherKeys[2] = {{0.0f, -3.0f}, {10.0f, 6.0f}};
static float g_constValue = 12.0f;

static HSF_DATA g_reusedHsf;
static HSF_MOTION g_reusedMotion;
static HSF_OBJECT g_reusedObject;
static HSF_TRACK g_reusedTrack;
static HSF_LINEAR_KEY g_reusedKeys[2] = {{0.0f, 30.0f}, {10.0f, 40.0f}};
static HSF_DATA g_otherHsf;

static void set_transform_track(HSF_TRACK *track, u16 target, u16 channel,
                                u16 curveType, u16 keyCount, void *data)
{
    memset(track, 0, sizeof(*track));
    track->type = HSF_TRACK_TRANSFORM;
    track->target = target;
    track->channel = channel;
    track->curveType = curveType;
    track->numKeyframes = keyCount;
    track->data = data;
}

static void setup_scene(void)
{
    int modelNo;

    memset(g_models, 0, sizeof(g_models));
    memset(&g_hsf, 0, sizeof(g_hsf));
    memset(&g_motion, 0, sizeof(g_motion));
    memset(g_objects, 0, sizeof(g_objects));
    memset(g_tracks, 0, sizeof(g_tracks));

    Hu3DData = g_models;
    for (modelNo = 0; modelNo < HU3D_MODEL_MAX; ++modelNo)
        g_models[modelNo].linkMdlId = HU3D_MODELID_NONE;
    g_models[0].hsf = &g_hsf;
    g_models[0].hsfLink = NULL;
    g_models[0].linkMdlId = HU3D_MODELID_NONE;

    g_hsf.object = g_objects;
    g_hsf.objectNum = 2;
    g_hsf.motion = &g_motion;
    /* The native loader stores the embedded motion's track count here, and
     * the embedded HSF_MOTION carries the same validated track count. */
    g_hsf.motionNum = 6;
    g_motion.name = (char *)"embedded";
    g_motion.numTracks = 6;
    g_motion.track = g_tracks;

    g_objects[0].name = (char *)"scenery";
    g_objects[0].type = HSF_OBJ_NULL1;
    g_objects[0].mesh.base.pos.x = 10.0f;
    g_objects[0].mesh.base.pos.y = 20.0f;
    g_objects[0].mesh.base.pos.z = 30.0f;
    g_objects[0].mesh.base.rot.x = 1.0f;
    g_objects[0].mesh.base.rot.y = 2.0f;
    g_objects[0].mesh.base.rot.z = 3.0f;
    g_objects[0].mesh.base.scale.x = 4.0f;
    g_objects[0].mesh.base.scale.y = 5.0f;
    g_objects[0].mesh.base.scale.z = 6.0f;

    g_objects[1].name = (char *)"other";
    g_objects[1].type = HSF_OBJ_MESH;

    set_transform_track(&g_tracks[0], 0, HSF_CHANNEL_POSX,
                        HSF_CURVE_STEP, 2, g_stepKeys);
    set_transform_track(&g_tracks[1], 0, HSF_CHANNEL_POSX,
                        HSF_CURVE_LINEAR, 2, g_linearKeys);
    set_transform_track(&g_tracks[2], 0, HSF_CHANNEL_POSX,
                        HSF_CURVE_BEZIER, 2, g_bezierKeys);
    set_transform_track(&g_tracks[3], 0, HSF_CHANNEL_POSY,
                        HSF_CURVE_LINEAR, 2, g_yKeys);
    set_transform_track(&g_tracks[4], 1, HSF_CHANNEL_POSX,
                        HSF_CURVE_LINEAR, 2, g_otherKeys);
    set_transform_track(&g_tracks[5], 0, HSF_CHANNEL_POSX,
                        HSF_CURVE_CONST, 0, NULL);
    g_tracks[5].value = g_constValue;
}

static void setup_reused_scene(void)
{
    memset(&g_reusedHsf, 0, sizeof(g_reusedHsf));
    memset(&g_reusedMotion, 0, sizeof(g_reusedMotion));
    memset(&g_reusedObject, 0, sizeof(g_reusedObject));
    memset(&g_reusedTrack, 0, sizeof(g_reusedTrack));

    g_models[0].hsf = &g_reusedHsf;
    g_models[0].hsfLink = NULL;
    g_models[0].linkMdlId = HU3D_MODELID_NONE;
    g_reusedHsf.object = &g_reusedObject;
    g_reusedHsf.objectNum = 1;
    g_reusedHsf.motion = &g_reusedMotion;
    g_reusedHsf.motionNum = 1;
    g_reusedMotion.numTracks = 1;
    g_reusedMotion.track = &g_reusedTrack;
    g_reusedObject.name = (char *)"scenery";
    g_reusedObject.type = HSF_OBJ_ROOT;
    g_reusedObject.mesh.base.pos.x = 30.0f;
    set_transform_track(&g_reusedTrack, 0, HSF_CHANNEL_POSX,
                        HSF_CURVE_LINEAR, 2, g_reusedKeys);
}

static void check_close(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.0001f);
}

static void check_transactional_rejections(void)
{
    float nativeX = g_objects[0].mesh.base.pos.x;
    u16 savedCurve = g_tracks[0].curveType;

    mp6_widescreen_motion_x(-1, "scenery", 2.0f);
    mp6_widescreen_motion_x(HU3D_MODEL_MAX, "scenery", 2.0f);
    mp6_widescreen_motion_x(0, "not-present", 2.0f);
    mp6_widescreen_motion_x(0, "scenery", NAN);
    assert(g_objects[0].mesh.base.pos.x == nativeX);

    /* An unknown curve in a matching POSX track rejects the whole
     * registration; no partially snapshotted entry may be published. */
    g_tracks[0].curveType = 99;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    assert(g_objects[0].mesh.base.pos.x == nativeX);
    g_tracks[0].curveType = savedCurve;

    /* Linked HSF storage is shared by the engine and therefore not writable
     * through this opt-in helper. */
    g_models[0].hsfLink = &g_hsf;
    g_models[0].linkMdlId = 1;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    assert(g_objects[0].mesh.base.pos.x == nativeX);
    g_models[0].hsfLink = NULL;
    g_models[0].linkMdlId = HU3D_MODELID_NONE;

    /* A recycled normal slot can retain an old hsfLink pointer while its
     * linkMdlId has been reset.  That stale pointer must not make a live
     * native HSF look shared. */
    g_models[1].hsfLink = &g_hsf;
    g_models[1].linkMdlId = HU3D_MODELID_NONE;
    g_scale = 1.0f;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    g_scale = 1.75f;
    mp6_widescreen_motion_reapply();
    check_close(g_objects[0].mesh.base.pos.x, 16.0f);
    g_scale = 1.0f;
    mp6_widescreen_motion_reapply();
    check_close(g_objects[0].mesh.base.pos.x, nativeX);
    mp6_widescreen_motion_forget(0);
    g_models[1].hsfLink = NULL;

    /* A different live model may still link this HSF, or may expose a
     * distinct HSF wrapper around the same embedded motion. Both are shared
     * ownership and must reject registration. */
    memset(&g_otherHsf, 0, sizeof(g_otherHsf));
    g_models[1].hsfLink = &g_hsf;
    g_models[1].linkMdlId = 1;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    assert(g_objects[0].mesh.base.pos.x == nativeX);
    g_models[1].hsfLink = NULL;
    g_models[1].linkMdlId = HU3D_MODELID_NONE;
    g_otherHsf.motion = &g_motion;
    g_models[1].hsf = &g_otherHsf;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    assert(g_objects[0].mesh.base.pos.x == nativeX);
    g_models[1].hsf = NULL;
}

static void check_registration_and_reapply(void)
{
    HSF_LINEAR_KEY nativeStep[2];
    HSF_LINEAR_KEY nativeLinear[2];
    HSF_BEZIER_KEY nativeBezier[2];
    HSF_LINEAR_KEY nativeY[2];
    HSF_LINEAR_KEY nativeOther[2];
    float nativeConst;
    float nativeX;
    float nativeYBase;
    float nativeZBase;
    float nativeRot[3];
    float nativeScale[3];

    memcpy(nativeStep, g_stepKeys, sizeof(nativeStep));
    memcpy(nativeLinear, g_linearKeys, sizeof(nativeLinear));
    memcpy(nativeBezier, g_bezierKeys, sizeof(nativeBezier));
    memcpy(nativeY, g_yKeys, sizeof(nativeY));
    memcpy(nativeOther, g_otherKeys, sizeof(nativeOther));
    memcpy(&nativeConst, &g_tracks[5].value, sizeof(nativeConst));
    nativeX = g_objects[0].mesh.base.pos.x;
    nativeYBase = g_objects[0].mesh.base.pos.y;
    nativeZBase = g_objects[0].mesh.base.pos.z;
    nativeRot[0] = g_objects[0].mesh.base.rot.x;
    nativeRot[1] = g_objects[0].mesh.base.rot.y;
    nativeRot[2] = g_objects[0].mesh.base.rot.z;
    nativeScale[0] = g_objects[0].mesh.base.scale.x;
    nativeScale[1] = g_objects[0].mesh.base.scale.y;
    nativeScale[2] = g_objects[0].mesh.base.scale.z;

    g_scale = 1.0f;
    mp6_widescreen_motion_x(0, "scenery", 2.0f);
    assert(memcmp(nativeStep, g_stepKeys, sizeof(nativeStep)) == 0);
    assert(memcmp(nativeLinear, g_linearKeys, sizeof(nativeLinear)) == 0);
    assert(memcmp(nativeBezier, g_bezierKeys, sizeof(nativeBezier)) == 0);
    assert(memcmp(&nativeConst, &g_tracks[5].value, sizeof(nativeConst)) == 0);

    /* Reapply is a scale-change gate. A same-scale call does not overwrite
     * live animation edits made by the native HSF player. */
    mp6_widescreen_motion_reapply();
    g_objects[0].mesh.base.pos.x = -77.0f;
    g_stepKeys[0][1] = -88.0f;
    mp6_widescreen_motion_reapply();
    assert(g_objects[0].mesh.base.pos.x == -77.0f);
    assert(g_stepKeys[0][1] == -88.0f);
    g_objects[0].mesh.base.pos.x = nativeX;
    memcpy(g_stepKeys, nativeStep, sizeof(g_stepKeys));

    g_scale = 1.75f;
    mp6_widescreen_motion_reapply();
    check_close(g_objects[0].mesh.base.pos.x, 16.0f);
    check_close(g_stepKeys[0][1], 16.0f);
    check_close(g_stepKeys[1][1], 33.5f);
    check_close(g_linearKeys[0][1], 7.25f);
    check_close(g_linearKeys[1][1], 24.75f);
    check_close(g_bezierKeys[0][1], 12.5f);
    check_close(g_bezierKeys[1][1], 30.0f);
    check_close(g_bezierKeys[0][2], 2.1875f);
    check_close(g_bezierKeys[0][3], -3.5f);
    check_close(g_bezierKeys[1][2], 5.25f);
    check_close(g_bezierKeys[1][3], 7.875f);
    check_close(g_tracks[5].value, 19.5f);

    check_close(g_yKeys[0][1], nativeY[0][1]);
    check_close(g_yKeys[1][1], nativeY[1][1]);
    check_close(g_otherKeys[0][1], nativeOther[0][1]);
    check_close(g_otherKeys[1][1], nativeOther[1][1]);
    check_close(g_objects[0].mesh.base.pos.y, nativeYBase);
    check_close(g_objects[0].mesh.base.pos.z, nativeZBase);
    check_close(g_objects[0].mesh.base.rot.x, nativeRot[0]);
    check_close(g_objects[0].mesh.base.rot.y, nativeRot[1]);
    check_close(g_objects[0].mesh.base.rot.z, nativeRot[2]);
    check_close(g_objects[0].mesh.base.scale.x, nativeScale[0]);
    check_close(g_objects[0].mesh.base.scale.y, nativeScale[1]);
    check_close(g_objects[0].mesh.base.scale.z, nativeScale[2]);

    /* A second non-native scale derives from the original snapshots, not
     * from the already widened values. */
    g_objects[0].mesh.base.pos.x = -1.0f;
    g_bezierKeys[1][1] = -2.0f;
    g_scale = 1.325f;
    mp6_widescreen_motion_reapply();
    check_close(g_objects[0].mesh.base.pos.x, 12.6f);
    check_close(g_bezierKeys[1][1], 23.2f);
    check_close(g_bezierKeys[1][2], 3.975f);

    g_scale = 1.0f;
    mp6_widescreen_motion_reapply();
    assert(memcmp(nativeStep, g_stepKeys, sizeof(nativeStep)) == 0);
    assert(memcmp(nativeLinear, g_linearKeys, sizeof(nativeLinear)) == 0);
    assert(memcmp(nativeBezier, g_bezierKeys, sizeof(nativeBezier)) == 0);
    assert(memcmp(nativeY, g_yKeys, sizeof(nativeY)) == 0);
    assert(memcmp(nativeOther, g_otherKeys, sizeof(nativeOther)) == 0);
    assert(memcmp(&nativeConst, &g_tracks[5].value, sizeof(nativeConst)) == 0);
    check_close(g_objects[0].mesh.base.pos.x, nativeX);

    /* Duplicate registration is harmless and cannot replace the native
     * snapshot with a different pivot. */
    g_scale = 1.75f;
    mp6_widescreen_motion_reapply();
    mp6_widescreen_motion_x(0, "scenery", 100.0f);
    check_close(g_objects[0].mesh.base.pos.x, 16.0f);
}

static void check_forget_and_reused_id(void)
{
    mp6_widescreen_motion_forget(0);
    setup_reused_scene();
    g_scale = 1.0f;
    mp6_widescreen_motion_x(0, "scenery", 0.0f);
    g_scale = 1.75f;
    mp6_widescreen_motion_reapply();
    check_close(g_reusedObject.mesh.base.pos.x, 52.5f);
    check_close(g_reusedKeys[0][1], 52.5f);
    check_close(g_reusedKeys[1][1], 70.0f);

    mp6_widescreen_motion_forget(0);
    g_reusedObject.mesh.base.pos.x = 77.0f;
    g_scale = 1.0f;
    mp6_widescreen_motion_reapply();
    check_close(g_reusedObject.mesh.base.pos.x, 77.0f);
}

int main(void)
{
    setup_scene();
    check_transactional_rejections();
    check_registration_and_reapply();
    check_forget_and_reused_id();
    puts("widescreen motion native probe: PASS");
    return 0;
}
