#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <chrono>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXCommandList.h>
template<class T>struct ArrayRef { const T* data;size_t size; };
#define LIKELY
#define UNLIKELY
#define ZoneScoped
#define ZoneScopedN(x)
struct TestLog {
    template<class... Args> [[noreturn]] void fatal(Args&&...) {
        throw std::runtime_error("renderer fatal");
    }
} Log;
// Use the production macro definitions. Keep fixture assertions enabled:
// NDEBUG selects only the renderer macros, not the C assertion header above.
#ifdef MP6_RENDERER_RELEASE
#define NDEBUG
#endif
#include "checks.inc"
#ifdef MP6_RENDERER_RELEASE
#undef NDEBUG
#endif
static unsigned bufferWrites;
#include "buffer.inc"
#include "indices.inc"

namespace gfx {
using PipelineRef=u32;
struct Range { u32 offset=0,size=0; };
static ByteBuffer verts,indices;
static u32 g_mergedDrawCallCount,uploads;
static Range push(ByteBuffer& buffer,const u8* data,size_t size,size_t alignment) {
    if(alignment && buffer.size()%alignment)buffer.append_zeroes(alignment-buffer.size()%alignment);
    Range r{static_cast<u32>(buffer.size()),static_cast<u32>(size)};
    if(size)buffer.append(data,size);++uploads;return r;
}
static Range push_verts(const u8* data,size_t size,size_t alignment) { return push(verts,data,size,alignment); }
static Range push_indices(const u8* data,size_t size,size_t alignment) { return push(indices,data,size,alignment); }
static Range push_storage(const u8*,size_t) { std::abort(); }
}
struct GXBindGroups {};
#include "drawdata.inc"
static std::vector<DrawData> commands;
struct State {
    bool stateDirty=true;
    GXVtxFmt lastVtxFmt=GX_MAX_VTXFMT;
    u32 lastVtxSize=0,dstAlpha=0;
    GXAttrType vtxDesc[GX_VA_TEX7+1]{};
    struct { gfx::Range cachedRange;const void* data;size_t size; } arrays[GX_VA_TEX7+1]{};
} g_gxState;
struct BindGroupRanges { gfx::Range vaRanges[GX_VA_TEX7-GX_VA_POS+1]; };
struct PipelineConfig {};struct ShaderInfo {};
static void populate_pipeline_config(PipelineConfig&,GXPrimitive,GXVtxFmt) {}
namespace gfx {
static PipelineRef find_gx_pipeline(PipelineConfig&,ShaderInfo&) { return 1; }
template<class T>static T* get_last_draw_command() { return commands.empty()?nullptr:&commands.back(); }
static void push_draw_command(DrawData d) { commands.push_back(d); }
}
static void resolve_sampled_textures(const ShaderInfo&) {}
static GXBindGroups build_bind_groups(const ShaderInfo&) { return {}; }
static gfx::Range build_uniform(const ShaderInfo&,u32,BindGroupRanges&) { g_gxState.stateDirty=false;return {}; }
static void note_efb_depth_write() {}
static u32 calculate_last_vtx_size(GXVtxFmt fmt) { g_gxState.lastVtxFmt=fmt;return g_gxState.lastVtxSize=(fmt==GX_VTXFMT0?4:8); }
[[noreturn]] static void handle_draw_overrun(uint64_t,const u8*,const u32&,u32) { throw std::runtime_error("overrun"); }
static void handle_draw_unmerged(GXPrimitive,GXVtxFmt,u16,gfx::Range);
static void push_gx_draw(GXPrimitive,GXVtxFmt,u16,gfx::Range,gfx::Range,u32);
static ByteBuffer handle_draw_idx_buf,handle_draw_unmerged_idxBuf;
#include "draws.inc"
static constexpr u8 CP_VAT_MASK=GX_VAT_MASK,CP_OPCODE_MASK=GX_OPCODE_MASK;
static u16 read_u16(const u8* p,bool be) { u16 n;std::memcpy(&n,p,2);return be?u16(p[0]*256+p[1]):n; }
static u32 read_u32(const u8* p,bool be) { u32 n;std::memcpy(&n,p,4);return be?(u32(p[0])<<24|u32(p[1])<<16|u32(p[2])<<8|p[3]):n; }
#include "ordinary.inc"
#include "sized.inc"
#include "indexed.inc"

