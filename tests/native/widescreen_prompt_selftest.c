#include <assert.h>
#include <math.h>
#include <string.h>
typedef struct { short sprites[5]; } Team;
typedef struct { Team teams[2]; } M638SceneView;
static float margin, positions[10][2];
static int calls;
float mp6_widescreen_half_width_delta(void) { return margin; }
void espPosSet(short sprite, float x, float y)
{
    assert(sprite >= 0 && sprite < 10);
    positions[sprite][0]=x; positions[sprite][1]=y; ++calls;
}
#include "prompt_subject.inc"
int main(void)
{
    M638SceneView scene;
    for (int team=0;team<2;++team)
        for (int i=0;i<5;++i) scene.teams[team].sprites[i]=team*5+i;
    const float margins[]={0,93.6f,216,0,216,93.6f,0};
    for (int pass=0;pass<100;++pass) {
        margin=margins[pass%7]; calls=0;
        mp6_m638_prompt_positions(&scene);
        assert(calls==10);
        for (int i=0;i<5;++i) {
            /* The centered 576-wide canvas spans [-margin,576+margin]. */
            assert(fabsf(positions[i][0]+margin-50)<0.001f);
            assert(fabsf(576+margin-positions[5+i][0]-50)<0.001f);
            assert(positions[i][1]==150 && positions[5+i][1]==150);
        }
    }
    return 0;
}
