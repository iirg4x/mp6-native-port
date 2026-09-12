// The production non-Tracy implementation, with deterministic deferred queries.
#include <aurora/aurora.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
namespace wgpu {
enum class FeatureName { TimestampQuery };
enum class QueryType { Timestamp };
enum class BufferUsage { QueryResolve=1,CopySrc=2,MapRead=4,CopyDst=8 };
inline BufferUsage operator|(BufferUsage a,BufferUsage b) { return BufferUsage(int(a)|int(b)); }
enum class MapMode { Read };
enum class CallbackMode { AllowSpontaneous };
enum class MapAsyncStatus { Success,Error };
using StringView=std::string_view;
struct QuerySetDescriptor { const char* label;QueryType type;uint32_t count; };
struct BufferDescriptor { const char* label;BufferUsage usage;uint64_t size; };
struct QuerySet { std::shared_ptr<std::vector<uint64_t>> data; explicit operator bool() const{return bool(data);} };
std::vector<std::function<void(MapAsyncStatus,StringView)>> callbacks;
struct Buffer {
    std::shared_ptr<std::vector<uint64_t>> data;
    explicit operator bool() const{return bool(data);}
    const void* GetConstMappedRange(uint64_t,uint64_t) { return data->data(); }
    void Unmap() {}
    void Destroy() {}
    template<class F> void MapAsync(MapMode,int,uint64_t,CallbackMode,F callback) {callbacks.push_back(callback);}
};
struct Device {
    bool supported=true;
    int allocations=0;
    bool HasFeature(FeatureName)const{return supported;}
    QuerySet CreateQuerySet(const QuerySetDescriptor* d) {++allocations;return {std::make_shared<std::vector<uint64_t>>(d->count)};}
    Buffer CreateBuffer(const BufferDescriptor* d) {++allocations;return {std::make_shared<std::vector<uint64_t>>(d->size/8)};}
};
struct CommandEncoder {
    void ResolveQuerySet(QuerySet q,int,uint32_t count,Buffer b,int)const {std::copy_n(q.data->begin(),count,b.data->begin());}
    void CopyBufferToBuffer(Buffer a,int,Buffer b,int,uint64_t size)const {std::copy_n(a.data->begin(),size/8,b.data->begin());}
};
struct PassTimestampWrites {QuerySet querySet;uint32_t beginningOfPassWriteIndex,endOfPassWriteIndex;};
}
namespace aurora::webgpu {
wgpu::Device g_device;
namespace gpu_prof {
class Zone {public:Zone(const wgpu::CommandEncoder&,std::string_view);~Zone();};
}
}
#include "gpu_stats_subject.inc"
using namespace aurora::webgpu::gpu_prof;
static wgpu::CommandEncoder encoder;
static void pass(const char* name,uint64_t start,uint64_t end) {
    auto* writes=pass_writes(name);
    assert(writes);
    auto& q=*writes->querySet.data;
    q[writes->beginningOfPassWriteIndex]=start;
    q[writes->endOfPassWriteIndex]=end;
}
static void finish_callback(size_t i,bool success=true) {
    wgpu::callbacks.at(i)(success?wgpu::MapAsyncStatus::Success:wgpu::MapAsyncStatus::Error,{});
}
int main() {
    initialize();
    AuroraGpuStats out{};
    frame_begin(encoder);frame_end(encoder);after_submit();
    assert(!pass_writes("off") && aurora::webgpu::g_device.allocations==0);
    aurora_gpu_stats_set_enabled(true);
    for(int i=0;i<4;++i) {
        frame_begin(encoder);
        pass("GTAO",10'000'000,11'000'000);
        pass("Blur",13'000'000,15'000'000);
        pass("GTAO",15'000'000,17'000'000);
        frame_end(encoder);after_submit();
    }
    frame_begin(encoder);
    assert(!pass_writes("backpressure"));
    assert(wgpu::callbacks.size()==4); // no wait and no unbounded fifth buffer
    finish_callback(2); // out-of-order completion is safe
    frame_begin(encoder);
    aurora_gpu_stats_read(&out);
    assert(out.sampleCount==1 && out.status==2 && out.rowCount==2);
    assert(out.spanAverageMs==7 && out.betweenAverageMs==2);
    assert(std::strcmp(out.rows[0].name,"GTAO")==0 && out.rows[0].averageMs==3);
    assert(out.rows[1].averageMs==2 && out.droppedFrames==1);
    frame_end(encoder);
    aurora_gpu_stats_set_enabled(false);
    aurora_gpu_stats_set_enabled(true); // off/on before the next frame changes generation
    finish_callback(0);finish_callback(1);finish_callback(3);
    frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(!out.sampleCount); // never publish pre-toggle samples
    pass("Invalid",20,10);frame_end(encoder);after_submit();
    finish_callback(4);
    frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(!out.sampleCount && out.droppedFrames==1);
    pass("Failed",10,20);frame_end(encoder);after_submit();finish_callback(5,false);
    frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(out.status==3 && out.droppedFrames==2);
    for(uint32_t i=0;i<MaxPasses;++i) pass("Capacity",10+i*2,11+i*2);
    assert(!pass_writes("overflow"));
    frame_end(encoder);after_submit();finish_callback(6);
    frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(!out.sampleCount && out.droppedFrames==3);
    pass("Pending on shutdown",10,20);frame_end(encoder);after_submit();
    shutdown();initialize();
    finish_callback(7); // touches only an old Completion object, not a reused slot
    frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(!out.sampleCount);
    frame_end(encoder);shutdown();
    aurora::webgpu::g_device.supported=false;
    initialize();frame_begin(encoder);aurora_gpu_stats_read(&out);
    assert(!out.status && !pass_writes("unsupported"));
    shutdown();
    puts("PASS: async queries, backpressure, aggregation, generations, invalid data, shutdown, unsupported");
}