static void reset() {
    commands.clear();gfx::verts.clear();gfx::indices.clear();gfx::uploads=0;
    gfx::g_mergedDrawCallCount=0;g_gxState=State{};
    handle_draw_idx_buf.clear();handle_draw_unmerged_idxBuf.clear();
}
static std::vector<u16> expected(GXPrimitive prim,u32 count,u32 base) {
    std::vector<u16> out;
    auto tri=[&](u32 a,u32 b,u32 c){out.push_back(base+a);out.push_back(base+b);out.push_back(base+c);};
    switch(prim) {
    case GX_QUADS:
        for(u32 q=0;q<count/4;++q) { u32 v=q*4;tri(v,v+1,v+2);tri(v+2,v+3,v); }
        if(count%4==3)tri(count-3,count-2,count-1);
        break;
    case GX_TRIANGLES:
        for(u32 q=0;q<count/3;++q)tri(q*3,q*3+1,q*3+2);
        break;
    case GX_TRIANGLEFAN:
        for(u32 i=2;i<count;++i)tri(0,i-1,i);
        break;
    case GX_TRIANGLESTRIP:
        for(u32 i=2;i<count;++i) { if(i%2)tri(i-1,i-2,i);else tri(i-2,i-1,i); }
        break;
    case GX_LINES:if(count>=2)out={0,1,3,3,2,0};break;
    case GX_LINESTRIP:if(count>=2)out={0,1,3,3,2,0};break;
    case GX_POINTS:if(count)out={0,1,3,3,2,0};break;
    default:std::abort();
    }
    return out;
}
static void topology() {
    ByteBuffer buf;
    const GXPrimitive prims[]={GX_QUADS,GX_TRIANGLES,GX_TRIANGLEFAN,GX_TRIANGLESTRIP,GX_LINES,GX_LINESTRIP,GX_POINTS};
    std::vector<u32> counts;
    for(u32 n=0;n<=512;++n)counts.push_back(n);
    for(u32 n=65520;n<=65535;++n)counts.push_back(n);
    for(auto prim:prims)for(auto count:counts)for(u32 base:std::array<u32,2>{0,65535-count}) {
        buf.clear();const u16 guard=0xbeef;buf.append(guard);bufferWrites=0;
        auto ref=expected(prim,count,base);
        u32 n=prepare_idx_buffer(buf,prim,base,count);
        assert(n==ref.size() && buf.size()==sizeof(guard)+ref.size()*2);
        assert(std::memcmp(buf.data(),&guard,sizeof(guard))==0);
        if(!ref.empty())assert(std::memcmp(buf.data()+2,ref.data(),ref.size()*2)==0);
#ifndef MP6_INDEX_BENCHMARK
        assert(bufferWrites==(ref.empty()?0u:1u));
#endif
    }
    bool rejected=false;try {prepare_idx_buffer(buf,GX_QUADS,65532,4);}catch(...) {rejected=true;}
    assert(rejected);
}
static std::vector<u8> raw(70000*8,0x46);
static void draw(GXPrimitive prim,u16 count,GXVtxFmt fmt=GX_VTXFMT0) {
    u32 pos=0;draw_prim(prim,fmt,count,raw.data(),pos,raw.size());
    assert(pos==count*(fmt==GX_VTXFMT0?4u:8u));
}
static void batching() {
    reset();draw(GX_TRIANGLES,6);draw(GX_QUADS,4);draw(GX_TRIANGLESTRIP,5);
    assert(commands.size()==1 && commands[0].vtxCount==15 && commands[0].indexCount==21);
    auto ref=expected(GX_TRIANGLES,6,0);
    for(auto pair:{std::pair{GX_QUADS,4u},std::pair{GX_TRIANGLESTRIP,5u}}) {
        auto next=expected(pair.first,pair.second,pair.first==GX_QUADS?6:10);
        ref.insert(ref.end(),next.begin(),next.end());
    }
    assert(gfx::indices.size()==ref.size()*2 && std::memcmp(gfx::indices.data(),ref.data(),ref.size()*2)==0);
    // Equal stride is not proof of equal VAT; both format directions stay separate.
    reset();draw(GX_TRIANGLES,3);draw(GX_TRIANGLES,3,GX_VTXFMT1);draw(GX_TRIANGLES,3);
    assert(commands.size()==3);
    for(auto prim:{GX_LINES,GX_LINESTRIP,GX_POINTS}) {
        reset();draw(prim,prim==GX_POINTS?1:2);draw(GX_TRIANGLES,3);
        assert(commands.size()==2 && !commands[0].mergeable && commands[1].mergeable);
        reset();draw(GX_TRIANGLES,3);draw(prim,prim==GX_POINTS?1:2);
        assert(commands.size()==2);
    }
    reset();draw(GX_TRIANGLES,65535);draw(GX_TRIANGLES,3);
    assert(commands.size()==2 && commands[0].vtxCount==65535);
    reset();draw(GX_TRIANGLESTRIP,65535);
    assert(commands[0].indexCount==196599 && gfx::indices.size()==393198);
    reset();draw(GX_TRIANGLES,4);draw(GX_TRIANGLES,4);
    assert(commands.size()==1 && commands[0].vtxCount==6 && gfx::verts.size()==24);
    reset();draw(GX_QUADS,3);assert(commands[0].vtxCount==3 && commands[0].indexCount==3);
    reset();for(auto prim:{GX_TRIANGLES,GX_QUADS,GX_TRIANGLESTRIP,GX_TRIANGLEFAN,GX_LINES,GX_LINESTRIP,GX_POINTS}) {
        draw(prim,0);
        if(prim!=GX_POINTS)draw(prim,1);
    }
    assert(commands.empty() && !gfx::uploads);
    reset();draw(GX_TRIANGLES,3);g_gxState.stateDirty=true;draw(GX_TRIANGLES,3);
    assert(commands.size()==2);
}
static void put32(u8* p,u32 n,bool be) { if(be){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}else std::memcpy(p,&n,4); }
static void parsers() {
    for(bool be:{false,true}) {
        // Reject incomplete headers before touching data, including offsets
        // for which addition would wrap. Null data makes that contract strict.
        for (u32 badPos : {0u, 1u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu}) {
            const u32 limit = badPos < 2 ? badPos + 1 : 0xffffffffu;
            if (limit - badPos >= 2) continue;
            reset(); u32 cursor = badPos; bool rejected = false;
            try { handle_draw(GX_TRIANGLES, nullptr, cursor, limit, be); }
            catch (...) { rejected = true; }
            assert(rejected && cursor == badPos && !gfx::uploads);
        }
        reset(); u32 cursor = 2; bool rejectedHeader = false;
        try { handle_draw(GX_TRIANGLES, nullptr, cursor, 1, be); }
        catch (...) { rejectedHeader = true; }
        assert(rejectedHeader && cursor == 2 && !gfx::uploads);
        u8 ordinary[14] = {}; ordinary[be ? 1 : 0] = 3; cursor = 0;
        handle_draw(GX_TRIANGLES, ordinary, cursor, sizeof(ordinary), be);
        assert(cursor == sizeof(ordinary) && commands.size() == 1);
        u8 bytes[64]={GX_TRIANGLES,0,0};u32 pos;
        for(u32 count:{0x80000000u,0x7fffffffu,0xfffffffbu,0xffffffffu}) {
            reset();put32(bytes+3,count,be);pos=0;bool rejected=false;
            try {parse_indexed(bytes,pos,7,be);}catch(...) {rejected=true;}
            assert(rejected && !gfx::uploads);
        }
        reset();pos=0;bytes[1]=be?0:3;bytes[2]=be?3:0;put32(bytes+3,3,be);
        const u16 indices[]={0,1,2};std::memcpy(bytes+7,indices,6);
        parse_indexed(bytes,pos,25,be);assert(pos==25 && commands.size()==1 && commands[0].indexCount==3);
        reset();pos=0;bool rejected=false;try {parse_indexed(bytes,pos,24,be);}catch(...) {rejected=true;}
        assert(rejected && !gfx::uploads);
        reset();pos=0;put32(bytes+3,0,be);parse_indexed(bytes,pos,19,be);
        assert(pos==19 && commands.empty() && !gfx::uploads);
        reset();pos=0;bytes[1]=bytes[2]=0;put32(bytes+3,3,be);parse_indexed(bytes,pos,13,be);
        assert(pos==13 && commands.empty() && !gfx::uploads);
        reset();pos=0;bytes[0]=GX_TRIANGLES;put32(bytes+1,0xffffffffu,be);rejected=false;
        try {parse_sized(bytes,pos,5,be);}catch(...) {rejected=true;}
        assert(rejected && !gfx::uploads);
        reset();pos=0;put32(bytes+1,12,be);parse_sized(bytes,pos,17,be);
        assert(pos==17 && commands.size()==1 && commands[0].vtxCount==3);
    }
}
#ifdef MP6_INDEX_BENCHMARK
#include "baseline_indices.inc"
static volatile u32 benchmarkSink;
static void benchmark() {
    ByteBuffer buffers[2];
    for(auto prim:{GX_QUADS,GX_TRIANGLESTRIP,GX_TRIANGLEFAN})for(u16 count:{4,16,64,256,1024}) {
        prepare_idx_buffer(buffers[0],prim,0,count);
        prepare_idx_buffer_baseline(buffers[1],prim,0,count);
        for(unsigned trial=0;trial<12;++trial) {
            double ns[2];
            for(unsigned order=0;order<2;++order) {
                unsigned candidate=(order+trial)%2;
                auto begin=std::chrono::steady_clock::now();
                for(unsigned n=0;n<30000;++n) {
                    auto& buffer=buffers[candidate];buffer.clear();
                    benchmarkSink=candidate?prepare_idx_buffer(buffer,prim,0,count):
                                            prepare_idx_buffer_baseline(buffer,prim,0,count);
                }
                ns[candidate]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/30000;
            }
            std::printf("INDEX prim=%u count=%u trial=%u baseline_ns=%.3f candidate_ns=%.3f\n",
                        unsigned(prim),unsigned(count),trial,ns[0],ns[1]);
        }
    }
}
#endif
int main(int argc,char** argv) {
    bool checked = false, asserted = false;
    CHECK((checked = true), "macro selection");
    ASSERT((asserted = true), "macro selection");
    assert(asserted);
#ifdef MP6_RENDERER_RELEASE
    assert(!checked);
#else
    assert(checked);
#endif
    if(argc>1) {
        reset();
        if(std::strcmp(argv[1],"index-width")==0) {
            ByteBuffer b;assert(prepare_idx_buffer(b,GX_TRIANGLESTRIP,0,32768)==98298);
        } else if(std::strcmp(argv[1],"format")==0) {
            draw(GX_TRIANGLES,3);draw(GX_TRIANGLES,3,GX_VTXFMT1);assert(commands.size()==2);
        } else if(std::strcmp(argv[1],"line-boundary")==0) {
            draw(GX_LINES,2);draw(GX_TRIANGLES,3);assert(commands.size()==2);
        } else if(std::strcmp(argv[1],"vertex-limit")==0) {
            draw(GX_TRIANGLES,65535);draw(GX_TRIANGLES,3);assert(commands.size()==2);
        } else if(std::strcmp(argv[1],"empty")==0) {
            draw(GX_LINESTRIP,0);assert(commands.empty() && !gfx::uploads);
        } else if(std::strcmp(argv[1],"indexed")==0) {
            parsers();
        } else assert(false);
        return 0;
    }
    topology();batching();parsers();
    puts("Real index generation, draw callers and parser upload boundaries passed");
#ifdef MP6_INDEX_BENCHMARK
    benchmark();
#endif
}
