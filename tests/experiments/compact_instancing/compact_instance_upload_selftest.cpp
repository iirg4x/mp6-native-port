#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>
#include <cstdlib>
#include <cstdio>
#define CHECK(c, ...) do { if (!(c)) { std::fprintf(stderr,"snapshot check failed: %d\n",__LINE__); std::abort(); } } while (0)
struct ByteBuffer {
  std::vector<uint8_t> allocation;
  size_t used=0;
  size_t size() const { return used; }
  uint8_t* data() { return allocation.data(); }
  const uint8_t* data() const { return allocation.data(); }
  auto begin() { return allocation.begin(); }
  auto end() { return allocation.begin()+used; }
  void clear() { used=0; }
  void reserve_extra(size_t n) { if (allocation.size()<used+n) allocation.resize(used+n); }
  void append_uninitialized(size_t n) { reserve_extra(n); used+=n; }
  void push_back(uint8_t v) { reserve_extra(1); allocation[used++]=v; }
  void insert(std::vector<uint8_t>::iterator at,const uint8_t* start,const uint8_t* finish) {
    CHECK(at==end(),"append only"); const auto n=finish-start;
    reserve_extra(n); std::memcpy(data()+used,start,n); used+=n;
  }
};
namespace aurora::gfx {
struct Range { uint32_t offset=0,size=0; };
struct Frame { uint32_t frameIndex=0; ByteBuffer uniforms; } frame;
Frame& current_frame_packet() { return frame; }
bool recording=true;
bool check_recording(const char*) { return recording; }
Range push_uniform(const uint8_t* data,size_t length) {
  while (frame.uniforms.size()%256) frame.uniforms.push_back(0);
  Range r{uint32_t(frame.uniforms.size()),uint32_t(length)};
  frame.uniforms.insert(frame.uniforms.end(),data,data+length); return r;
}
}
#include "uniform_writer.hpp"
namespace aurora::gfx {
#include "compact_instance_upload.inc"
}
int main() {
  using namespace aurora::gfx;
  for (uint32_t round=0;round<1000;++round) {
    frame.frameIndex=round%7; // Include reused IDs, as after renderer reinitialization.
    frame.uniforms.clear();
    std::array<uint32_t,37> bytes{};
    for (auto& v:bytes) v=0x7f800000u ^ (round*104729u);
    auto write=[&] {
      auto writer=begin_instance_uniform(160);
      writer.append(bytes);
      return finish_instance_uniform(writer.finish());
    };
    const auto a=write();
    CHECK(a.offset==0 && a.size==148,"raw length");
    const auto old=bytes;
    bytes[2]^=0x80000000u;
    const auto b=write();
    CHECK(b.offset==256 && b.size==148,"alignment");
    std::fill(frame.uniforms.begin(),frame.uniforms.end(),0xa5);
    auto sa=instance_uniform_bytes(a), sb=instance_uniform_bytes(b);
    CHECK(sa.size()==148 && sb.size()==148,"snapshot size");
    CHECK(!std::memcmp(sa.data(),old.data(),148),"previous CPU data, not poisoned upload");
    CHECK(!std::memcmp(sb.data(),bytes.data(),148),"current CPU data, not poisoned upload");
    bytes[3]^=0xffffffffu;
    const auto c=write();
    CHECK(instance_uniform_bytes(a).empty(),"retired slot");
    CHECK(!std::memcmp(instance_uniform_bytes(c).data(),bytes.data(),148),"newest");
    CHECK(!std::memcmp(instance_uniform_bytes(b).data(),sb.data(),148),"second slot survives");
    CHECK(instance_uniform_bytes({b.offset,b.size-1}).empty(),"length mismatch");
    ++frame.frameIndex;
    CHECK(instance_uniform_bytes(b).empty(),"frame mismatch");
    recording=false; CHECK(!begin_instance_uniform(160),"outside frame"); recording=true;
  }
  // Force both slots to have the same range and frame after a renderer restart.
  frame={0,{}};
  auto first=begin_instance_uniform(4); first.append<uint32_t>(111);
  const auto a=finish_instance_uniform(first.finish());
  frame={0,{}};
  auto second=begin_instance_uniform(4); second.append<uint32_t>(222);
  const auto b=finish_instance_uniform(second.finish());
  CHECK(a.offset==b.offset && a.size==b.size,"reused addresses");
  uint32_t value=0; std::memcpy(&value,instance_uniform_bytes(b).data(),4);
  CHECK(value==222,"newest snapshot wins");
  std::puts("CPU snapshot oracle PASS: 1000 lifecycles, poisoned GPU uploads, renderer restart.");
}
