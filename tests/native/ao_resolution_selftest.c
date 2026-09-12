#include "mp6_ao_resolution.h"
#include <assert.h>
int main(void)
{
    unsigned w,h;
#ifdef __ANDROID__
    const unsigned limit=960;
#else
    const unsigned limit=1920;
#endif
    mp6_ao_working_size(1920,1080,&w,&h);
    assert(w==limit && h==limit*9/16);
    mp6_ao_working_size(3840,2160,&w,&h);
    assert(w==limit && h==limit*9/16);
    mp6_ao_working_size(2401,1081,&w,&h);
    assert(w==limit && h==(1081*limit+2400)/2401);
    mp6_ao_working_size(1081,2401,&w,&h);
    assert(h==limit && w==(1081*limit+2400)/2401);
    mp6_ao_working_size(640,480,&w,&h);
    assert(w==640 && h==480);
    mp6_ao_working_size(0,0,&w,&h);
    assert(w==1 && h==1);
    return 0;
}
