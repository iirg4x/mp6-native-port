#include <cassert>
#include <chrono>
#include <mutex>
struct Clock {
    using time_point=std::chrono::steady_clock::time_point;
    static time_point current;
    static time_point now() { return current; }
};
Clock::time_point Clock::current{};
std::mutex g_mutex;
bool g_enabled=true,g_snapshotRequested=false;
Clock::time_point g_nextSnapshotTime{};
#include "subject.inc"
int main() {
    using namespace std::chrono_literals;
    assert(!needs_frame_snapshot());
    g_snapshotRequested=true;
    assert(needs_frame_snapshot() && g_snapshotRequested); // non-consuming
    g_nextSnapshotTime=Clock::now()+100ms;
    assert(!needs_frame_snapshot() && g_snapshotRequested); // throttled, not lost
    Clock::current+=100ms;assert(needs_frame_snapshot());
    g_enabled=false;assert(!needs_frame_snapshot() && g_snapshotRequested);
    g_enabled=true;g_snapshotRequested=false;
    const bool currentPassReads=needs_frame_snapshot();
    g_snapshotRequested=true; // arrives after current pass store decision
    assert(!currentPassReads && needs_frame_snapshot()); // belongs to next pass
}
