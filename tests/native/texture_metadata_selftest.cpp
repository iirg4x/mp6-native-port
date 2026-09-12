#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <dolphin/gx.h>
namespace aurora::gfx { constexpr u32 InvalidTextureFormat = UINT32_MAX; }
#include "texture_metadata_types.inc"
constexpr unsigned MaxTextures=8;
struct State { std::array<GXTexObj_,MaxTextures> loadedTextures{}; bool stateDirty=false; } g_gxState;
#define CHECK(expr, ...) do { if(!(expr)) throw std::out_of_range("FIFO bounds"); } while(false)
static u64 read_integer(const u8* p, unsigned count, bool big) {
    u64 result=0;
    for(unsigned i=0;i<count;++i) result |= u64(p[i]) << (8*(big ? count-1-i : i));
    return result;
}
static u64 read_u64(const u8* p,bool big) { return read_integer(p,8,big); }
static u32 read_u32(const u8* p,bool big) { return u32(read_integer(p,4,big)); }
[[gnu::noinline]] static void decode(const u8* data,u32 size,u32& pos,bool bigEndian) {
#include "texture_metadata_subject.inc"
}
#ifdef METADATA_BENCH
[[gnu::noinline]] static void decode_reference(const u8* data,u32 size,u32& pos,bool bigEndian) {
#include "texture_metadata_reference.inc"
}
#endif
static bool equal(const GXTexObj_& a,const GXTexObj_& b) {
    return a.mode0==b.mode0 && a.mode1==b.mode1 && a.image0==b.image0 && a.image3==b.image3 &&
        a.userData==b.userData && a.data==b.data && a.mWidth==b.mWidth && a.mHeight==b.mHeight &&
        a.mFormat==b.mFormat && a.tlut==b.tlut && a.texObjId==b.texObjId &&
        a.texDataVersion==b.texDataVersion && a.flags==b.flags;
}
static auto payload(unsigned slot,const GXTexObj_& obj,bool big) {
    std::array<u8,34> data{}; unsigned pos=0;
    auto put=[&](u64 value,unsigned count) {
        for(unsigned i=0;i<count;++i) data[pos++]=u8(value>>(8*(big ? count-1-i : i)));
    };
    put(slot,1); put(uintptr_t(obj.data),8); put(obj.mWidth,4); put(obj.mHeight,4);
    put(obj.mFormat,4); put(obj.tlut,4); put(obj.flags&1,1); put(obj.texObjId,4); put(obj.texDataVersion,4);
    assert(pos==data.size()); return data;
}
static void apply(unsigned slot,const GXTexObj_& incoming,bool big,bool priorDirty=false) {
    const auto old=g_gxState.loadedTextures[slot];
    auto expected=old;
    expected.data=incoming.data; expected.mWidth=incoming.mWidth; expected.mHeight=incoming.mHeight;
    expected.mFormat=incoming.mFormat; expected.tlut=incoming.tlut;
    expected.texObjId=incoming.texObjId; expected.texDataVersion=incoming.texDataVersion;
    expected.flags=(old.flags&~0x81u)|(incoming.flags&1);
    const bool changed=!equal(old,expected);
    auto bytes=payload(slot,incoming,big); u32 pos=0;
    g_gxState.stateDirty=priorDirty;
    decode(bytes.data(),u32(bytes.size()),pos,big);
    assert(pos==bytes.size()); assert(equal(g_gxState.loadedTextures[slot],expected));
    // A decoder may conservatively retain a barrier for an identical load.
    // It must never remove one for changed data, uncached data or prior state.
    if(priorDirty || changed || incoming.texObjId==0) assert(g_gxState.stateDirty);
}
#ifdef METADATA_BENCH
static void benchmark() {
    std::array<std::array<u8,34>,195> stream{};
    GXTexObj_ obj; obj.mWidth=32; obj.mHeight=64; obj.mFormat=GX_TF_RGBA8; obj.texDataVersion=1;
    unsigned count=0;
    for(unsigned i=0;i<167;++i) {
        obj.texObjId=i+1; obj.data=reinterpret_cast<void*>(uintptr_t(0x10000+i*2048));
        stream[count++]=payload(0,obj,false);
        if(i<28) stream[count++]=payload(0,obj,false);
    }
    assert(count==195);
    static volatile unsigned sink=0;
    auto run=[&](auto fn) {
        g_gxState={}; unsigned sum=0;
        const auto begin=std::chrono::steady_clock::now();
        for(unsigned batch=0;batch<20000;++batch) for(const auto& bytes:stream) {
            u32 pos=0;
            fn(bytes.data(),34,pos,false);
            sum+=g_gxState.loadedTextures[0].texObjId+pos;
        }
        const auto end=std::chrono::steady_clock::now(); sink=sum;
        return std::chrono::duration<double,std::nano>(end-begin).count()/20000;
    };
    for(unsigned trial=0;trial<14;++trial) {
        double a,b;
        if(trial%2==0) { a=run(decode_reference); b=run(decode); }
        else { b=run(decode); a=run(decode_reference); }
        if(trial>=2) std::printf("trial=%u reference_ns=%.3f candidate_ns=%.3f\n",trial-2,a,b);
    }
}
#endif
int main(int argc,char**) {
#ifdef METADATA_BENCH
    if(argc>1) { benchmark(); return 0; }
#endif
    for(bool big:{false,true}) {
        g_gxState={}; GXTexObj_ initial;
        initial.data=reinterpret_cast<void*>(uintptr_t(0x123456789abcdef0ull));
        initial.mWidth=32; initial.mHeight=64; initial.mFormat=GX_TF_C8;
        initial.texObjId=41; initial.texDataVersion=9; initial.tlut=GX_TLUT1;
        for(unsigned slot=0;slot<MaxTextures;++slot) {
            apply(slot,initial,big); apply(slot,initial,big); apply(slot,initial,big,true);
            // Every metadata field independently forces a draw barrier.
            for(unsigned field=0;field<8;++field) {
                auto changed=initial;
                switch(field) {
                case 0: changed.data=reinterpret_cast<void*>(uintptr_t(0x1111111187654321ull)); break;
                case 1: ++changed.mWidth; break;
                case 2: ++changed.mHeight; break;
                case 3: changed.mFormat=GX_TF_RGBA8; break;
                case 4: changed.tlut=GX_TLUT2; break;
                case 5: changed.flags^=1; break;
                case 6: ++changed.texObjId; break;
                case 7: ++changed.texDataVersion; break;
                }
                apply(slot,changed,big); apply(slot,initial,big);
            }
            auto& state=g_gxState.loadedTextures[slot];
            // Interleaved BP writes survive metadata and their dirty flag stays set.
            state.mode0=0x1234; state.mode1=0x5678; state.image0=0x3456; state.image3=0x9876;
            state.flags|=0x40; apply(slot,initial,big,true);
            state.set_no_cache(true); apply(slot,initial,big); // a new load restores cache eligibility
            auto uncached=initial; uncached.texObjId=0;
            apply(slot,uncached,big); apply(slot,uncached,big);
        }
        auto bytes=payload(0,initial,big);
        for(u32 length=0;length<34;++length) {
            u32 pos=0; bool caught=false;
            try { decode(bytes.data(),length,pos,big); } catch(const std::out_of_range&) { caught=true; }
            assert(caught);
        }
        bytes[0]=8; u32 pos=0; bool caught=false;
        try { decode(bytes.data(),34,pos,big); } catch(const std::out_of_range&) { caught=true; }
        assert(caught);
        u32 rng=0x19453827;
        for(unsigned i=0;i<50000;++i) {
            rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
            const unsigned slot=rng%8; auto next=g_gxState.loadedTextures[slot];
            if(i%4==0) next.texDataVersion=rng;
            if(i%13==0) next.texObjId=rng;
            if(i%997==0) next.texObjId=0;
            apply(slot,next,big,i%7==0);
            if(i%1009==0) g_gxState={};
        }
    }
    puts("PASS: 100000 mixed texture loads, both byte orders, every metadata field, BP interleaving, zero IDs, bounds and dirty preservation");
}
