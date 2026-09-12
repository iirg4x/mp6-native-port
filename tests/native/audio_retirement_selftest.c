#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define MP6_MSM_MAX_CHAN 16
#define MP6_MSM_MAX_SFX_VOICES 64
#define MP6_MSM_OUT_CHANNELS 2
#define MP6_MSM_MAX_SE_GROUPS 4
#define MP6_MSM_ERR_RANGE_STREAM -1
#define MP6_MSM_ERR_INVALIDSE -2
#define MSM_ERR_PLAYFAIL -3
#define MSM_SEPARAM_VOL 1
#define MSM_SEPARAM_PAN 2
typedef int s32;
typedef int BOOL;
enum { MP6_FADE_NONE,MP6_FADE_TO_STOP,MP6_FADE_TO_PAUSE,MP6_FADE_TO_PLAY };
typedef struct { int segCount; } Env;
typedef struct {
    int active,paused,loop,baseVol,vol,fadeAction;
    int16_t* pcm;
    uint32_t totalFrames,loopStartFrame,loopEndFrame,tlWraps;
    uint64_t posFrac,stepFrac;
    float fadeMul,fadeStep,gainL,gainR;
    Env envPlan;
    int seId,no,gid,keyGroup,pan;
    uint64_t tlStartTick;
} MsmChan;
typedef MsmChan MsmSeVoice;
MsmChan g_chan[MP6_MSM_MAX_CHAN];
MsmSeVoice g_sfxVoice[MP6_MSM_MAX_SFX_VOICES];
int g_chanMax=MP6_MSM_MAX_CHAN,g_mixerLockInit=0,g_savestateMixerMuted=0;
int g_masterVol=127,g_seMasterVol=127,g_wavArmed=0;
int g_sfxVoiceCap=MP6_MSM_MAX_SFX_VOICES,requestedCap=MP6_MSM_MAX_SFX_VOICES;
unsigned long g_kgReleaseEvents=0,g_kgVoicesReleased=0;
uint64_t mp6_tick_count=1;
struct { int inUse,baseGrpF;uint16_t gid; } g_seGroups[MP6_MSM_MAX_SE_GROUPS];
typedef struct { int flag,vol,pan; } MSM_SEPARAM;
typedef struct { int seId,vol,pan,paused,seNo,fadeAction;uint64_t posFrac;float fadeMul,fadeStep; } Mp6SsAudioVoice;
_Atomic int g_pcmRetired;
int locked=0,freed=0,lockCount=0,reactivateOnLock=0;
void mp6_lock(void) {
    assert(!locked);locked=1;++lockCount;
    if(reactivateOnLock) { g_chan[0].active=1;reactivateOnLock=0; }
}
void mp6_unlock(void) { assert(locked);locked=0; }
void checked_free(void* pointer) { assert(!locked && pointer);++freed;free(pointer); }
#define free checked_free
int mp6_sfx_voice_cap(void) { return g_sfxVoiceCap; }
int mp6_enh_sfx_voices(void) { return requestedCap; }
int mp6_msm_voice_cap_clamp(int cap) { return cap; }
int mp6_se_keygroup_disabled(void) { return 0; }
int mp6_se_timeline_on(void) { return 1; }
float mp6_fade_step_from_speed(int speed) { return speed>0?0.5f:0; }
void mp6_grp_lock(void) { assert(!locked); }
void mp6_grp_unlock(void) { assert(!locked); }
MsmSeVoice* find_sfx_voice_by_no(int no) {
    for(int i=0;i<g_sfxVoiceCap;++i)if(g_sfxVoice[i].active && g_sfxVoice[i].no==no)return &g_sfxVoice[i];
    return NULL;
}
int msmSePlay(int seId,MSM_SEPARAM* param);
int checked_printf(const char* format,...) {
    assert(!locked);va_list args;va_start(args,format);int result=vprintf(format,args);va_end(args);return result;
}
#define printf checked_printf
int16_t mp6_clamp16(int x) { return x>32767?32767:x<-32768?-32768:(int16_t)x; }
int mp6_msm_resolve_end_q16(uint64_t* pos,int loop,uint32_t start,uint32_t end) {
    if(!loop || end<=start) return 0;
    *pos=((uint64_t)start<<16)+(*pos-((uint64_t)start<<16))%((uint64_t)(end-start)<<16);
    return 1;
}
float mp6_msm_env_mul(const Env* env,uint64_t idx) { (void)env;(void)idx;return 1; }
void mp6_wav_capture(int16_t* out,uint32_t frames,int armed) {
    (void)out;(void)frames;assert(!locked && !armed);
}
#include "audio_subject.inc"
#undef free
#undef printf

