#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string_view>

#define LIKELY [[likely]]
static uint16_t bswap(uint16_t x) { return __builtin_bswap16(x); }
static uint32_t bswap(uint32_t x) { return __builtin_bswap32(x); }
static uint64_t bswap(uint64_t x) { return __builtin_bswap64(x); }
static float bswap(float x) {
  uint32_t u; std::memcpy(&u, &x, 4); u=bswap(u); std::memcpy(&x, &u, 4); return x;
}
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL: %s at %d\n", #x, __LINE__); std::exit(1); } } while (0)

// Generated from the actual source-profile header, not rewritten scalar paths.
namespace baseline {
#include "fifo_original.inc"
#include "macros_original.inc"
}
namespace candidate {
#include "fifo_candidate.inc"
#include "macros_candidate.inc"
}

#define DEFINE_STORAGE(ns) \
namespace ns::aurora::gx::fifo { \
namespace detail { \
  uint8_t* sBufferData=nullptr; uint32_t sBufferSize=0, sBufferCapacity=0; \
  bool sInDisplayList=false; uint8_t* sDlBuffer=nullptr; \
  uint32_t sDlSize=0, sDlWritePos=0; \
} \
unsigned growths=0; \
void write_data_grow(const void* data, uint32_t length) { \
  ++growths; \
  const auto capacity=std::max(detail::sBufferCapacity*2, detail::sBufferSize+length); \
  auto* resized=static_cast<uint8_t*>(std::realloc(detail::sBufferData,capacity)); \
  REQUIRE(resized); detail::sBufferData=resized; detail::sBufferCapacity=capacity; \
  std::memcpy(resized+detail::sBufferSize,data,length); detail::sBufferSize+=length; \
} \
}
DEFINE_STORAGE(baseline)
DEFINE_STORAGE(candidate)
namespace oldfifo=baseline::aurora::gx::fifo;
namespace newfifo=candidate::aurora::gx::fifo;

