#include <stdio.h>
#include <string.h>
#include "game/hu3d.h"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
HU3D_MOTION Hu3DMotion[HU3D_MOTION_MAX];
float minimumVcountf = 1.0f;
static HSF_DATA hsf;
static HSF_MOTION clip;

float Hu3DMotionShiftMaxTimeGet(HU3D_MODELID id) { return 44.0f; }
void mbPlayerMotionShiftSet(int p, int slot, float start, float blend, u32 attr) {
    Hu3DMotionShiftSet(p, slot, start, blend, attr);
}

#include "board_subject.inc"

static void reset(void) {
    memset(models, 0, sizeof(models));
    memset(Hu3DMotion, 0, sizeof(Hu3DMotion));
    clip.maxTime = 44.0f;
    hsf.motion = &clip;
    for (int i=0;i<16;i++) Hu3DMotion[i].hsf = &hsf;
    HU3D_MODEL *m = &Hu3DData[0];
    m->motId = 1;
    m->motIdShift = m->motIdOvl = m->motIdShape = -1;
    m->motAttr = HU3D_MOTATTR_LOOP & ~HU3D_MOTATTR;
    m->motWork.end = 44.0f;
    m->motWork.speed = 1.0f;
}

static int holds_at_end(void) {
    for (int i=0;i<300;i++) Hu3DMotionNext(0);
    HU3D_MODEL *m = &Hu3DData[0];
    if (m->motIdShift != -1 || m->motWork.time != 44.0f || (m->motAttr & 1)) {
        puts("FAIL one-shot did not stop at its last frame"); return 0;
    }
    for (int i=0;i<100;i++) {
        Hu3DMotionNext(0);
        if (m->motWork.time != 44.0f) { puts("FAIL end pose restarted"); return 0; }
    }
    return 1;
}

int main(void) {
    reset();
    subject_dk_reaction(0);
    if (!holds_at_end()) return 1;
    /* All stock one-shot slots: an existing loop must not leak through a
     * completed blend or a mid-blend replacement. */
    const int one_shot[] = {4,5,7,8,9,10,11,12,13};
    for (int i=0;i<sizeof(one_shot)/sizeof(one_shot[0]);i++) {
        reset();
        Hu3DMotionShiftSet(0, one_shot[i], 0, 8, HU3D_MOTATTR_NONE);
        if (!holds_at_end()) return 1;
        reset();
        Hu3DMotionShiftSet(0, 2, 0, 8, HU3D_MOTATTR_LOOP);
        Hu3DMotionNext(0);
        Hu3DMotionShiftSet(0, one_shot[i], 0, 8, HU3D_MOTATTR_NONE);
        if (!holds_at_end()) return 1;
    }
    /* Intentional loops still wrap after transitioning from a one-shot. */
    Hu3DMotionShiftSet(0, 1, 0, 8, HU3D_MOTATTR_LOOP);
    int wraps = 0;
    for (int i=0;i<300;i++) {
        float t = Hu3DData[0].motWork.time;
        int shifting = Hu3DData[0].motIdShift != -1;
        Hu3DMotionNext(0);
        if (!shifting && Hu3DData[0].motWork.time < t) wraps++;
    }
    if (wraps < 3) { puts("FAIL idle loop stopped"); return 1; }
    puts("PASS DK holds; one-shot blends and intentional loops preserved");
    return 0;
}
