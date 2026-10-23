#include "../../src/audio/menu_sfx_bank.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
using namespace mp6::audio;
static MusicRead reader(const std::vector<uint8_t>& data) {
    return [&data](uint32_t at,void* out,size_t n){
        assert(uint64_t(at)+n<=data.size());std::memcpy(out,data.data()+at,n);return true;};
}
int main(int argc,char** argv){
    std::vector<uint8_t> data(1024);
    const auto put32=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)data[p+i]=v>>(24-i*8);};
    const auto put16=[&](size_t p,uint16_t v){data[p]=v>>8;data[p+1]=v;};
    put32(0,0x47534e44);put32(4,2);
    put32(32,96);put32(36,32);put32(48,128);put32(52,64);
    put32(56,192);put32(60,512);put32(64,704);put32(68,320);
    put32(96+8,512);put32(96+16,320);
    for(int i=0;i<4;++i){data[128+i*16+4]=127;data[128+i*16+5]=64;}
    put32(192,16);put32(196,128);put32(200,256);
    put32(208,16);put32(224,0xffffffff);put16(228,17);
    put32(232,0x59);put32(240,0x1a10);put32(248,0x01000107);put32(252,0xffff0000);put32(256,0x11);
    put32(320,0xffffffff);put16(326,1);put32(348,40);
    put16(360,1);put16(366,17);data[370]=127;data[371]=64;data[372]=60;
    put16(448,26);put32(460,32000);put32(464,14);put32(476,64);
    put16(480,0xffff);put16(512,8);data[704]=0;
    for(int i=1;i<8;++i)data[704+i]=0x11;
    for(unsigned i=0;i<4;++i){const auto cue=menu_cue(reader(data),uint32_t(data.size()),i);assert(cue.rate==32000&&cue.pcm.size()==14&&cue.pcm[0]==1);}
    assert(menu_cue(reader(data),uint32_t(data.size()),4).pcm.empty());
    for(size_t n=0;n<data.size();++n){auto truncated=data;truncated.resize(n);assert(menu_cue(reader(truncated),uint32_t(n),0).pcm.empty());}
    // Exercise every field with corrupt values. Any attempted out-of-file read
    // asserts in reader, rather than quietly masking an invalid parser span.
    for(size_t p=0;p<704;++p){auto corrupt=data;corrupt[p]=255;menu_cue(reader(corrupt),uint32_t(corrupt.size()),0);}
    puts("Menu SFX: synthetic decoder, truncation and corrupt spans passed");
    if(argc>1){std::ifstream file(argv[1],std::ios::binary);assert(file);
        data.assign(std::istreambuf_iterator<char>(file),{});
        for(unsigned i=0;i<4;++i){const auto cue=menu_cue(reader(data),uint32_t(data.size()),i);
            assert(cue.rate==32000&&!cue.pcm.empty());int peak=0;for(auto v:cue.pcm)peak=std::max(peak,std::abs(int(v)));
            assert(peak>100);printf("Retail UI cue %u: %zu samples, peak %d\n",i,cue.pcm.size(),peak);}
    }
}
