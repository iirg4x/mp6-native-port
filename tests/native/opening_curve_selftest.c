#include <math.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { float x, y, z; } HuVecF;
static float VECMag(const HuVecF *v) { return sqrtf(v->x*v->x+v->y*v->y+v->z*v->z); }
#include "board_subject.inc"
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Real indirection exposes default float promotion and pointer truncation. */
static OPENINGCURVEEVALFUNC volatile callback = OpeningCurveEval;

int main(void)
{
    HuVecF a={0,0,0}, b={100,0,0}, c={100,0,0}, d={100,0,0};
    CHECK((uintptr_t)callback == (uintptr_t)OpeningCurveEval);
    CHECK(fabsf(OpeningCurveLength(callback,&a,&b,&c,&d,1)-100)<0.001f);
    for (int i=0;i<=16;i++) {
        float t=i/16.0f;
        CHECK(fabsf(callback(&a,&b,&c,&d,t)-100)<0.001f);
        CHECK(fabsf(OpeningCurveIntegrate(callback,&a,&b,&c,&d,t)-100*t)<0.001f);
        CHECK(fabsf(OpeningCurveNewton(callback,&a,&b,&c,&d,0.4f,100*t,10)-t)<0.001f);
    }
    a=(HuVecF){-300,50,-100}; b=(HuVecF){600,700,200};
    c=(HuVecF){1000,200,-200}; d=(HuVecF){700,-100,300};
    for (int i=0;i<=16;i++) {
        float t=i/16.0f;
        CHECK(fabsf(callback(&a,&b,&c,&d,t)-OpeningCurveEval(&a,&b,&c,&d,t))<0.001f);
        float distance=OpeningCurveIntegrate(callback,&a,&b,&c,&d,t);
        CHECK(fabsf(OpeningCurveNewton(callback,&a,&b,&c,&d,0.4f,distance,10)-t)<0.002f);
    }
    puts("opening native curve callback: PASS");
    return 0;
}
