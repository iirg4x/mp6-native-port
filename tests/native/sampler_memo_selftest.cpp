#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
using u32=uint32_t;
using WGPUSampler=uint64_t;
constexpr unsigned MaxTextures=8;
namespace wgpu {
struct Sampler {
  uint64_t id=0;
  explicit operator bool() const { return id!=0; }
  uint64_t Get() const { return id; }
};
}
struct { unsigned textureAnisotropy=1; } g_graphicsConfig;
namespace gfx {
struct Texture { bool isReplacement=false,generatedMips=false,hasArbitraryMips=false; };
struct TextureBind {
  struct { u32 mode0=0,mode1=0; } texObj;
  Texture* ref;
  unsigned get_descriptor() const { return 0; }
};
unsigned calls=0;
wgpu::Sampler sampler_ref(unsigned) { return {++calls}; }
}
#include "sampler_subject.inc"
int main() {
  gfx::Texture a,b;
  gfx::TextureBind tex{{0,0},&a};
  for(unsigned slot=0;slot<8;++slot) {
    auto id=texture_sampler(slot,tex);
    auto count=gfx::calls;
    for(unsigned repeat=0;repeat<1000;++repeat) assert(texture_sampler(slot,tex)==id);
    assert(gfx::calls==count);
    tex.ref=&b;
    assert(texture_sampler(slot,tex)==id); // identity is not sampler state
    tex.ref=&a;
    for(unsigned bit=0;bit<32;++bit) {
      tex.texObj.mode0^=1u<<bit;
      assert(texture_sampler(slot,tex)!=id); id=texture_sampler(slot,tex);
      tex.texObj.mode1^=1u<<bit;
      assert(texture_sampler(slot,tex)!=id); id=texture_sampler(slot,tex);
    }
    for(unsigned aniso=0;aniso<256;++aniso) {
      g_graphicsConfig.textureAnisotropy=aniso;
      auto previous=gfx::calls;
      auto next=texture_sampler(slot,tex);
      assert(next!=id || previous==gfx::calls);
      id=next;
    }
    for(unsigned flags=0;flags<8;++flags) {
      a={bool(flags&1),bool(flags&2),bool(flags&4)};
      auto previous=gfx::calls;
      id=texture_sampler(slot,tex);
      assert(gfx::calls==previous+(flags!=0));
      assert(texture_sampler(slot,tex)==id);
    }
    sSamplerMemo={};
    assert(texture_sampler(slot,tex)!=id); // device reset never reuses a handle
    a={}; tex.texObj={}; g_graphicsConfig.textureAnisotropy=1;
  }
  puts("sampler memo state/identity/reset checks passed");
}
