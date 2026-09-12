#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXCommandList.h>
#include "selftest_assert.h"
#define ZoneScoped
#define ZoneScopedN(...)
#define CHECK(condition, ...) assert(condition)
#define FATAL(...) std::abort()
namespace fmt { template<class... T> std::string format(const char*, T...) { return {}; } }
struct Logger {
    template<class... T> void warn(const char*, T...) {}
    template<class... T> void error(const char*, T...) {}
    template<class... T> void debug(const char*, T...) {}
} Log;
namespace gfx { struct Range { u32 offset=0, size=0; }; }
#include "array.inc"
struct State { std::array<AttrArray,GX_VA_MAX_ATTR> arrays{}; bool stateDirty=false; } g_gxState;
static std::vector<u8> storage;
static std::vector<std::array<u8,3>> draws;
static unsigned groups, uploads;
static u32 read_u32(const u8* p, bool be) {
    u32 v; std::memcpy(&v,p,4); return be ? __builtin_bswap32(v) : v;
}
static u16 read_u16(const u8* p, bool be) {
    u16 v; std::memcpy(&v,p,2); return be ? __builtin_bswap16(v) : v;
}
static bool copy_xf_data(u16, const u8*, u16, bool) { std::abort(); }
static void handle_cp(u8,u32,bool) { std::abort(); }
static void handle_xf(const u8*,u32&,u32,bool) { std::abort(); }
static void handle_aurora(const u8*,u32&,u32,bool) { std::abort(); }
static void handle_bp(u32 value, bool) {
    // Mock an in-place source update between two FIFO draws. Source ownership
    // and upload contents remain independent; no GPU work is mocked as timing.
    for (auto attr : {GX_VA_POS,GX_VA_NRM,GX_VA_TEX0}) {
        *const_cast<u8*>(static_cast<const u8*>(g_gxState.arrays[attr].data)) = u8(value+attr);
    }
}
static void handle_draw(u8, const u8*, u32&, u32, bool) {
    if(g_gxState.stateDirty || groups==0) ++groups;
    std::array<u8,3> values{}; unsigned n=0;
    for(auto attr : {GX_VA_POS,GX_VA_NRM,GX_VA_TEX0}) {
        auto& a=g_gxState.arrays[attr];
        if(!a.cachedRange.size) {
            a.cachedRange={u32(storage.size()),a.size};
            auto* data=static_cast<const u8*>(a.data);
            storage.insert(storage.end(),data,data+a.size); ++uploads;
        }
        values[n++]=storage[a.cachedRange.offset];
    }
    draws.push_back(values);
    g_gxState.stateDirty=false;
}
#include "dispatch.inc"

int main() {
    std::array<std::array<u8,4>,GX_VA_MAX_ATTR> arrays{};
    for(unsigned i=0;i<arrays.size();++i) {
        arrays[i].fill(u8(i));
        g_gxState.arrays[i]={arrays[i].data(),4,u8(i+1),bool(i%2),{0,0}};
    }
    const auto layout=g_gxState.arrays;
    const u8 draw[]={GX_TRIANGLES};
    process(draw,sizeof(draw),true);
    assert(uploads==3 && groups==1);
    process(draw,sizeof(draw),true);
    assert(uploads==3 && groups==1); // normal unchanged draws still reuse uploads
    const u8 changed[]={GX_LOAD_BP_REG,0,0,0,70,GX_CMD_INVL_VC,GX_NOP,GX_TRIANGLES};
    process(changed,sizeof(changed),true);
    assert(draws.back()[0]==70+GX_VA_POS); // fails when INVL_VC is silently ignored
    assert(draws.front()[0]==GX_VA_POS && storage[0]==GX_VA_POS);
    assert(uploads==6 && groups==2); // invalidation also prevents merging old/new ranges
    for(auto attr : {GX_VA_POS,GX_VA_NRM,GX_VA_TEX0})
        assert(draws.back()[attr==GX_VA_POS?0:attr==GX_VA_NRM?1:2]==70+attr);
    const u8 invalidate[]={GX_CMD_INVL_VC};
    process(invalidate,sizeof(invalidate),false);
    process(invalidate,sizeof(invalidate),true);
    assert(g_gxState.stateDirty && uploads==6); // lazy upload, not a copy on invalidation
    for(unsigned i=0;i<arrays.size();++i) {
        const auto& a=g_gxState.arrays[i];
        assert(!a.cachedRange.size && !a.cachedRange.offset);
        assert(a.data==layout[i].data && a.size==layout[i].size);
        assert(a.stride==layout[i].stride && a.le==layout[i].le);
    }
    process(draw,sizeof(draw),false);
    assert(uploads==9 && groups==3 && draws.back()==draws[2]);
    for(unsigned i=0;i<1000;++i) {
        handle_bp(i%200,false);
        process(invalidate,sizeof(invalidate),i%2);
        process(draw,sizeof(draw),i%2);
        assert(draws.back()[0]==i%200+GX_VA_POS);
    }
    puts("Vertex invalidation: updated snapshots, preserved old draws, merge boundary and layouts passed");
}
