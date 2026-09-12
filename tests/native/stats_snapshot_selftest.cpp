#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <aurora/gfx.h>
#include "selftest_assert.h"
namespace aurora::gfx {
#include "stats_pipeline.inc"
#include "stats_publication.inc"
}
#include "stats_getter.inc"

static AuroraStats frame(uint32_t n) {
    AuroraStats s{};
    s.drawCallCount=n; s.mergedDrawCallCount=n^0xA517u;
    s.lastVertSize=n*3; s.lastUniformSize=n*5; s.lastIndexSize=n*7;
    s.lastStorageSize=n*11; s.lastTextureUploadSize=n*13;
    return s;
}
static void valid(const AuroraStats& s) {
    const auto expected=frame(s.drawCallCount);
    assert(s.mergedDrawCallCount==expected.mergedDrawCallCount);
    assert(s.lastVertSize==expected.lastVertSize && s.lastUniformSize==expected.lastUniformSize);
    assert(s.lastIndexSize==expected.lastIndexSize && s.lastStorageSize==expected.lastStorageSize);
    assert(s.lastTextureUploadSize==expected.lastTextureUploadSize);
}
int main() {
    using namespace aurora::gfx;
    publish_completed_stats(frame(1));
    const auto* held=aurora_get_stats();
    const auto copy=*held;
    std::thread worker([]{publish_completed_stats(frame(2));}); worker.join();
    // Deterministic old-policy failure without needing to provoke a data race.
    assert(std::memcmp(held,&copy,sizeof(copy))==0);
    assert(aurora_get_stats()==held && held->drawCallCount==2);
    std::atomic<bool> start=false;
    std::vector<std::thread> threads;
    threads.emplace_back([&]{
        while(!start.load()) std::this_thread::yield();
        for(uint32_t i=3;i<200003;++i) publish_completed_stats(frame(i));
    });
    threads.emplace_back([&]{
        while(!start.load()) std::this_thread::yield();
        for(unsigned i=0;i<200000;++i) { ++queuedPipelines; ++createdPipelines; --queuedPipelines; }
    });
    for(unsigned r=0;r<3;++r) threads.emplace_back([&]{
        while(!start.load()) std::this_thread::yield();
        for(unsigned i=0;i<200000;++i) {
            const auto* snapshot=aurora_get_stats();
            valid(*snapshot);
            assert(snapshot->queuedPipelines<=1 && snapshot->createdPipelines<=200000);
        }
    });
    start=true;
    for(auto& thread:threads) thread.join();
    // Other threads' API calls and publications do not mutate this thread's snapshot.
    assert(held->drawCallCount==2);
    const auto* final=aurora_get_stats();
    valid(*final);
    assert(final->drawCallCount==200002 && final->createdPipelines==200000 && final->queuedPipelines==0);
    puts("PASS: 200000 frame publications, 600000 concurrent reads, atomic pipeline counters, stable per-thread pointers");
}
