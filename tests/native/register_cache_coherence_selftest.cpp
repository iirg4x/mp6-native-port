#include <array>
#include <bitset>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <dolphin/gx/GXEnum.h>
#include "selftest_assert.h"
static_assert(sizeof(u32)==4 && sizeof(u16)==2);
template<class T> using Vec4=std::array<T,4>;
namespace GX { using LightMask=std::bitset<8>; }
constexpr u32 MaxColorChannels=4, MaxTexCoord=8, MaxTluts=1024;
#include "register_types.inc"
struct State {
    std::array<u32,256> bpRegCache=[] { std::array<u32,256> a{}; a[0xFE]=0xFFFFFF; return a; }();
#include "xfRegCache.inc"
#include "xfRegCacheValid.inc"
    std::array<ColorChannelConfig,4> colorChannelConfig{};
    std::array<ColorChannelState,4> colorChannelState{};
    std::array<TcgConfig,8> tcgs{};
    u32 numChans=0, numTexGens=0, numTevStages=0, numIndStages=0, currentPnMtx=0;
    GXCullMode cullMode=GX_CULL_NONE;
    bool stateDirty=false;
    struct Tlut { u32 loadTlut0=0; u16 numEntries=0; };
    std::array<Tlut,MaxTluts> loadedTluts{};
    u32 texCopyDest=0, copies=0;
};
struct Logger { template<class... T> void warn(const char*, T...) {} };
static u32 bp_get(u32 value,u32 count,u32 shift) { return (value>>shift)&((1u<<count)-1); }
static Vec4<float> unpack_color(u32 value) {
    return {float((value>>24)&255)/255, float((value>>16)&255)/255,
            float((value>>8)&255)/255, float(value&255)/255};
}
namespace reference {
State g_gxState; Logger Log;
void copy_tex(u32,bool) { ++g_gxState.copies; }
#include "register_reference.inc"
}
namespace candidate {
State g_gxState; Logger Log;
void copy_tex(u32,bool) { ++g_gxState.copies; }
#include "register_subject.inc"
}
void equal() {
    const auto& a=reference::g_gxState; const auto& b=candidate::g_gxState;
    assert(a.numChans==b.numChans && a.numTexGens==b.numTexGens);
    assert(a.numTevStages==b.numTevStages && a.numIndStages==b.numIndStages && a.cullMode==b.cullMode);
    assert(a.currentPnMtx==b.currentPnMtx);
    assert(a.copies==b.copies);
    assert(a.bpRegCache==b.bpRegCache); // Never invalidate raw words used by masked writes.
    assert(a.colorChannelConfig==b.colorChannelConfig && a.tcgs==b.tcgs);
    for(unsigned i=0;i<4;++i) {
        assert(a.colorChannelState[i].matColor==b.colorChannelState[i].matColor);
        assert(a.colorChannelState[i].ambColor==b.colorChannelState[i].ambColor);
        assert(a.colorChannelState[i].lightMask==b.colorChannelState[i].lightMask);
    }
    for(unsigned i=0;i<MaxTluts;++i) {
        assert(a.loadedTluts[i].loadTlut0==b.loadedTluts[i].loadTlut0);
        assert(a.loadedTluts[i].numEntries==b.loadedTluts[i].numEntries);
    }
}
void reset() { reference::g_gxState={}; candidate::g_gxState={}; }
void bp(u32 value) {
    reference::handle_bp(value,false); candidate::handle_bp(value,false); equal();
}
void xf(u32 reg,u32 value) {
    reference::handle_scalar_xf(reg,value); candidate::handle_scalar_xf(reg,value); equal();
}
void cp(u32 value) {
    reference::handle_matrix_cp(value); candidate::handle_matrix_cp(value); equal();
}
void regression(std::string_view name) {
    reset();
    if(name=="zero_xf") { xf(0x18,0); xf(0x19,0); assert(candidate::g_gxState.tcgs[0].mtx==static_cast<GXTexMtx>(0)); }
    if(name=="zero_bp") { bp(0); assert(candidate::g_gxState.numTevStages==1); }
    if(name=="matrix") { xf(0x18,3); cp(6); xf(0x18,3); }
    if(name=="channels") { xf(0x09,1); bp(0x20); xf(0x09,1); }
    if(name=="genmode") { bp(0x11); xf(0x09,2); xf(0x3f,3); bp(0x11); }
    if(name=="tlut") { bp(0x64000100); bp(0x65000c02); bp(0x64000200); bp(0x65000c02); }
}
int main(int argc,char** argv) {
    if(argc==2) { regression(argv[1]); return 0; }
    for(auto name:{"zero_xf","zero_bp","matrix","channels","genmode","tlut"}) regression(name);
    // Validity follows state copies/resets; repeated pure XF writes stay cheap
    // and never clear pre-existing dirtiness from another command.
    reset();
    for(u32 reg=0;reg<=0x19;++reg) {
        xf(reg,0);
        candidate::g_gxState.stateDirty=false;
        xf(reg,0); assert(!candidate::g_gxState.stateDirty);
        candidate::g_gxState.stateDirty=true;
        xf(reg,0); assert(candidate::g_gxState.stateDirty);
    }
    const auto savedA=reference::g_gxState, savedB=candidate::g_gxState;
    cp(27); xf(0x09,2);
    reference::g_gxState=savedA; candidate::g_gxState=savedB;
    candidate::g_gxState.stateDirty=false;
    xf(0x18,0); assert(!candidate::g_gxState.stateDirty);
    u32 rng=0x716224a9;
    auto next=[&] { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; };
    u32 gen=0x11;
    for(unsigned i=0;i<100000;++i) {
        if(i%9973==0) reset();
        switch(next()%12) {
        case 0: bp(gen); break;
        case 1: gen=(next()%9)|((next()%3)<<4)|((next()%16)<<10)|((next()%4)<<14); bp(gen); break;
        case 2: xf(0x09,next()%3); break;
        case 3: xf(0x3f,next()%9); break;
        case 4: cp(next()%30); break;
        case 5: xf(0x18,(next()%61)<<6 | next()%30); break;
        case 6: xf(0x19,next()%61); break;
        case 7: xf(0x0a+next()%4,next()); break;
        case 8: xf(0x0e + next()%4,next()); break;
        case 9: bp(0xfe000070); bp(gen); break; // BP merge must use last raw word, not last XF values.
        case 10: bp(0x64000000 | next()%0x10000); bp(0x65000c02); break;
        case 11: bp(0x52000000); bp(0x52000000); break;
        }
    }
    puts("PASS: first-zero writes, CP/XF matrix, BP/XF counts, masked BP, palette reload, copy triggers, reset/copy, 100000 mixed commands");
}