struct Packet { uint32_t addr, value; uint8_t opcode, kind; };
static void old_write(Packet p) {
  switch (p.kind) {
  case 0: baseline::emit_bp(p.value); break;
  case 1:
    if(p.opcode==0x08) baseline::emit_cp(p.addr,p.value);
    else { oldfifo::write_u8(p.opcode); oldfifo::write_u8(uint8_t(p.addr)); oldfifo::write_u32(p.value); }
    break;
  case 2: baseline::emit_xf(p.addr,p.value); break;
  default: REQUIRE(false);
  }
}
static void new_write(Packet p) {
  switch (p.kind) {
  case 0: candidate::emit_bp(p.value); break;
  case 1:
    if(p.opcode==0x08) candidate::emit_cp(p.addr,p.value);
    else newfifo::write_cp_packet(p.opcode,uint8_t(p.addr),p.value);
    break;
  case 2: candidate::emit_xf(p.addr,p.value); break;
  default: REQUIRE(false);
  }
}
static uint32_t rng=0x8241b7cd;
static uint32_t next() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void stream_setup(uint32_t size, uint32_t capacity) {
  REQUIRE(size<=capacity);
  std::free(oldfifo::detail::sBufferData); std::free(newfifo::detail::sBufferData);
  oldfifo::detail::sBufferData=static_cast<uint8_t*>(std::malloc(std::max(1u,capacity)));
  newfifo::detail::sBufferData=static_cast<uint8_t*>(std::malloc(std::max(1u,capacity)));
  REQUIRE(oldfifo::detail::sBufferData && newfifo::detail::sBufferData);
  std::memset(oldfifo::detail::sBufferData,0xa7,capacity);
  std::memset(newfifo::detail::sBufferData,0xa7,capacity);
  oldfifo::detail::sBufferSize=newfifo::detail::sBufferSize=size;
  oldfifo::detail::sBufferCapacity=newfifo::detail::sBufferCapacity=capacity;
  oldfifo::detail::sInDisplayList=newfifo::detail::sInDisplayList=false;
  oldfifo::growths=newfifo::growths=0;
}
static void stream_equal() {
  REQUIRE(oldfifo::detail::sBufferSize==newfifo::detail::sBufferSize);
  REQUIRE(oldfifo::detail::sBufferCapacity==newfifo::detail::sBufferCapacity);
  REQUIRE(oldfifo::growths==newfifo::growths);
  REQUIRE(std::memcmp(oldfifo::detail::sBufferData,newfifo::detail::sBufferData,oldfifo::detail::sBufferSize)==0);
}
static void verify() {
  uint64_t cases=0;
  for (uint32_t alignment=0;alignment<32;++alignment) {
    for (uint32_t capacity=0;capacity<40;++capacity) {
      for (uint32_t position=0;position<=capacity;++position) {
        for (uint8_t kind=0;kind<3;++kind) {
          // Independent DL storage includes canaries on both sides; no writes
          // outside the supplied length, even after individual fields overflow.
          std::array<uint8_t,160> a,b; a.fill(0xc9); b=a;
          oldfifo::detail::sInDisplayList=newfifo::detail::sInDisplayList=true;
          oldfifo::detail::sDlBuffer=a.data()+32+alignment;
          newfifo::detail::sDlBuffer=b.data()+32+alignment;
          oldfifo::detail::sDlSize=newfifo::detail::sDlSize=capacity;
          oldfifo::detail::sDlWritePos=newfifo::detail::sDlWritePos=position;
          for (unsigned repeat=0;repeat<4;++repeat) {
            const Packet p{next(),next(),uint8_t(next()),kind};
            old_write(p); new_write(p); ++cases;
            REQUIRE(oldfifo::detail::sDlWritePos==newfifo::detail::sDlWritePos);
            REQUIRE(a==b);
          }
        }
      }
    }
    for (uint32_t spare=0;spare<24;++spare) {
      for (uint8_t kind=0;kind<3;++kind) {
        stream_setup(alignment,alignment+spare);
        for(unsigned repeat=0;repeat<8;++repeat) {
          const Packet p{next(),next(),uint8_t(next()),kind};
          old_write(p); new_write(p); stream_equal(); ++cases;
        }
      }
    }
  }
  stream_setup(0,1);
  for (unsigned i=0;i<100000;++i) {
    const Packet p{next(),next(),uint8_t(next()),uint8_t(next()%3)};
    old_write(p); new_write(p); ++cases;
    if ((i%1024)==0) stream_equal();
  }
  stream_equal();
  // Exact known endian encoding, independent of the scalar reference.
  stream_setup(0,64);
  new_write({0,0x12345678,0,0});
  new_write({0x9a,0xabcdef01,0x08,1});
  new_write({0x23,0x76543210,0,2});
  constexpr uint8_t expected[]={0x61,0x12,0x34,0x56,0x78,0x08,0x9a,0xab,0xcd,0xef,0x01,
      0x10,0,0,0x10,0x23,0x76,0x54,0x32,0x10};
  REQUIRE(newfifo::detail::sBufferSize==sizeof(expected));
  REQUIRE(std::memcmp(newfifo::detail::sBufferData,expected,sizeof(expected))==0);
  std::printf("PASS: %llu packet writes; alignment, partial display-list overflow, growth, mixed stream and endian bytes\n",
      static_cast<unsigned long long>(cases));
}
static void benchmark() {
  std::vector<Packet> packets;
  for (unsigned i=0;i<6874+1484+3725;++i) {
    const uint8_t kind=i<6874?0:i<6874+1484?1:2;
    packets.push_back({next()&255,next(),0x08,kind});
  }
  // Interleave once outside measurement. Counts approximate the measured board
  // census, not a recording of its call order or a whole-game FPS prediction.
  for (size_t i=packets.size()-1;i>0;--i) std::swap(packets[i],packets[next()%(i+1)]);
  stream_setup(0,131072);
  for(auto p:packets) { old_write(p); new_write(p); }
  stream_equal();
  for(unsigned sample=0;sample<12;++sample) {
    for(unsigned step=0;step<2;++step) {
      const bool useNew=(step^(sample&1))!=0;
      const auto start=std::chrono::steady_clock::now();
      for(unsigned repeat=0;repeat<600;++repeat) {
        if(useNew) {
          newfifo::detail::sBufferSize=0;
          for(auto p:packets) new_write(p);
        } else {
          oldfifo::detail::sBufferSize=0;
          for(auto p:packets) old_write(p);
        }
        asm volatile("" ::: "memory"); // Every iteration's output must be materialized.
      }
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      stream_equal();
      std::printf("BENCH sample=%u variant=%s ns_per_packet=%.4f\n",sample,useNew?"candidate":"baseline",double(ns)/(600*packets.size()));
    }
  }
}
int main(int argc,char**argv) {
  verify();
  if(argc==2 && std::string_view(argv[1])=="--bench") benchmark();
  std::free(oldfifo::detail::sBufferData); std::free(newfifo::detail::sBufferData);
}
