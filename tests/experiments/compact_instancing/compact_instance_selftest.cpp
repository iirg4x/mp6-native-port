#include "compact_instance.hpp"
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

#define CHECK(condition, ...) do { if (!(condition)) { std::fprintf(stderr, "check failed: %s:%d\n", __FILE__, __LINE__); std::abort(); } } while (0)
using ByteBuffer = std::vector<uint8_t>;
namespace wgpu { struct RenderPipeline {}; }
namespace aurora::gfx {
using PipelineRef = uint64_t;
struct Range { uint32_t offset=0, size=0; };
}
namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline=123;
  gfx::Range vertRange, idxRange, uniformRange;
  uint32_t vtxCount=4, indexCount=6, instanceCount=1;
  struct { uint64_t textureBindGroup=456; } bindGroups;
  uint32_t dstAlpha=~0u, vtxFmt=0;
  bool mergeable=true;
};
struct PipelineConfig { struct { bool meshInstancing=false; } shaderConfig; };
struct ShaderInfo { bool compactMatrices=true; uint8_t lineMode=0; std::bitset<10> usesTexMtx; uint32_t uniformSize=512; };
}
namespace aurora::gfx {
struct Frame { uint32_t frameIndex=0; ByteBuffer uniforms, storage, indices; } frame;
gx::DrawData* lastDraw=nullptr;
uint32_t g_mergedDrawCallCount=0;
bool pipelineReady=true;
Frame& current_frame_packet() { return frame; }
ByteBuffer& g_instanceIndexShadow = frame.indices;
static std::span<const uint8_t> instance_uniform_bytes(Range range) {
  return std::span<const uint8_t>(frame.uniforms).subspan(range.offset, range.size);
}
template<class T> T* get_last_draw_command() { return lastDraw; }
PipelineRef find_gx_pipeline(const gx::PipelineConfig& cfg, gx::ShaderInfo& info) {
  CHECK(cfg.shaderConfig.meshInstancing, "expected instanced"); info.uniformSize=512; return 789;
}
bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline&) { CHECK(ref==789, "pipeline"); return pipelineReady; }
Range push_storage(const uint8_t* p, size_t length) {
  while (frame.storage.size()%256) frame.storage.push_back(0);
  Range range{static_cast<uint32_t>(frame.storage.size()), static_cast<uint32_t>(length)};
  frame.storage.insert(frame.storage.end(), p, p+length); return range;
}
#include "compact_instance.inc"
}

