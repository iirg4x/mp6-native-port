#include "ready_memo.hpp"
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstdlib>
#define ZoneScoped
static unsigned checks;
static void require(bool value){++checks;if(!value){std::fprintf(stderr,"FAIL %u\n",checks);std::abort();}}
namespace before {
#include "cache_fixture.inc"
#include "lookup_original.inc"
}
namespace after {
#include "cache_fixture.inc"
#include "lookup_candidate.inc"
}
static void compare(uint64_t hash,unsigned frame,unsigned alignment=256){
  before::frame=after::frame=frame;before::alignment=after::alignment=alignment;
  before::gx::ShaderInfo a;
  after::gx::ShaderInfo b;
  auto ar=before::find_pipeline_impl(before::ShaderType::GX,before::gx::PipelineConfig{hash},before::create_pipeline,
                                    before::PipelinePriority::Blocking,std::nullopt,&a);
  auto br=after::find_pipeline_impl(after::ShaderType::GX,after::gx::PipelineConfig{hash},after::create_pipeline,
                                   after::PipelinePriority::Blocking,std::nullopt,&b);
  require(ar==br && a.hash==b.hash && a.size==b.size);
  require(before::creates==after::creates);
  require(before::writes.size()==after::writes.size());
  for(unsigned i=0;i<before::writes.size();++i){
    require(before::writes[i].hash==after::writes[i].hash && before::writes[i].frame==after::writes[i].frame);
  }
  require(before::g_pipelines.size()==after::g_pipelines.size());
  for(auto& [key,value]:before::g_pipelines){
    auto it=after::g_pipelines.find(key);require(it!=after::g_pipelines.end());
    require(value.firstFrameUsed==it->second.firstFrameUsed);
  }
}
int main(){
  before::reset();after::reset();
  for(unsigned i=0;i<10000;++i) compare(i%32,100+i);
  require(after::g_pipelineMutex.locks<before::g_pipelineMutex.locks/10);
  unsigned baselineLocks=before::g_pipelineMutex.locks,candidateLocks=after::g_pipelineMutex.locks;
  for(unsigned i=0;i<32;++i)compare(i,1); // Earlier first-use must persist.
  for(unsigned i=0;i<100;++i)compare(i%32,20000+i,16);
  for(unsigned i=0;i<100;++i)compare(i%32,21000+i,256);
  for(unsigned i=0;i<100;++i)compare(i&1?0:64,22000+i); // Direct-slot collision.
  before::reset();after::reset();
  before::g_hasPipelineThread=after::g_hasPipelineThread=true;
  compare(7,100);compare(7,101); // Wait for compilation, then ready hit.
  before::reset();after::reset();
  before::g_hasPipelineThread=after::g_hasPipelineThread=true;
  before::shutdownWake=after::shutdownWake=true;
  compare(7,100); // Woken by shutdown, no compiled pipeline.
  after::gx::ShaderInfo absent;
  require(!after::g_readyGxMemo.get(7,256,101,absent));
  before::reset();after::reset();compare(7,101); // New device/session must compile.
  require(after::creates==1);
  std::printf("PASS: %u checks using exported actual lookup functions; identical layout/persistence/compile results; synthetic ready-repeat locks %u -> %u (not game timing)\n",checks,baselineLocks,candidateLocks);
}
