"""Execute the renderer's real queue/backpressure code without a GPU."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'build/aurora-release-source'


def function(source, signature):
    start=source.index(signature)
    begin=source.index('{',start)
    depth=1
    end=begin+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]


class EnginePass(unittest.TestCase):
    def test_resource_notifications_and_nonmutating_admission(self):
        if not SOURCE.exists():self.skipTest('configured local renderer unavailable')
        worker=(SOURCE/'lib/gfx/render_worker.cpp').read_text()
        header=(SOURCE/'lib/gfx/render_worker.hpp').read_text()
        common=(SOURCE/'lib/gfx/common.cpp').read_text()
        probe=function(common,'bool wait_for_replay_resources(')
        with tempfile.TemporaryDirectory(prefix='resource-progress-',dir=ROOT/'build') as tmp:
            folder=Path(tmp)
            (folder/'render_worker.hpp').write_text(header)
            (folder/'worker.cpp').write_text(worker.replace('#include <tracy/Tracy.hpp>',
                '#define ZoneScoped\n#define ZoneScopedN(name)\n'))
            test=folder/'test.cpp'
            test.write_text("""
#include "render_worker.hpp"
#include <atomic>
#include <array>
#include <cassert>
using namespace aurora::gfx;
using namespace std::chrono_literals;
render_worker::FrameSlotPool g_frameSlots{2},g_stagingSlots{2};
void* g_recordingFrame=nullptr;
std::optional<size_t> g_reservedFrameSlot,g_reservedStagingSlot;
enum class BufferMapState {Unmapped,Mapped};
std::array<std::atomic<BufferMapState>,2> s_mappingStates;
void enqueue_process_events() {}
"""+probe+"""
int main() {
  using namespace render_worker;
  s_mappingStates[0]=s_mappingStates[1]=BufferMapState::Mapped;
  assert(wait_for_replay_resources(0ns));
  assert(g_frameSlots.free_count()==2 && g_stagingSlots.free_count()==2);
  auto f0=g_frameSlots.acquire(), f1=g_frameSlots.acquire();
  const auto before=progress_version();
  for(int i=0;i<1000;++i)assert(!wait_for_replay_resources(0ns));
  assert(progress_version()==before); // No self-generated retry notifications.
  g_frameSlots.release(f1);
  assert(progress_version()==before);
  assert(wait_for_replay_resources(0ns));
  auto s0=g_stagingSlots.acquire(), s1=g_stagingSlots.acquire();
  assert(!wait_for_replay_resources(0ns));
  std::thread completion([&]{
    std::this_thread::sleep_for(2ms);
    g_stagingSlots.release(s0);
    notify_progress();
  });
  assert(wait_for_replay_resources(2s));
  completion.join();
  g_frameSlots.release(f0);g_stagingSlots.release(s1);
  s_mappingStates[0]=BufferMapState::Unmapped;
  assert(!wait_for_replay_resources(0ns));
  s_mappingStates[0]=BufferMapState::Mapped;
  assert(wait_for_replay_resources(0ns));
  g_recordingFrame=&f0;assert(!wait_for_replay_resources(0ns));g_recordingFrame=nullptr;
  g_reservedFrameSlot=0;assert(!wait_for_replay_resources(0ns));g_reservedFrameSlot.reset();
  // A signal between failed probing and waiting must not be lost.
  for(int i=0;i<500;++i) {
    auto version=progress_version();notify_progress();
    auto start=std::chrono::steady_clock::now();
    wait_for_progress(version,2s);
    assert(std::chrono::steady_clock::now()-start<1s);
  }
  BoundedQueue queue(1);
  assert(queue.has_space());
  assert(queue.try_push({}));assert(!queue.has_space());
  auto version=progress_version();bool closed=false;
  assert(queue.pop_for(0ms,closed));assert(progress_version()!=version);
  version=progress_version();queue.close();assert(progress_version()!=version);
  queue.reset();assert(queue.has_space());
  auto start=std::chrono::steady_clock::now();
  wait_for_progress(progress_version(),2ms);
  assert(std::chrono::steady_clock::now()-start>=1ms);
  // Actual asynchronous worker completion, not just the isolated CV.
  initialize();std::atomic<int> done{0};
  for(int i=0;i<200;++i)enqueue_end_frame(i,[&]{++done;notify_progress();});
  synchronize();assert(done==200);shutdown();
}
""")
            result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',
                str(test),str(folder/'worker.cpp'),'-o',str(folder/'test.exe')],
                capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(folder/'test.exe')],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_replay_preflight_precedes_rebuild_and_retains_deadline(self):
        code=(ROOT/'src/gx/frame_interp.c').read_text()
        idle=function(code,'int mp6_fi_idle_present(')
        preflight=idle.index('aurora_wait_replay_ready(')
        self.assertLess(preflight,idle.index('len = fi_build_replay('))
        self.assertLess(preflight,idle.index('SDL_PumpEvents();'))
        after=idle[preflight:idle.index('prev = (s_prev')]
        self.assertIn('deadlineNs - t0 - margin - s_replayBudgetNs',after)
        self.assertIn('t0 = (int64_t)mp6_host_monotonic_ns();',after)
        self.assertIn('mp6_fi_deadline_fits(deadlineNs, t0, margin, s_replayBudgetNs)',after)
        self.assertIn('return (ready < 0 || paced) ? 0 : -1;',after)


if __name__=='__main__':unittest.main()
