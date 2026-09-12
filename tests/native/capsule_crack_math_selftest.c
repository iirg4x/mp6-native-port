#include <math.h>
#include <stdio.h>
#include "board_subject.inc"
double __frsqrte(double x) {return 1.0/sqrt(x);}
#define CHECK(c) do {if (!(c)) {printf("FAIL %d: %s\n",__LINE__,#c);return 1;}} while (0)
int main(void) {
    const float input[]={.0001f,.25f,1,2,9,65536,1e12f};
    for (int i=0;i<7;i++) {
        volatile float a=0,b=0;
        float result=CapEffCrackSqrt(input[i],&a);
        CapEffCrackSqrtStore(input[i],&a,&b);
        CHECK(isfinite(result) && fabsf(result-sqrtf(input[i]))<.0001f*fmaxf(result,1));
        CHECK(result==a && result==b);
    }
    puts("Capsule crack finite native normalization: PASS");
    return 0;
}
