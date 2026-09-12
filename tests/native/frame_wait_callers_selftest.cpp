#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <atomic>
#define ZoneScopedN(x) ((void)0)
#define TracyPlot(name,value) ((void)0)
using PresentClock=std::chrono::steady_clock;
namespace render_worker {
uint64_t version=0;
int waits=0,missed=0;
uint64_t progress_version() { return version; }
bool is_idle() { return false; }
void wait_for_progress(uint64_t observed,std::chrono::nanoseconds) {
    ++waits;
    if(observed==version)++missed;
}
}
void enqueue_process_events() {}
struct Pool {
    int probes=0;
    std::optional<size_t> try_acquire() {
        if(probes++==0) { ++render_worker::version;return std::nullopt; }
        return 0;
    }
    void release(size_t) {}
} g_frameSlots,g_stagingSlots;
enum class BufferMapState { Unmapped,Mapping,Mapped };
struct State {
    mutable int probes=0;
    bool race=true;
    BufferMapState load(std::memory_order) const {
        if(race && probes++==0) {
            ++render_worker::version; // callback wins just after the failed probe
            return BufferMapState::Mapping;
        }
        return BufferMapState::Mapped;
    }
};
std::array<State,1> s_mappingStates;
void map_staging_buffer(size_t) {}
#include "subject.inc"
int main() {
    for(int i=0;i<1000;++i) {
        g_frameSlots.probes=g_stagingSlots.probes=0;
        s_mappingStates[0]={};
        assert(acquire_frame_slot()==0);
        assert(wait_for_staging_buffer(0));
        s_mappingStates[0].race=false;
        assert(acquire_mapped_staging_buffer()==0);
    }
    assert(render_worker::waits==3000 && render_worker::missed==0);
    puts("PASS: frame/staging/map completion between probe and wait is never missed");
}
