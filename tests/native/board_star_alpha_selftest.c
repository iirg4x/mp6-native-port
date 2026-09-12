#include <assert.h>
#include <math.h>
#include "mp6_hwcast.h"
typedef unsigned char u8;
void mp6_hwcast_saturation_report(float v,const char *s) {(void)v;(void)s;}
#include "star_subject.inc"
int main(void) {
    assert(alpha(0.0f)==255 && alpha(0.5f)==127 && alpha(1.0f)==0);
    assert(alpha(NAN)==0 && alpha(INFINITY)==0 && alpha(-INFINITY)==255);
    assert(alpha(2.0f)==1);
    for(int i=0;i<=1000;i++) {float w=i/1000.0f; assert(alpha(w)==(u8)(255.0f*(1.0f-w)));}
    return 0;
}
