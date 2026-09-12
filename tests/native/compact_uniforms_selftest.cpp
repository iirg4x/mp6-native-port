#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
using u32 = uint32_t;
constexpr int MaxPnMtx=10, MaxTexMtx=10, MaxPTTexMtx=20;
constexpr int GX_VA_PNMTXIDX=0,GX_VA_TEX0MTXIDX=1,GX_VA_TEX7MTXIDX=8;
constexpr int GX_NONE=0,GX_DIRECT=1,GX_TG_MTX2x4=1,GX_TG_MTX3x4=0;
constexpr int GX_IDENTITY=60,GX_PTTEXMTX0=64,GX_PTIDENTITY=125;
template<class T> using Mat3x4=std::array<T,12>;
using Matrix=Mat3x4<float>;
struct Attr { int attrType=GX_NONE; };
struct Tcg { int type=GX_TG_MTX2x4; u32 mtx=GX_IDENTITY,postMtx=GX_PTIDENTITY; };
struct Config { std::array<Attr,9> attrs; std::array<Tcg,8> tcgs; };
struct Info {
    bool compactMatrices=false;
    std::bitset<8> sampledTexCoords;
    std::bitset<20> usesTexMtx,usesPTTexMtx;
    size_t uniformSize=0;
};
struct Pn { Matrix pos,nrm; };
struct State { u32 currentPnMtx=0; std::array<Pn,10> pnMtx; std::array<Matrix,10> texMtxs; std::array<Matrix,20> ptTexMtxs; } g_gxState;
struct Buffer { std::vector<Matrix> matrices; void append(const Matrix& m) { matrices.push_back(m); } };
#include "matrix_subject.inc"
int main() {
    std::mt19937 rng(0x12345678);
    for(auto& pn:g_gxState.pnMtx) for(auto* m:{&pn.pos,&pn.nrm}) for(float& f:*m) f=float(int(rng()%65536)-32768)/128;
    for(auto& m:g_gxState.texMtxs) for(float& f:m) f=float(int(rng()%65536)-32768)/128;
    for(auto& m:g_gxState.ptTexMtxs) for(float& f:m) f=float(int(rng()%65536)-32768)/128;
    size_t saved=0;
    for(u32 pn=0;pn<10;++pn) for(int indexed=-1;indexed<9;++indexed) for(int trial=0;trial<2000;++trial) {
        Config config;
        Info info;
        if(indexed>=0) config.attrs[indexed].attrType=GX_DIRECT;
        g_gxState.currentPnMtx=pn;
        for(int i=0;i<8;++i) {
            info.sampledTexCoords.set(i,(rng()%4)!=0);
            auto& t=config.tcgs[i];
            t.type=rng()%3; // includes non-matrix SRTG
            t.mtx=trial<20 ? (trial*3) : (rng()%21)*3;
            t.postMtx=(rng()%3)==0 ? GX_PTIDENTITY : GX_PTTEXMTX0+(rng()%20)*3;
        }
        plan(config,info);
        assert(info.compactMatrices==(indexed<0));
        auto packed=upload(info).matrices;
        assert(packed.size()*sizeof(Matrix)==info.uniformSize);
        const auto normalBase=info.compactMatrices ? 1+info.usesTexMtx.count() : 20;
        assert(packed[info.compactMatrices ? 0:pn]==g_gxState.pnMtx[pn].pos);
        assert(packed[normalBase+(info.compactMatrices ? 0:pn)]==g_gxState.pnMtx[pn].nrm);
        if(!info.compactMatrices) {
            for(u32 j=0;j<20;++j) assert(packed[j]==(j<10 ? g_gxState.pnMtx[j].pos:g_gxState.texMtxs[j-10]));
            for(u32 j=0;j<10;++j) assert(packed[20+j]==g_gxState.pnMtx[j].nrm);
        }
        for(int i=0;i<8;++i) if(info.sampledTexCoords.test(i)) {
            const auto& t=config.tcgs[i];
            if(t.type<=1 && t.mtx!=GX_IDENTITY) {
                auto original=t.mtx/3;
                assert(packed[texture_slot(t,info)]==(original<10 ? g_gxState.pnMtx[original].pos:g_gxState.texMtxs[original-10]));
            }
            if(t.postMtx!=GX_PTIDENTITY) {
                auto base=normalBase+(info.compactMatrices ? 1:10);
                assert(packed[base+post_slot(t,info)]==g_gxState.ptTexMtxs[(t.postMtx-GX_PTTEXMTX0)/3]);
            }
        }
        size_t old=30+(info.usesPTTexMtx.any()?20:0);
        assert(packed.size()<=old);
        saved+=old-packed.size();
    }
    std::printf("PASS: 200000 palette configurations, all PN slots, static/dynamic texture indices; %zu matrix copies avoided\n",saved);
}
