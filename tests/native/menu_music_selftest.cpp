#include "../../src/audio/pdt_music.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>

using namespace mp6::audio;
static void be16(std::vector<uint8_t>& b,size_t at,uint16_t v){b[at]=v>>8;b[at+1]=v;}
static void be32(std::vector<uint8_t>& b,size_t at,uint32_t v){be16(b,at,v>>16);be16(b,at+2,v);}
static MusicRead reader(const std::vector<uint8_t>& b){return [&b](uint32_t at,void* dst,size_t n){
    if(uint64_t(at)+n>b.size())return false;std::memcpy(dst,b.data()+at,n);return true;};}
static std::vector<uint8_t> fixture(){
    // Three packs (looped stereo, looped mono, non-looped jingle) and one removed ID.
    constexpr uint32_t data=224,bytes=260*8;
    std::vector<uint8_t> b(data+bytes*2);
    be16(b,0,1);be16(b,2,4);be32(b,8,32000);be32(b,16,32);be32(b,20,64);be32(b,24,128);be32(b,28,data);
    for(unsigned i=0;i<3;++i){be32(b,32+i*4,128+i*32);auto p=128+i*32;
        b[p]=i==0?3:i==1?2:0;b[p+1]=127;be16(b,p+6,i==1?0:32000);
        be32(b,p+8,bytes*2);be32(b,p+12,64);be32(b,p+16,data);be32(b,p+24,data+bytes);be16(b,p+28,1);}
    be16(b,64,1536);be16(b,66,uint16_t(-256));be16(b,96,1024);be16(b,98,512);
    for(unsigned ch=0;ch<2;++ch)for(unsigned f=0;f<260;++f){auto at=data+ch*bytes+f*8;b[at]=3;
        for(unsigned n=1;n<8;++n)b[at+n]=uint8_t(f+n*7+ch*61);}
    be32(b,36,0);be32(b,44,160); // Give the mono loop the approved idle ID 3; remove ID 1.
    return b;
}
static void synthetic(){
    auto b=fixture();auto read=reader(b);const auto tracks=music_catalog(read,uint32_t(b.size()));
    assert(tracks.size()==2 && tracks[0].channels==2 && tracks[1].channels==1 && tracks[1].rate==32000);
    assert(tracks[0].coefficients[0].coef[0][1]==-256);
    const std::set<uint32_t> idleIds{2,3,4,6,7,92};
    for(uint32_t id=0;id<4096;++id)assert(idle_music_track(id)==(idleIds.count(id)!=0));
    assert(!idle_music_track(UINT32_MAX));
    const auto idle=menu_music_catalog(read,uint32_t(b.size()));
    assert(idle.size()==1&&idle[0].id==3&&idle[0].channels==1); // ID 2 is non-looping in this fixture.
    auto noIdle=b;be32(noIdle,44,0); // Only an unapproved loop and an approved one-shot remain.
    assert(!music_catalog(reader(noIdle),uint32_t(noIdle.size())).empty());
    assert(menu_music_catalog(reader(noIdle),uint32_t(noIdle.size())).empty());
    assert(menu_music_catalog(read,16).empty());
    std::array<int16_t,MusicDecoder::blockSamples*2> block;
    MusicDecoder decoder;
    for(const auto& track:tracks){
        decoder.start(track);std::vector<int16_t> interleaved;
        for(int n;(n=decoder.pull(read,block.data()))>0;)interleaved.insert(interleaved.end(),block.begin(),block.begin()+n*2);
        assert(decoder.position()==260*14 && decoder.length()==260*14 && decoder.pull(read,block.data())==0);
        for(unsigned ch=0;ch<2;++ch){const unsigned source=ch<track.channels?ch:0;MP6AdpcmState history{};
            std::vector<int16_t> whole(260*14);mp6_dspadpcm_decode(b.data()+track.sampleOffset[source],260,&track.coefficients[source],&history,whole.data());
            for(size_t i=0;i<whole.size();++i)assert(interleaved[i*2+ch]==whole[i]);}
        decoder.start(track);assert(decoder.pull(read,block.data())==int(MusicDecoder::blockSamples));
        assert(std::equal(block.begin(),block.end(),interleaved.begin())); // Restart clears predictor history.
    }
    decoder.start(tracks[0]);assert(decoder.pull([](uint32_t,void*,size_t){return false;},block.data())==-1);
    assert(decoder.position()==0);
    assert(music_catalog(read,16).empty());assert(music_catalog(read,uint32_t(b.size()-1)).empty());
    for(auto [at,value]:std::vector<std::pair<size_t,uint32_t>>{{16,0},{20,33},{24,65},{28,UINT32_MAX},
            {32,127},{32,223},{136,UINT32_MAX},{140,UINT32_MAX},{144,UINT32_MAX},{152,UINT32_MAX}}){
        auto bad=b;be32(bad,at,value);assert(music_catalog(reader(bad),uint32_t(bad.size())).empty());}
    for(auto [at,value]:std::vector<std::pair<size_t,uint8_t>>{{0,1},{128,0x82},{129,128},{148,0xff},{156,0xff}}){
        auto bad=b;bad[at]=value;assert(music_catalog(reader(bad),uint32_t(bad.size())).empty());}
    assert(music_catalog([](uint32_t,void*,size_t){return false;},uint32_t(b.size())).empty());
    MusicShuffle shuffle(42);shuffle.reset(97);size_t last=97;
    for(int bag=0;bag<10;++bag){std::set<size_t> seen;for(int i=0;i<97;++i){auto n=shuffle.next();assert(n<97&&n!=last);seen.insert(n);last=n;}assert(seen.size()==97);}
    shuffle.reset(1);assert(shuffle.next()==0&&shuffle.next()==0);shuffle.reset(0);assert(shuffle.next()==0);
    puts("PDT validation, mono/stereo chunk history, restart, read failure, and ten shuffle bags passed");
}
static void retail(const char* path){
    FILE* file=std::fopen(path,"rb");assert(file);std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);assert(size>0&&size<=UINT32_MAX);
    MusicRead read=[file](uint32_t at,void* dst,size_t n){return std::fseek(file,at,SEEK_SET)==0&&std::fread(dst,1,n,file)==n;};
    const auto tracks=music_catalog(read,uint32_t(size));assert(tracks.size()==97);
    const auto idle=menu_music_catalog(read,uint32_t(size));
    const std::set<uint32_t> expected{2,3,4,6,7,92};std::set<uint32_t> actual;
    for(const auto& track:idle)actual.insert(track.id);
    if(actual!=expected){std::fprintf(stderr,"Validated idle IDs:");for(auto id:actual)std::fprintf(stderr," %u",id);std::fprintf(stderr,"\n");}
    assert(actual==expected&&idle.size()==expected.size());
    MusicShuffle shuffle(42);shuffle.reset(idle.size());uint32_t last=UINT32_MAX;
    for(int bag=0;bag<10;++bag){std::set<uint32_t> seen;
        for(size_t i=0;i<idle.size();++i){const auto id=idle[shuffle.next()].id;
            assert(expected.count(id)&&id!=last);seen.insert(id);last=id;}
        assert(seen==expected);}
    puts("Menu playlist: 6 idle tracks only, ten complete shuffle bags without immediate repeats");
    std::array<int16_t,MusicDecoder::blockSamples*2> block;MusicDecoder decoder;
    uint64_t samples=0;uint32_t audible=0;
    for(const auto& track:tracks){decoder.start(track);bool nonzero=false;
        // Decode every track end-to-end, without materializing a full-song PCM buffer.
        int n;while((n=decoder.pull(read,block.data()))>0){samples+=n;for(int i=0;i<n*2;++i)nonzero|=block[i]!=0;}
        assert(n==0&&decoder.position()==decoder.length()&&nonzero);audible+=nonzero;}
    std::fclose(file);printf("Retail bank: %u audible tracks, %llu PCM frames decoded to completion\n",audible,(unsigned long long)samples);
}
int main(int argc,char** argv){synthetic();if(argc>1)retail(argv[1]);}
