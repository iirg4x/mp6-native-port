#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game/hu3d.h"
#include "game/memory.h"
#include "mp6_widescreen.h"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HU3D_MODELID lbl_1_bss_2BC[4][2];
static HU3D_MODELID lbl_1_bss_2F4[4][3];
static int lbl_1_data_28[4] = {1,2,4,8};
static HuVec2f lbl_1_data_48[4] = {{0,0},{3208,0},{4346,1673},{996,3610}};
static struct {int stageSection;} lbl_1_bss_114[4];
static int enabled = 1, reads, creates, frees, extensions, failRead, failCreate;
static float factor = 1.75f;
int mp6_widescreen_enabled(void) { return enabled; }
float mp6_widescreen_scale_factor(void) { return factor; }
void *HuDataSelHeapReadNum(int file, s32 tag, HEAPID heap)
{
    assert(tag == HU_MEMNUM_OVL && heap == HEAP_MODEL);
    ++reads;
    return failRead ? NULL : malloc(8);
}
void HuMemDirectFree(void *data) { ++frees; free(data); }
HU3D_MODELID Hu3DModelCreate(void *data)
{
    ++creates;
    if (failCreate) return HU3D_MODELID_NONE;
    free(data); return 20 + creates;
}
void Hu3DModelPosSet(HU3D_MODELID id, float x, float y, float z) { models[id].pos=(HuVecF){x,y,z}; }
void Hu3DModelCameraSet(HU3D_MODELID id, u16 mask) { models[id].cameraBit=mask; }
void Hu3DModelAttrSet(HU3D_MODELID id, u32 attr) { models[id].attr|=attr; }
void Hu3DModelAttrReset(HU3D_MODELID id, u32 attr) { models[id].attr&=~attr; }
void mp6_widescreen_extend_edge_span(s16 id, const char *name, int axis, float boundary, float span)
{
    assert(id >= 21 && axis == 0 && span == 800); ++extensions;
}
void mp6_widescreen_extend_color_rim(s16 id, const char *name, int axis, float x, float y)
{
    assert(id >= 21 && (axis == 1 || axis == 2)); ++extensions;
}
void mp6_widescreen_tile_mirror_x(s16 id, float left, float right, float halfView)
{
    if (id == HU3D_MODELID_NONE) return;
    assert(id >= 21 && fabsf(left + 308.4627f) < 0.001f);
    assert(fabsf(right - 3316.575f) < 0.001f && halfView == 800.0f);
}
#include "mp6_m618_widescreen.h"

int main(void)
{
    portWideWallsInit();
    for (int stage=0; stage<4; ++stage) for (int part=0; part<3; ++part) {
        assert(portWideBackdrops[stage][part] == -1);
        lbl_1_bss_2F4[stage][part]=8+stage*3+part;
        models[8+stage*3+part].cameraBit=15;
    }
    for (int stage=0; stage<4; ++stage) for (int side=0; side<2; ++side) {
        assert(portWideWalls[stage][side] == HU3D_MODELID_NONE);
        lbl_1_bss_2BC[stage][side]=stage*2+side;
        models[stage*2+side].cameraBit=15;
    }
    enabled=0; failRead=1; portWideWallCreate(0,0,83,"wall",-428);
    assert(reads == 1 && creates == 0 && portWideWalls[0][0] == -1);
    failRead=0; failCreate=1; portWideWallCreate(0,0,83,"wall",-428);
    assert(frees == 1 && extensions == 0 && portWideWalls[0][0] == -1);
    failCreate=0;
    portWideWallCreate(0,0,83,"left",-428);
    portWideWallCreate(0,1,84,"right",2853);
    portWideWallCreate(2,0,96,"right",2000);
    portWideWallCreate(2,1,97,"left",-412);
    portWideWallCreate(3,0,101,"right",950);
    assert(extensions == 5);
    portWideBackdropCreate(1,0,76,"ita01",1,0,0);
    portWideBackdropCreate(3,1,80,"back",2,1525,0);
    portWideBackdropCreate(3,2,81,"front",2,1500,-30);
    assert(extensions == 8);
    /* All companions are prepared while OFF, but only native models draw. */
    portWideWallsUpdate();
    assert(models[portWideWalls[0][0]].cameraBit==0 && models[0].cameraBit==15);
    assert(models[portWideBackdrops[3][1]].cameraBit==0 && models[18].cameraBit==15);
    enabled=1;
    for (int frame=0; frame<100; ++frame) {
        for (int player=0; player<4; ++player) lbl_1_bss_114[player].stageSection=(frame+player)%4;
        portWideWallsUpdate();
        for (int stage=0; stage<4; ++stage) for (int side=0; side<2; ++side) {
            int wide=portWideWalls[stage][side], native=lbl_1_bss_2BC[stage][side];
            if (wide < 0) continue;
            int expected=0;
            for (int player=0; player<4; ++player) if (lbl_1_bss_114[player].stageSection==stage) expected|=1<<player;
            assert(models[wide].cameraBit==expected && models[native].cameraBit==(15^expected));
            assert((models[wide].cameraBit & models[native].cameraBit)==0);
            assert(models[wide].pos.x==lbl_1_data_48[stage].x && models[wide].pos.y==lbl_1_data_48[stage].y);
        }
        for (int stage=0; stage<4; ++stage) for (int part=0; part<3; ++part) {
            int wide=portWideBackdrops[stage][part], native=lbl_1_bss_2F4[stage][part];
            if (wide < 0) continue;
            int expected=0;
            for (int player=0; player<4; ++player) if (lbl_1_bss_114[player].stageSection==stage) expected|=1<<player;
            assert(models[wide].cameraBit==expected && models[native].cameraBit==(15^expected));
            assert(models[wide].attr & HU3D_MOTATTR_LOOP);
        }
    }
    /* Visibility and winner camera restrictions belong to the native scene. */
    models[0].attr=HU3D_ATTR_DISPOFF; portWideWallsUpdate();
    assert(models[portWideWalls[0][0]].attr & HU3D_ATTR_DISPOFF);
    models[0].attr=0; portWideWallsUpdate();
    assert(!(models[portWideWalls[0][0]].attr & HU3D_ATTR_DISPOFF));
    portWallCameras[3][0]=2;
    portBackdropCameras[3][1]=portBackdropCameras[3][2]=2;
    for (int player=0; player<4; ++player) lbl_1_bss_114[player].stageSection=3;
    portWideWallsUpdate();
    assert(models[portWideWalls[3][0]].cameraBit==2 && models[6].cameraBit==0);
    assert(models[portWideBackdrops[3][1]].cameraBit==2 && models[18].cameraBit==0);
    assert(models[portWideBackdrops[3][2]].cameraBit==2 && models[19].cameraBit==0);
    factor=1; portWideWallsUpdate();
    assert(models[portWideWalls[3][0]].cameraBit==0 && models[6].cameraBit==2);
    assert(models[0].cameraBit==15);
    assert(models[portWideBackdrops[3][1]].cameraBit==0 && models[18].cameraBit==2);
    factor=1.75f; enabled=0; portWideWallsUpdate();
    assert(models[portWideWalls[0][0]].cameraBit==0 && models[0].cameraBit==15);
    assert(extensions==8 && creates==9 && reads==10); /* no reload during toggles */
    portWideWallsInit();
    assert(portWallCameras[3][0]==15 && portWideWalls[3][0]==-1);
    assert(portBackdropCameras[3][1]==15 && portWideBackdrops[3][1]==-1);
    return 0;
}
