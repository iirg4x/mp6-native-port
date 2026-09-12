#include <array>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <cstdio>
#include <aurora/dl.hpp>
struct Module {
    Module(const char*) {}
    template<class...T>void warn(const char*,T...) {}
    template<class...T>[[noreturn]]void fatal(const char*,T...) {throw std::runtime_error("fatal");}
};
namespace aurora::gx {
static Module Log("test");
#include "attrs.inc"
}
#include "reader.inc"
using namespace aurora::gx::dl;
static void be32(u8* p,u32 n) {p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
int main() {
    u8 bytes[128]={GX_AURORA,u8(GX_AURORA_DRAW_INDEXED>>8),u8(GX_AURORA_DRAW_INDEXED),GX_TRIANGLES};
    for(u32 count:{0x80000000u,0x7fffffffu,0xfffffffbu,0xffffffffu}) {
        be32(bytes+6,count);Reader r(bytes,10,u8(4));assert(!r.next() && r.failed());
    }
    bytes[4]=0;bytes[5]=3;be32(bytes+6,3);
    const u16 idx[]={0,1,2};std::memcpy(bytes+10,idx,6);
    for(u32 size=0;size<28;++size) {
        Reader r(bytes,size,u8(4));assert(!r.next());assert(!size || r.failed());
    }
    Reader valid(bytes,28,u8(4));auto c=valid.next();
    assert(c && c->size==28 && c->draw.index(2)==2 && c->draw.vertices==bytes+16 && !valid.next() && !valid.failed());
    // Valid formats, NOP removal, batch boundaries and prebuilt index retention.
    GXVtxDescList desc[]={{GX_VA_POS,GX_DIRECT},{GX_VA_NULL,GX_NONE}};
    GXVtxAttrFmtList fmt[]={{GX_VA_POS,GX_POS_XY,GX_F32,0},{GX_VA_NULL,GX_POS_XY,GX_F32,0}};
    VtxFmtLists formats{};formats.fill(fmt);
    std::vector<u8> input;
    for(auto prim:{GX_TRIANGLES,GX_QUADS}) {
        input.push_back(prim);input.push_back(0);input.push_back(prim==GX_TRIANGLES?3:4);
        input.insert(input.end(),(prim==GX_TRIANGLES?3:4)*8,0x32);input.push_back(GX_NOP);
    }
    auto output=optimize(input.data(),input.size(),desc,&formats);assert(output);
    Reader result(output->data(),output->size(),desc,&formats);auto batch=result.next();
    assert(batch && batch->kind==Command::Kind::DrawIndexed && batch->draw.vtxCount==7 && batch->draw.indexCount==9);
    for(u32 i=0;i<9;++i)assert(batch->draw.index(i)<7);
    assert(!result.next() && !result.failed());
    auto again=optimize(output->data(),output->size(),desc,&formats);assert(again && *again==*output);
    puts("Actual display-list reader/optimizer bounds and valid commands passed");
}
