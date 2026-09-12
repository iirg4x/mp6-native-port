#include "../../src/hsf/grounding_math.h"
#include <assert.h>

int main(void)
{
    GroundTri flat={{0,-10,0},{100,-10,0},{0,-10,100}};
    GroundTri slope={{0,0,0},{100,10,0},{0,0,100}};
    GroundTri wall={{0,0,0},{0,100,0},{0,0,100}};
    GroundTri degenerate={{0,0,0},{0,0,0},{0,0,0}};
    float y=0;
    assert(ground_height(&flat,10,20,&y) && fabsf(y+10)<1e-5f);
    assert(!ground_height(&flat,90,90,&y));
    assert(!ground_height(&flat,-1,20,&y));
    assert(ground_height(&slope,20,20,&y) && fabsf(y-2)<1e-5f);
    assert(!ground_height(&wall,0,0,&y));
    assert(!ground_height(&degenerate,0,0,&y));
    return 0;
}
