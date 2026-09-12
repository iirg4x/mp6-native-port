// Deterministic stand-ins for the GPU compiler, persistence and worker wake.
// The lookup and layout-publication functions themselves are source exports.
using PipelineRef=uint64_t;
using HashType=uint64_t;
enum class ShaderType { GX, Other };
enum class PipelinePriority { Normal, Blocking, Background };
namespace wgpu { using RenderPipeline=unsigned; }
namespace gx {
struct ShaderInfo { uint64_t hash{}; uint32_t size{}; bool operator==(const ShaderInfo&) const=default; };
struct PipelineConfig { uint64_t shaderConfig{}; };
}
struct CachedPipeline {
  wgpu::RenderPipeline pipeline{};
  uint32_t firstFrameUsed=UINT32_MAX;
  std::optional<gx::ShaderInfo> shaderInfo;
  uint32_t shaderInfoAlignment{};
};
struct PendingPipeline { PipelineRef hash{}; uint32_t firstFrameUsed{}; std::function<unsigned()> create; };
struct PipelineCacheWrite { PipelineRef hash{}; uint32_t frame{}; bool operator==(const PipelineCacheWrite&) const=default; };
struct Mutex { unsigned locks{}; void lock(){++locks;} void unlock(){} };
static Mutex g_pipelineMutex;
struct Cv {
  template<class Lock,class Pred> void wait(Lock&,Pred pred);
  void notify_one(){}
};
static Cv g_pipelineReadyCv,g_pipelineQueueCv;
static std::unordered_map<PipelineRef,CachedPipeline> g_pipelines;
static std::unordered_set<PipelineRef> g_pendingPipelines;
static std::deque<PendingPipeline> g_pipelineQueue,g_backgroundPipelineQueue;
static std::vector<PipelineCacheWrite> writes;
static bool g_pipelineThreadEnd{},g_hasPipelineThread{},shutdownWake{};
static unsigned g_pipelinesPerFrame{},queuedPipelines{},creates{},frame{},alignment=256;
static constexpr unsigned BuildPipelinesPerFrame=1000000;
static PipelineRef g_lastPipelineRef=UINT64_MAX;
static ReadyPipelineMemo<gx::ShaderInfo> g_readyGxMemo;
static uint32_t align_uniform(unsigned){return alignment;}
static uint32_t current_frame(){return frame;}
static PipelineRef xxh3_hash(const gx::PipelineConfig& config,HashType){return config.shaderConfig;}
namespace gx { static ShaderInfo build_shader_info(uint64_t hash){return {hash,alignment};} }
static unsigned create_pipeline(const gx::PipelineConfig&){return ++creates;}
static PipelineCacheWrite make_pipeline_cache_write(ShaderType,PipelineRef hash,const gx::PipelineConfig&,uint32_t first){return {hash,first};}
static void enqueue_pipeline_cache_write(PipelineCacheWrite value){writes.push_back(value);}
static void notify_pipeline_ready(bool queued){if(queued)--queuedPipelines;}
static PendingPipeline* touch_pending_pipeline(PipelineRef hash,PipelinePriority){
  for(auto& pending:g_pipelineQueue) if(pending.hash==hash)return &pending;
  for(auto& pending:g_backgroundPipelineQueue) if(pending.hash==hash)return &pending;
  return nullptr;
}
static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash){
  for(auto* queue:{&g_pipelineQueue,&g_backgroundPipelineQueue}) {
    for(auto it=queue->begin();it!=queue->end();++it) if(it->hash==hash){
      auto result=std::move(*it);queue->erase(it);g_pendingPipelines.erase(hash);return result;
    }
  }
  return std::nullopt;
}
template<class Lock,class Pred> void Cv::wait(Lock&,Pred pred){
  if(!pred()) {
    if(shutdownWake) g_pipelineThreadEnd=true;
    else {
      require(!g_pipelineQueue.empty());
      auto pending=take_pending_pipeline(g_pipelineQueue.front().hash);
      require(bool(pending));
      g_pipelines.emplace(pending->hash,CachedPipeline{pending->create(),pending->firstFrameUsed});
    }
  }
  require(pred());
}
static void reset(){
  g_pipelines.clear();g_pendingPipelines.clear();g_pipelineQueue.clear();g_backgroundPipelineQueue.clear();
  writes.clear();g_readyGxMemo.clear();g_lastPipelineRef=UINT64_MAX;
  g_pipelineThreadEnd=g_hasPipelineThread=shutdownWake=false;
  g_pipelinesPerFrame=queuedPipelines=creates=frame=0;alignment=256;g_pipelineMutex.locks=0;
}
