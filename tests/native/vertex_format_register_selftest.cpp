#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>
#include <dolphin/gx/GXEnum.h>
#include "selftest_assert.h"
static_assert(sizeof(u32)==4 && sizeof(u16)==2 && sizeof(u8)==1);
constexpr u32 MaxVtxAttr=GX_VA_MAX_ATTR,MaxVtxFmt=GX_MAX_VTXFMT;
#include "vertex_formats.inc"
struct State {
    std::array<GXAttrType,MaxVtxAttr> vtxDesc{};
    std::array<VtxFmt,MaxVtxFmt> vtxFmts{};
    struct Array { u8 stride=0; };
    std::array<Array,MaxVtxAttr> arrays{};
    u32 currentPnMtx=0,clears=0;
    bool stateDirty=false;
    GXVtxFmt lastVtxFmt=GX_VTXFMT0;
#include "vertex_format_cache_fields.inc"
    void clearVtxSizeCache() {
#ifndef MP6_REG_BENCH
        ++clears;
#endif
        lastVtxFmt=GX_MAX_VTXFMT;
    }
};
struct Logger { unsigned errors=0; template<class... T> void error(const char*,T...) { ++errors; } };
inline u32 bp_get(u32 reg,u32 size,u32 shift) { return (reg>>shift)&((1u<<size)-1); }
namespace reference {
State g_gxState;Logger Log;
#include "vertex_format_reference.inc"
}
namespace candidate {
State g_gxState;Logger Log;
#include "vertex_format_subject.inc"
}
std::array<u8,26> registers() {
    std::array<u8,26> a{0x50,0x60};
    for(unsigned i=0;i<24;++i) a[2+i]=u8(0x70+16*(i/8)+i%8);
    return a;
}
void equal_state() {
    const auto& a=reference::g_gxState;const auto& b=candidate::g_gxState;
    assert(a.vtxDesc==b.vtxDesc);
    assert(!std::memcmp(a.vtxFmts.data(),b.vtxFmts.data(),sizeof(a.vtxFmts)));
    assert(a.currentPnMtx==b.currentPnMtx);
    for(unsigned i=0;i<MaxVtxAttr;++i) assert(a.arrays[i].stride==b.arrays[i].stride);
    assert(reference::Log.errors==candidate::Log.errors);
}
void write(u8 addr,u32 value,bool endian,bool repeated) {
    auto& a=reference::g_gxState;auto& b=candidate::g_gxState;
    const auto clears=b.clears;
    a.stateDirty=b.stateDirty=false;
    a.lastVtxFmt=b.lastVtxFmt=GX_VTXFMT3;
    reference::handle_cp(addr,value,endian);candidate::handle_cp(addr,value,endian);
    equal_state();
    if(repeated) {
        assert(!b.stateDirty && b.clears==clears && b.lastVtxFmt==GX_VTXFMT3);
        b.stateDirty=true;
        candidate::handle_cp(addr,value,endian);
        assert(b.stateDirty && b.clears==clears); // Never erase another command's dirtiness.
    } else {
        assert(b.stateDirty==a.stateDirty && b.lastVtxFmt==a.lastVtxFmt);
    }
}
void correctness() {
    for(bool endian:{false,true}) {
        reference::g_gxState={};candidate::g_gxState={};
        for(auto addr:registers()) {
            write(addr,0,endian,false);write(addr,0,endian,true);
            for(unsigned bit=0;bit<32;++bit) {
                write(addr,1u<<bit,endian,false);write(addr,1u<<bit,endian,true);
            }
            write(addr,0xffffffffu,endian,false);write(addr,0xffffffffu,endian,true);
        }
        assert(candidate::g_gxState.vertexFormatRegsValid==((1u<<26)-1));
        // Exercise mixed formats and changed-back values against the unmodified decoder.
        reference::g_gxState={};candidate::g_gxState={};
        std::array<std::optional<u32>,256> values;
        u32 rng=0x13579bdf;
        auto next=[&] { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; };
        const auto addrs=registers();
        for(unsigned i=0;i<100000;++i) {
            if(i%10007==0) { reference::g_gxState={};candidate::g_gxState={};values={}; }
            const auto addr=addrs[next()%addrs.size()];
            const u32 value=(next()%10<8 && values[addr])?*values[addr]:next();
            const bool repeated=values[addr] && *values[addr]==value;
            write(addr,value,endian,repeated);values[addr]=value;
        }
        // State copies/resets carry cache validity with the decoded state.
        const auto savedA=reference::g_gxState,savedB=candidate::g_gxState;
        write(0x50,0xaabbccdd,endian,false);
        reference::g_gxState=savedA;candidate::g_gxState=savedB;equal_state();
        const u32 saved=savedB.vertexFormatRegs[0];
        write(0x50,saved,endian,true);
        // Matrix state has another writer (XF): repeated CP writes MUST execute.
        write(0x30,6,endian,false);
        reference::g_gxState.currentPnMtx=candidate::g_gxState.currentPnMtx=9;
        write(0x30,6,endian,false);
        assert(candidate::g_gxState.currentPnMtx==2);
        for(unsigned addr=0;addr<256;++addr) {
            if(values[addr]) continue;
            const auto valid=candidate::g_gxState.vertexFormatRegsValid;
            const auto cache=candidate::g_gxState.vertexFormatRegs;
            write(u8(addr),123,endian,false);write(u8(addr),123,endian,false);
            assert(candidate::g_gxState.vertexFormatRegsValid==valid);
            assert(candidate::g_gxState.vertexFormatRegs==cache);
        }
    }
    puts("PASS: 26 pure registers, every input bit, mixed writes, first-zero/reset/copy, dirty/size-cache preservation, matrix and array exclusions");
}
struct Write { u8 addr;u32 value; };
[[gnu::noinline]] void old_write(u8 addr,u32 value) { reference::handle_cp(addr,value,false); }
[[gnu::noinline]] void new_write(u8 addr,u32 value) { candidate::handle_cp(addr,value,false); }
double bench(void(*fn)(u8,u32),const std::vector<Write>& writes) {
    const auto start=std::chrono::steady_clock::now();
    for(unsigned repeat=0;repeat<3000;++repeat) for(auto w:writes) fn(w.addr,w.value);
    return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/3000;
}
int main(int argc,char** argv) {
    if(argc==2 && std::string_view(argv[1])=="--bench") {
        std::vector<Write> writes;
        for(unsigned i=0;i<183;++i) {
            writes.push_back({0x50,0x00016a00u|(i/8)%2});
            writes.push_back({0x60,(i/6)%2});
            writes.push_back({0x70,0x40000009u|(((i/30)%2)<<4)});
            writes.push_back({0x80,0});writes.push_back({0x90,0});
        }
        for(unsigned trial=0;trial<14;++trial) {
            double oldNs,newNs;
            if(trial%2) { newNs=bench(new_write,writes);oldNs=bench(old_write,writes); }
            else { oldNs=bench(old_write,writes);newNs=bench(new_write,writes); }
            if(trial>=2) std::printf("trial=%u writes=%zu reference_ns=%.3f candidate_ns=%.3f\n",trial,writes.size(),oldNs,newNs);
        }
        return 0;
    }
    correctness();
}
