#include <assert.h>
#include <stdint.h>
#include "selftest_assert.h"
typedef struct {float x,y,z;} HuVecF;
static HuVecF *expected;
static int seen;
static int StarPlayerCreate(int p,HuVecF *pos) {
    assert(p==3 && pos==expected);
    assert(pos->x==12.5f && pos->y==456.0f && pos->z==-98.0f);
    seen++; return 19;
}
static int ZtarPlayerCreate(int p,HuVecF *pos) { return StarPlayerCreate(p,pos); }
#include "star_subject.inc"
int main(void) {
    HuVecF pos={12.5f,456.0f,-98.0f}; expected=&pos;
    assert(sizeof(void*)==8);
    assert(test_Star(3,&pos)==19 && test_Ztar(3,&pos)==19 && seen==2);
    return 0;
}
