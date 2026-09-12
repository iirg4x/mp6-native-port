#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HU3D_CAM_MAX 16
#define HU3D_MODEL_MAX 4
typedef struct { float x,y,z; } Vec;
typedef struct { Vec pos,target,up; float fov; } HU3D_CAMERA;
static HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
static struct { void *hsf; } *Hu3DData;
static uint32_t s_generation[HU3D_MODEL_MAX];
static unsigned calculations, context_resets, defer_resets;
static uint32_t next_generation(uint32_t n) { return n+1; }
static void mp6_fi_capture_context_reset(void) { ++context_resets; }
static void mp6_fi_capture_defer_reset(void) { ++defer_resets; }
#include "camera-under-test.inc"

static void compare_all(void) {
    for(int c=-2;c<HU3D_CAM_MAX+2;++c) {
        int expected=fi_camera_stable_uncached(c);
        unsigned before=calculations;
        int wasCached=c>=0 && c<HU3D_CAM_MAX && s_camStable[c]!=0;
        for(int n=0;n<200;++n) assert(mp6_fi_model_camera_stable(c)==expected);
        assert(calculations-before==(unsigned)(c>=0 && c<HU3D_CAM_MAX && !wasCached));
    }
}

int main(void) {
    compare_all(); /* no snapshots */
    for(int c=0;c<HU3D_CAM_MAX;++c)
        Hu3DCamera[c]=(HU3D_CAMERA){.pos={0,0,100},.target={0,0,0},.up={0,1,0},.fov=45};
    mp6_fi_model_snapshot();compare_all();
    assert(!mp6_fi_model_camera_stable(0)); /* first snapshot cannot pair */
    mp6_fi_model_snapshot();compare_all();
    assert(mp6_fi_model_camera_stable(0));
    /* Live game camera writes cannot alter the already captured frame. */
    Hu3DCamera[0].pos.x=1000;
    assert(mp6_fi_model_camera_stable(0));
    mp6_fi_model_snapshot();compare_all();
    assert(!mp6_fi_model_camera_stable(0));
    for(int tick=0;tick<300;++tick) {
        for(int c=0;c<HU3D_CAM_MAX;++c) {
            HU3D_CAMERA *p=&Hu3DCamera[c];
            *p=(HU3D_CAMERA){.pos={0,0,100},.target={0,0,0},.up={0,1,0},.fov=45};
            switch((tick+c)%12) {
            case 0:p->pos.x=50;break;
            case 1:p->target.y=49.999f;break;
            case 2:p->target.y=50;break;
            case 3:p->up=(Vec){0,0,0};break;
            case 4:p->up=(Vec){1,0,0};break;
            case 5:p->up.y=NAN;break;
            case 6:p->pos.z=INFINITY;break;
            case 7:p->fov=-1;break;
            case 8:p->fov=50;break;
            case 9:p->fov=NAN;break;
            case 10:p->up=(Vec){.1f,1,0};break;
            }
        }
        mp6_fi_model_snapshot();compare_all();
        if(tick%17==0) {
            mp6_fi_model_reset();compare_all();
            for(int c=0;c<HU3D_CAM_MAX;++c)assert(!mp6_fi_model_camera_stable(c));
        }
    }
    assert(context_resets && context_resets==defer_resets);
    puts("Camera cache agrees with every original cut decision; one calculation per snapshot");
    return 0;
}
