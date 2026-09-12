#include <stdio.h>
#include <stdlib.h>
#include <math.h>
typedef struct { float x, y, z; } HuVecF;
static float VECMag(const HuVecF *v) { return sqrtf(v->x*v->x + v->y*v->y + v->z*v->z); }
#define __fabs fabs
#include "board_subject.inc"

#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* Volatile indirection prevents optimization from hiding a mismatched ABI. */
static W01CurveEval volatile bezier = mp6W01BezierEval;
static W01CurveEval volatile hermite = fn_1_14BF0;

int main(void)
{
    HuVecF a = {0,0,0}, b = {50,0,0}, c = {100,0,0};
    double slope;
    CHECK(fabsf(w01CurveLen(bezier,&a,&b,&c)-100.0f)<0.001f);
    for (int i=0;i<=16;i++) {
        float t = i/16.0f;
        CHECK(fabsf(bezier(&a,&b,&c,NULL,t)-fn_1_14A90(&a,&b,&c,t))<0.001f);
        CHECK(fabsf(w01CurveLen2(bezier,&a,&b,&c,t)-100*t)<0.001f);
        CHECK(fabsf(w01CurveT(bezier,&a,&b,&c,0.4f,100*t,&slope)-t)<0.001f);
    }
    b = (HuVecF){100,0,0}; c = (HuVecF){100,0,0};
    HuVecF d = {100,0,0};
    for (int i=0;i<=16;i++) {
        float t = i/16.0f;
        CHECK(fabsf(hermite(&a,&b,&c,&d,t)-100)<0.001f);
        CHECK(fabsf(W01HermiteIntegrate(hermite,&a,&b,&c,&d,t)-100*t)<0.001f);
        CHECK(fabsf(W01CurveNewton(hermite,&a,&b,&c,&d,0.4f,100*t,10)-t)<0.001f);
        CHECK(fabsf(fn_1_144C0(hermite,&a,&b,&c,&d,0.4f,100*t,10)-t)<0.001f);
        CHECK(fabsf(fn_1_147DC(hermite,&a,&b,&c,&d,0.4f,100*t,10)-t)<0.001f);
    }
    /* Curved, non-axis-aligned control points exercise changing derivatives. */
    a=(HuVecF){-400,600,-200}; b=(HuVecF){200,1300,400}; c=(HuVecF){1000,600,-600};
    float reference=0, step=1.0f/10000;
    for (int i=0;i<10000;i++) reference += fn_1_14A90(&a,&b,&c,(i+0.5f)*step)*step;
    float length=w01CurveLen(bezier,&a,&b,&c);
    CHECK(isfinite(length) && fabsf(length-reference)/reference<0.002f);
    for (int i=0;i<=10;i++) {
        float t=i/10.0f, distance=w01CurveLen2(bezier,&a,&b,&c,t);
        CHECK(fabsf(w01CurveT(bezier,&a,&b,&c,0.5f,distance,&slope)-t)<0.002f);
    }
    puts("board native curve callbacks: PASS");
    return 0;
}
