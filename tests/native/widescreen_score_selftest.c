#include <assert.h>
#include <math.h>
typedef struct { float x, y; } HuVec2f;
typedef struct { int box, seconds, hundredths, separator; } M657ScoreTeam;
typedef struct { M657ScoreTeam teams[2]; } M657ScoreWork;
static HuVec2f lbl_1_data_1E8[2]={{142,56},{432,56}};
static float margin, positions[8][2];
static int calls;
float mp6_widescreen_half_width_delta(void) { return margin; }
void MgScorePosSet(int id, float x, float y)
{
    assert(id>=0 && id<8);
    positions[id][0]=x; positions[id][1]=y; ++calls;
}
void MgScoreBoxPosSet(int id,float x,float y) { MgScorePosSet(id,x,y); }
void HuSprGrpPosSet(int id,float x,float y) { MgScorePosSet(id,x,y); }
#include "score_subject.inc"
int main(void)
{
    M657ScoreWork work={{{0,1,2,3},{4,5,6,7}}};
    const float margins[]={0,93.6f,216,0,216,93.6f,0};
    for(int pass=0;pass<100;++pass) {
        margin=margins[pass%7]; calls=0;
        mp6_m657_score_positions(&work);
        assert(calls==8);
        for(int team=0;team<2;++team) {
            int id=team*4;
            float viewCenter=team ? 432+0.5f*margin : 144-0.5f*margin;
            float x=positions[id][0];
            assert(fabsf(x-viewCenter-(team?0:-2))<0.001f);
            assert(positions[id+1][0]==x-32);
            assert(positions[id+2][0]==x+16);
            assert(positions[id+3][0]==x);
            for(int i=0;i<4;++i) assert(positions[id+i][1]==56);
        }
    }
    return 0;
}
