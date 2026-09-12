#include <assert.h>
#include <stdlib.h>
#include "selftest_assert.h"
typedef int BOOL;
typedef short s16;
#define FALSE 0
static int stars, calls, sleeps, ticks, positive, negative, finish, displays, shown, waits;
static int mbPlayerStarGet(int p) { assert(p==2); return stars; }
static void mbPlayerStarAdd(int p,int n) { assert(p==2); stars+=n; calls++; }
static void mbAudFXPlay(int id) {
    if(id==8) positive++; else if(id==50) negative++; else {assert(id==15); finish++;}
}
static void HuPrcSleep(int n) { sleeps++; ticks+=n; }
static void mbStarDispPlayerCreate(int p,int n) { assert(p==2); displays++; shown=n; }
static int mbStarDispCheck(int p) { assert(p==2); return waits==2; }
static void HuPrcVSleep(void) { waits++; }
#include "star_subject.inc"
int main(void) {
    const int starting[]={0,1,20,500,998,999};
    const int changes[]={-1200,-50,-20,-1,0,1,19,20,49,50,1200};
    for(int i=0;i<6;i++) for(int j=0;j<11;j++)
    for(int mode=0;mode<3;mode++) for(int fast=0;fast<2;fast++) for(int disp=0;disp<2;disp++) {
        stars=starting[i]; calls=sleeps=ticks=positive=negative=finish=displays=shown=waits=0;
        int n=changes[j], end=stars+n;
        if(end<0) end=0; if(end>999) end=999;
        int delta=end-stars, actual;
        if(mode==0) actual=mbStarAddProcExec(2,n,disp,fast);
        else if(mode==1) actual=mbStarAddDispExec(2,n,disp,fast);
        else actual=mbStarAddExec(2,n);
        assert(actual==delta && stars==end);
        int fast_used=mode==2?0:fast;
        int count=abs(delta), delay=abs(n)>=50?1:abs(n)>=20?3:6;
        assert(sleeps==(fast_used?0:count));
        assert(ticks==(fast_used?0:count*delay));
        assert(calls==(fast_used?(mode==0 && n==0?0:1):count));
        assert(positive==(!fast_used && delta>0?count:0));
        assert(negative==(!fast_used && delta<0?count:0));
        assert(finish==(mode==0?(delta!=0 || disp):(delta!=0)));
        int display=mode==0?(delta!=0 || disp):mode==1?(disp && delta!=0):0;
        assert(displays==display && waits==display*2);
        if(display) assert(shown==delta);
    }
    return 0;
}
