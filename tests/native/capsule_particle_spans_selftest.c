/* Run the actual patched capsule draw bindings against exact array extents. */
#include "dolphin.h"
#include "game/hu3d.h"
#include "mp6_gxarray_registry.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {int attr; const void *data; uint32_t bytes; uint8_t stride;} BIND;
static BIND observed[3];
static int used;
void mp6_gxarray_bind_span(int attr, const void *data, uint32_t bytes, uint8_t stride)
{
    if (used >= 3) abort();
    observed[used++] = (BIND){attr, data, bytes, stride};
}
#include "board_subject.inc"

#define CHECK(c) do {if (!(c)) {printf("FAIL line %d: %s\n",__LINE__,#c);return 1;}} while(0)
#define CHECK_BIND(i,a,p,n,s) CHECK(observed[i].attr==(a) && observed[i].data==(p) && observed[i].bytes==(n) && observed[i].stride==(s))
#define COLOUR_BYTES(array,n) ((uint32_t)((char *)&(array)[(n)-1].color + sizeof(GXColor) - (char *)&(array)[0].color))
int main(void)
{
    CAPEFFRAYPARTICLEWORK ray = {0};
    used=0; qa_ray(&ray);
    CHECK(used==2);
    CHECK_BIND(0,GX_VA_POS,ray.prevVtx,sizeof(ray.prevVtx),sizeof(HuVecF));
    CHECK_BIND(1,GX_VA_CLR0,ray.colorLerp,sizeof(ray.colorLerp),sizeof(GXColor));
    CHECK(sizeof(ray.prevVtx)==192 && sizeof(ray.colorLerp)==32);

    static CAPEFFGLOWPARTICLEWORK glow[192];
    static CAP_EFF_CRACK_DATA crack[192];
    static CAP_EFF_DATA effect[192];
    static HuVecF vertices[192*4];
    static HuVec2f uv[192*4];
    const int counts[]={1,32,192};
    for (int i=0;i<3;i++) {
        int n=counts[i];
        QA_GLOW g={n,glow,vertices,uv};
        used=0; qa_glow(&g);
        CHECK(used==3);
        CHECK_BIND(0,GX_VA_POS,vertices,n*4*sizeof(HuVecF),sizeof(HuVecF));
        CHECK_BIND(1,GX_VA_CLR0,&glow[0].color,COLOUR_BYTES(glow,n),sizeof(glow[0]));
        CHECK_BIND(2,GX_VA_TEX0,uv,n*4*sizeof(HuVec2f),sizeof(HuVec2f));

        QA_CRACK c={n,n*3,crack,vertices,uv};
        used=0; qa_crack(&c);
        CHECK(used==3);
        CHECK_BIND(0,GX_VA_POS,vertices,n*3*sizeof(HuVecF),sizeof(HuVecF));
        CHECK_BIND(1,GX_VA_CLR0,&crack[0].color,COLOUR_BYTES(crack,n),sizeof(crack[0]));
        CHECK_BIND(2,GX_VA_TEX0,uv,n*3*sizeof(HuVec2f),sizeof(HuVec2f));

        QA_EFFECT e={n,effect,vertices,uv};
        used=0; qa_effect(&e);
        CHECK(used==3);
        CHECK_BIND(0,GX_VA_POS,vertices,n*4*sizeof(HuVecF),sizeof(HuVecF));
        CHECK_BIND(1,GX_VA_CLR0,&effect[0].color,COLOUR_BYTES(effect,n),sizeof(effect[0]));
        CHECK_BIND(2,GX_VA_TEX0,uv,n*4*sizeof(HuVec2f),sizeof(HuVec2f));
    }
    puts("Capsule ray, glow, crack and trail draw spans: PASS");
    return 0;
}