MsmChan voice(int stereo) {
    MsmChan c={0};
    c.active=1;c.baseVol=c.vol=127;c.fadeMul=1;c.gainL=c.gainR=1;
    c.totalFrames=c.loopEndFrame=4;c.stepFrac=65536;
    c.pcm=malloc((stereo?8:4)*sizeof(int16_t));assert(c.pcm);
    for(int i=0;i<(stereo?8:4);++i)c.pcm[i]=(int16_t)(100*(i+1));
    return c;
}
int msmSePlay(int seId,MSM_SEPARAM* param) {
    (void)seId;(void)param;
    g_sfxVoice[0]=voice(0);g_sfxVoice[0].no=123;
    return 123;
}
void reset(void) {
    assert(!locked);
    for(int i=0;i<MP6_MSM_MAX_CHAN;++i)free(g_chan[i].pcm);
    for(int i=0;i<MP6_MSM_MAX_SFX_VOICES;++i)free(g_sfxVoice[i].pcm);
    memset(g_chan,0,sizeof(g_chan));memset(g_sfxVoice,0,sizeof(g_sfxVoice));
    g_pcmRetired=0;g_savestateMixerMuted=0;freed=0;
    g_sfxVoiceCap=requestedCap=MP6_MSM_MAX_SFX_VOICES;
}
int main(void) {
    int16_t out[24];
    memset(out,1,sizeof(out));mp6_msm_render(out,12);
    for(int i=0;i<24;++i)assert(out[i]==0);
    assert(!lockCount);g_mixerLockInit=1;
    // Natural endings: exact stereo and mono samples, no allocation release in callback.
    g_chan[0]=voice(1);g_sfxVoice[0]=voice(0);
    mp6_msm_render(out,12);
    for(int i=0;i<8;++i)assert(out[i]==100*(i+1)+100*(i/2+1));
    for(int i=8;i<24;++i)assert(out[i]==0);
    assert(!g_chan[0].active && !g_sfxVoice[0].active && !freed);
    assert(g_chan[0].pcm && g_sfxVoice[0].pcm);
    mp6_msm_collect_finished();assert(freed==2 && !g_chan[0].pcm && !g_sfxVoice[0].pcm);
    mp6_msm_collect_finished();assert(freed==2);
    // Fade-to-stop: same two-sample ramp, reclaim both kinds off the callback.
    reset();g_chan[0]=voice(1);g_sfxVoice[0]=voice(0);
    g_chan[0].fadeAction=g_sfxVoice[0].fadeAction=MP6_FADE_TO_STOP;
    g_chan[0].fadeStep=g_sfxVoice[0].fadeStep=-0.5f;
    mp6_msm_render(out,12);
    assert(out[0]==200 && out[1]==300 && out[2]==250 && out[3]==300);
    for(int i=4;i<24;++i)assert(out[i]==0);
    assert(!freed);mp6_msm_collect_finished();assert(freed==2);
    // Paused, looping and save-state-muted voices keep their buffers.
    reset();g_chan[0]=voice(1);g_sfxVoice[0]=voice(0);
    g_chan[0].paused=1;g_sfxVoice[0].loop=1;
    mp6_msm_render(out,12);
    for(int i=0;i<24;++i)assert(out[i]==100*((i/2)%4+1));
    g_pcmRetired=1;mp6_msm_collect_finished();
    assert(!freed && g_chan[0].pcm && g_sfxVoice[0].pcm);
    g_savestateMixerMuted=1;mp6_msm_render(out,12);
    for(int i=0;i<24;++i)assert(!out[i]);
    // The lock-protected state, not the retirement flag, decides ownership.
    g_chan[0].active=0;g_pcmRetired=1;reactivateOnLock=1;
    mp6_msm_collect_finished();assert(!freed && g_chan[0].pcm);
    // Full retirement queue including SFX slots above the configured playback cap.
    reset();
    for(int i=0;i<MP6_MSM_MAX_CHAN;++i) { g_chan[i]=voice(1);g_chan[i].active=0; }
    for(int i=0;i<MP6_MSM_MAX_SFX_VOICES;++i) { g_sfxVoice[i]=voice(0);g_sfxVoice[i].active=0; }
    g_pcmRetired=1;mp6_msm_collect_finished();
    assert(freed==MP6_MSM_MAX_CHAN+MP6_MSM_MAX_SFX_VOICES);
    mp6_msm_collect_finished();assert(freed==MP6_MSM_MAX_CHAN+MP6_MSM_MAX_SFX_VOICES);
    reset();
    // Immediate and deferred control paths use the same sample lifetime.
    g_chan[0]=voice(1);assert(msmStreamStop(0,0)==0 && freed==1 && !g_chan[0].pcm);
    g_chan[0]=voice(1);assert(msmStreamStop(0,1)==0 && freed==1 && g_chan[0].active);
    mp6_msm_render(out,12);mp6_msm_collect_finished();assert(freed==2);
    g_chan[0]=voice(1);g_chan[1]=voice(1);msmStreamStopAll(0);assert(freed==4);
    reset();
    g_sfxVoice[0]=voice(0);g_sfxVoice[0].no=44;
    assert(msmSeStop(44,0)==0 && freed==1 && !g_sfxVoice[0].pcm);
    assert(msmSeStop(44,0)==MP6_MSM_ERR_INVALIDSE && freed==1);
    g_sfxVoice[0]=voice(0);g_sfxVoice[0].no=45;
    assert(msmSeStop(45,1)==0 && g_sfxVoice[0].active && freed==1);
    mp6_msm_render(out,12);mp6_msm_collect_finished();assert(freed==2);
    reset();
    g_sfxVoice[0]=voice(0);g_sfxVoice[0].gid=1;
    g_sfxVoice[1]=voice(0);g_sfxVoice[1].gid=2;
    g_seGroups[0].inUse=1;g_seGroups[0].baseGrpF=1;g_seGroups[0].gid=1;
    msmSeStopAll(1,0);assert(freed==1 && g_sfxVoice[0].active && !g_sfxVoice[1].pcm);
    msmSeStopAll(0,0);assert(freed==2 && !g_sfxVoice[0].pcm);
    reset();
    for(int i=0;i<MP6_MSM_MAX_SFX_VOICES;++i)g_sfxVoice[i]=voice(0);
    requestedCap=16;mp6_msm_apply_voice_limit();
    assert(g_sfxVoiceCap==16 && freed==48 && g_sfxVoice[15].pcm && !g_sfxVoice[16].pcm);
    reset();
    MsmPcmRetirement retired={0};MsmKgRelRec records[MP6_MSM_MAX_SFX_VOICES];
    for(int i=0;i<MP6_MSM_MAX_SFX_VOICES;++i) { g_sfxVoice[i]=voice(0);g_sfxVoice[i].keyGroup=1; }
    mp6_lock();assert(mp6_se_keygroup_release(1,1,records,&retired)==MP6_MSM_MAX_SFX_VOICES);
    assert(!freed);mp6_unlock();mp6_pcm_release(&retired);assert(freed==MP6_MSM_MAX_SFX_VOICES);
    mp6_pcm_release(&retired);assert(freed==MP6_MSM_MAX_SFX_VOICES);
    reset();
    Mp6SsAudioVoice saved={.seId=1,.vol=127,.pan=64,.seNo=9,.fadeMul=1};
    g_sfxVoice[1]=voice(0);g_sfxVoice[1].active=0;
    assert(mp6_msm_savestate_restore_voice(&saved,1)==0);
    assert(freed==1 && !g_sfxVoice[0].pcm && g_sfxVoice[1].active && g_sfxVoice[1].no==9);
    assert(mp6_msm_savestate_restore_voice(&saved,1)==MSM_ERR_PLAYFAIL);
    assert(freed==2 && !g_sfxVoice[0].pcm && g_sfxVoice[1].no==9);
    reset();
    puts("PASS: PCM, endings, loop/pause/mute, controls/retrigger/voice-limit/restore, off-lock retirement");
}