int main() {
  using namespace aurora;
  using namespace gx::compact_instance;
  uint32_t serial=0, checks=0;
  for (uint32_t matrices=0; matrices<=10; ++matrices) {
    const uint32_t normal=192+48*matrices;
    std::vector<uint8_t> first(normal+48+192);
    for (size_t i=0; i<first.size(); ++i) first[i]=uint8_t(i*119+matrices*13);
    CHECK(compatible_uniforms(first, first, matrices), "equal");
    const auto record=pack_record(first,matrices);
    CHECK(!std::memcmp(record.data(), first.data(),4), "vertex");
    CHECK(!std::memcmp(record.data()+1,first.data()+32,48), "arrays");
    CHECK(!std::memcmp(record.data()+16,first.data()+144,48), "position");
    CHECK(!std::memcmp(record.data()+28,first.data()+normal,48), "normal");
    CHECK(!record[13] && !record[14] && !record[15], "padding");
    for (size_t byte=0; byte<first.size(); ++byte) {
      auto second=first; second[byte]^=0x80;
      bool perInstance=byte<4 || (byte>=24 && byte<80) || (byte>=144 && byte<192) ||
                       (byte>=normal && byte<normal+48);
      CHECK(compatible_uniforms(first,second,matrices)==perInstance, "byte guard"); ++checks;
    }
    CHECK(!compatible_uniforms(first,std::span(first).first(first.size()-1),matrices), "size guard");
    CHECK(!compatible_uniforms(std::span(first).first(normal+47),std::span(first).first(normal+47),matrices), "short guard");
  }
  CHECK(!compatible_uniforms({}, {}, 0), "empty");
  CHECK(!compatible_uniforms({}, {}, 11), "too many matrices");

  gx::PipelineConfig config;
  gx::ShaderInfo info;
  gx::DrawData first, next;
  const auto reset=[&] {
    gfx::frame={++serial, ByteBuffer(1024), ByteBuffer{}, ByteBuffer(24)};
    first={}; next={};
    first.uniformRange={0,512}; next.uniformRange={512,512};
    first.idxRange={0,12}; next.idxRange={12,12};
    for (size_t i=0; i<512; ++i) gfx::frame.uniforms[i]=gfx::frame.uniforms[i+512]=uint8_t(i*79);
    gfx::lastDraw=&first; gfx::pipelineReady=true;
    gfx::g_mergedDrawCallCount=0; info={};
  };
  reset(); gfx::lastDraw=nullptr; CHECK(!gfx::try_compact_instance(next,config,info), "barrier");
  reset(); info.lineMode=1; CHECK(!gfx::try_compact_instance(next,config,info), "line");
  reset(); info.compactMatrices=false; CHECK(!gfx::try_compact_instance(next,config,info), "palette");
  reset(); next.instanceCount=2; CHECK(!gfx::try_compact_instance(next,config,info), "already instanced");
  reset(); next.mergeable=false; CHECK(!gfx::try_compact_instance(next,config,info), "not triangle");
  reset(); first.mergeable=false; CHECK(!gfx::try_compact_instance(next,config,info), "previous special");
  reset(); next.pipeline++; CHECK(!gfx::try_compact_instance(next,config,info), "pipeline barrier");
  reset(); next.vtxFmt++; CHECK(!gfx::try_compact_instance(next,config,info), "format barrier");
  reset(); next.dstAlpha=2; CHECK(!gfx::try_compact_instance(next,config,info), "alpha barrier");
  reset(); next.bindGroups.textureBindGroup++; CHECK(!gfx::try_compact_instance(next,config,info), "texture barrier");
  reset(); next.vtxCount++; CHECK(!gfx::try_compact_instance(next,config,info), "vertex count");
  reset(); next.indexCount++; CHECK(!gfx::try_compact_instance(next,config,info), "index count");
  reset(); next.idxRange.size--; CHECK(!gfx::try_compact_instance(next,config,info), "index size");
  reset(); gfx::frame.indices[12]=1; CHECK(!gfx::try_compact_instance(next,config,info), "index topology");
  reset(); gfx::frame.uniforms[512+80]^=1; CHECK(!gfx::try_compact_instance(next,config,info), "projection");
  reset(); gfx::frame.uniforms[512+240]^=1; CHECK(!gfx::try_compact_instance(next,config,info), "fragment/lighting");
  reset(); gfx::pipelineReady=false;
  const auto untouched=gfx::frame.uniforms;
  CHECK(!gfx::try_compact_instance(next,config,info), "pending shader fallback");
  CHECK(gfx::frame.storage.empty() && gfx::frame.uniforms==untouched && first.mergeable && first.instanceCount==1, "fallback unchanged");

  for (uint32_t test=0; test<100; ++test) {
    reset();
    // Three prefix bytes force a nonzero aligned record base.
    gfx::frame.storage.resize(3,0xa5);
    auto original=std::span<const uint8_t>(gfx::frame.uniforms).first(512);
    auto firstRecord=pack_record(original,0);
    for (uint32_t instance=1; instance<MaxInstances; ++instance) {
      for (size_t i=0; i<512; ++i) gfx::frame.uniforms[512+i]=uint8_t(i*79);
      gfx::frame.uniforms[512]=uint8_t(instance);
      gfx::frame.uniforms[512+32]=uint8_t(instance+1);
      gfx::frame.uniforms[512+144]=uint8_t(instance+2);
      gfx::frame.uniforms[512+192]=uint8_t(instance+3);
      const auto expected=pack_record(std::span<const uint8_t>(gfx::frame.uniforms).subspan(512,512),0);
      CHECK(gfx::try_compact_instance(next,config,info), "merge");
      CHECK(first.instanceCount==instance+1 && first.pipeline==789 && !first.mergeable, "draw state");
      CHECK(gfx::frame.storage.size()==256+RecordBytes*MaxInstances, "single reservation");
      CHECK(!std::memcmp(gfx::frame.storage.data()+256,firstRecord.data(),RecordBytes), "original instance");
      CHECK(!std::memcmp(gfx::frame.storage.data()+256+instance*RecordBytes,expected.data(),RecordBytes), "new instance");
      ++checks;
    }
    CHECK(!gfx::try_compact_instance(next,config,info), "batch capacity");
    CHECK(gfx::g_mergedDrawCallCount==MaxInstances-1, "counter");
    ++gfx::frame.frameIndex;
    CHECK(!gfx::try_compact_instance(next,config,info), "frame reuse barrier");
  }
  reset(); first.indexCount=next.indexCount=0; first.idxRange=next.idxRange={};
  CHECK(gfx::try_compact_instance(next,config,info), "nonindexed triangles");
  std::printf("Compact instance oracle PASS: %u byte/record cases, guards and fallback.\n", checks);
}
