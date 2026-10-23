"""Compile the real on-demand profiler helpers; no GPU or prebuilt tree."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build
from tests.test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'lib/webgpu/gpu_prof.cpp'
HEADER = 'include/aurora/gpu_timing_snapshot.h'
PUBLIC_HEADER = 'include/aurora/aurora.h'


def replay():
    """The profiler sources as they are now (third_party/aurora)."""
    return {target: (ROOT / 'third_party/aurora' / target).read_text(encoding='utf-8')
            for target in (TARGET, HEADER, PUBLIC_HEADER)}

class GpuTimingSnapshot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = replay()
        cls.source = cls.sources[TARGET]

    def compile_run(self, source, *, language='c++'):
        with tempfile.TemporaryDirectory(prefix='gpu-snapshot-') as temporary:
            folder = Path(temporary)
            for target in (HEADER, PUBLIC_HEADER):
                destination = folder/target
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(self.sources[target])
            cpp = folder/('test.cpp' if language == 'c++' else 'test.c')
            cpp.write_text(source)
            exe = folder/'test.exe'
            command = [build.ZIG, 'c++' if language == 'c++' else 'cc',
                       '-std=c++20' if language == 'c++' else '-std=c11',
                       '-O2', '-UNDEBUG', '-I'+str(folder/'include'), str(cpp)]
            command += ['-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_actual_helpers_snapshot_distribution_and_lifecycle(self):
        source = self.source
        harness = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <aurora/gpu_timing_snapshot.h>
namespace aurora::webgpu::gpu_prof {
constexpr uint32_t MaxPasses=256, QueryCount=MaxPasses*2;
constexpr uint64_t BufferSize=QueryCount*sizeof(uint64_t);
constexpr size_t RingDepth=4, HistorySize=120;
enum State { Pending, Mapped, Failed };
struct Completion { std::atomic<int> state{Pending}; };
struct FakeReadback {
  const uint64_t* timestamps=nullptr;
  const void* GetConstMappedRange(uint64_t,uint64_t) { return timestamps; }
  void Unmap() {}
  void Destroy() {}
  explicit operator bool() const { return timestamps != nullptr; }
};
struct Slot {
  FakeReadback readback;
  std::shared_ptr<Completion> completion;
  std::array<std::array<char,64>,MaxPasses> names{};
  uint32_t count=0;
  uint64_t generation=0;
  bool overflow=false;
};
std::array<Slot,RingDepth> slots{};
Slot* recording=nullptr;
Slot* submitted=nullptr;
uint32_t percentileCalls=0;
void (*percentileHook)()=nullptr;
uint64_t interleavedId=0;
'''
        harness += source[source.index('struct Sample {'):source.index('std::array<Slot, RingDepth> slots;')]
        harness += source[source.index('std::atomic<uint64_t> request{0}'):source.index('void reset_history(')]
        # Count invocation without changing the extracted percentile algorithm.
        harness += function(source, 'window_percentile').replace(
            'window_percentile(', 'actual_window_percentile(', 1)
        harness += r'''
double window_percentile(const std::array<double,HistorySize>& values,uint32_t count,bool p95) {
  ++percentileCalls;
  if (percentileHook) {
    const auto hook=percentileHook;
    percentileHook=nullptr;
    hook();
  }
  return actual_window_percentile(values,count,p95);
}
'''
        for name in ('reset_history', 'row_for', 'emit_window_report', 'named_ao_pass',
                     'cancel_timing_snapshot', 'publish_timing_snapshot', 'publish', 'consume', 'drain'):
            harness += '\n'+function(source, name)
        # Select non-Tracy shutdown explicitly, since the file contains both.
        nontracy = source[source.index('\n#else\n\n#include <algorithm>'):]
        harness += '\n'+function(nontracy, 'shutdown')+'\n}\n'
        for name in ('aurora_gpu_stats_set_enabled', 'aurora_gpu_stats_read',
                     'aurora_gpu_timing_snapshot_request', 'aurora_gpu_timing_snapshot_read'):
            harness += '\n'+function(source, name)
        harness += r'''
using namespace aurora::webgpu::gpu_prof;
bool near(double a,double b) { return std::abs(a-b)<1e-9; }
void unavailable(uint64_t id) {
  AuroraGpuTimingSnapshot out{};
  out.requestId=99;out.stats.sampleCount=99;out.aoP95Ms=99;
  assert(!aurora_gpu_timing_snapshot_read(id,&out));
  assert(out.requestId==0 && out.stats.sampleCount==0 && out.aoP95Ms==0);
}
int main() {
  assert(aurora_gpu_timing_snapshot_request()==0);
  unavailable(0);
  assert(!aurora_gpu_timing_snapshot_read(0,nullptr));
  supported=true;
  assert(aurora_gpu_timing_snapshot_request()==0);
  aurora_gpu_stats_set_enabled(true);
  // A request made before the worker handles the new generation survives reset.
  const auto initial=aurora_gpu_timing_snapshot_request();
  assert(initial!=0);
  reset_history(request.load());
  unavailable(initial);
  publish();
  AuroraGpuTimingSnapshot out{};
  assert(aurora_gpu_timing_snapshot_read(initial,&out));
  assert(out.requestId==initial && out.generation==generation);
  assert(out.stats.status==1 && out.stats.sampleCount==0 && out.aoP95Ms==0);
  const uint32_t emptyCalls=percentileCalls;
  publish();publish();
  assert(percentileCalls==emptyCalls);

  samples=HistorySize;nameCount=5;
  std::strcpy(names[0].data(),"XeGTAO main");
  std::strcpy(names[1].data(),"AO blur");
  std::strcpy(names[2].data(),"GTAO");
  std::strcpy(names[3].data(),"Depth snapshot");
  std::strcpy(names[4].data(),"EFB composite");
  assert(named_ao_pass("GTAO") && named_ao_pass("AO restore"));
  assert(!named_ao_pass("GTAO unrelated"));
  assert(!named_ao_pass("Depth snapshot") && !named_ao_pass("Some AO pass"));
  assert(!named_ao_pass("XeGTAOther") && !named_ao_pass("EFB composite"));
  for(uint32_t i=0;i<HistorySize;++i) {
    history[i].span=i+1;history[i].between=(i+1)*.5;
    history[i].rows[0]=i<8 ? 10 : 0;
    history[i].rows[1]=i>=8 && i<16 ? 10 : 0;
    history[i].rows[2]=2;
    history[i].rows[3]=300;history[i].rows[4]=400;
  }
  const auto full=aurora_gpu_timing_snapshot_request();
  assert(full>initial);
  unavailable(initial);unavailable(full);
  publish();
  assert(aurora_gpu_timing_snapshot_read(full,&out));
  assert(out.stats.status==2 && out.stats.sampleCount==120 && out.stats.droppedFrames==0);
  assert(std::memcmp(&out.stats,&published,sizeof published)==0);
  assert(near(out.stats.spanAverageMs,60.5) && near(out.spanMedianMs,60.5));
  assert(out.stats.spanMaxMs==120 && out.spanP95Ms==114);
  assert(near(out.betweenMedianMs,30.25) && out.betweenP95Ms==57);
  assert(near(out.aoAverageMs,2.0+160.0/120.0));
  assert(out.aoMaxMs==12 && out.aoMedianMs==2 && out.aoP95Ms==12);
  // The disjoint eight-frame spikes give per-pass p95 10+10+2=22, not 12.
  double sumP95=0;
  for(uint32_t row=0;row<3;++row) {
    std::array<double,HistorySize> values{};
    for(uint32_t i=0;i<HistorySize;++i) values[i]=history[i].rows[row];
    sumP95+=actual_window_percentile(values,HistorySize,true);
  }
  assert(sumP95==22 && sumP95!=out.aoP95Ms);
  const uint32_t fullCalls=percentileCalls;
  history[0].span=999;
  publish();
  assert(percentileCalls==fullCalls);
  assert(aurora_gpu_timing_snapshot_read(full,&out) && out.stats.spanMaxMs==120);

  const auto superseded=aurora_gpu_timing_snapshot_request();
  const auto latest=aurora_gpu_timing_snapshot_request();
  assert(latest>superseded);
  publish();
  unavailable(superseded);
  assert(aurora_gpu_timing_snapshot_read(latest,&out));
  assert(percentileCalls==fullCalls+6);
  // Deterministic request arrival while worker aggregation is outside the lock.
  const auto computing=aurora_gpu_timing_snapshot_request();
  percentileHook=[] { interleavedId=aurora_gpu_timing_snapshot_request(); };
  publish();
  assert(interleavedId>computing && snapshotPending.load()==interleavedId);
  unavailable(computing);unavailable(interleavedId);
  publish();
  assert(aurora_gpu_timing_snapshot_read(interleavedId,&out));
  samples=7;dropped=2;
  const auto partial=aurora_gpu_timing_snapshot_request();
  publish();
  assert(aurora_gpu_timing_snapshot_read(partial,&out));
  assert(out.stats.sampleCount==7 && out.stats.droppedFrames==2 && out.stats.status==2);

  // Both already published and pending results fail closed after a map failure.
  slots[0].generation=generation;
  slots[0].completion=std::make_shared<Completion>();
  slots[0].completion->state=Failed;
  drain();unavailable(partial);
  assert(mapFailed && dropped==3);
  const auto failed=aurora_gpu_timing_snapshot_request();
  const uint32_t failureCalls=percentileCalls;
  publish();unavailable(failed);
  assert(percentileCalls==failureCalls && snapshotPending.load()==0);
  mapFailed=false;
  const auto nullReadback=aurora_gpu_timing_snapshot_request();
  Slot missing;
  missing.count=1;
  consume(missing);
  publish();unavailable(nullReadback);
  assert(percentileCalls==failureCalls);

  // Overflowed names hide row membership; refuse to report an AO sum.
  nameCount=AURORA_GPU_STAT_ROWS;
  const auto collapsed=aurora_gpu_timing_snapshot_request();
  publish();unavailable(collapsed);
  assert(percentileCalls==failureCalls);
  nameCount=5;
  const auto duringDisable=aurora_gpu_timing_snapshot_request();
  percentileHook=[] { aurora_gpu_stats_set_enabled(false); };
  publish();unavailable(duringDisable);
  aurora_gpu_stats_set_enabled(true);
  reset_history(request.load());
  const auto oldGeneration=aurora_gpu_timing_snapshot_request();
  publish();
  assert(aurora_gpu_timing_snapshot_read(oldGeneration,&out));
  aurora_gpu_stats_set_enabled(false);
  unavailable(oldGeneration);
  assert(aurora_gpu_timing_snapshot_request()==0);
  aurora_gpu_stats_set_enabled(true);
  unavailable(oldGeneration);
  const auto afterReset=aurora_gpu_timing_snapshot_request();
  assert(afterReset>oldGeneration);
  // An old worker reset must preserve a request queued for the newer generation.
  reset_history(generation);
  assert(snapshotPending.load()==afterReset);
  unavailable(afterReset);
  reset_history(request.load());
  publish();
  assert(aurora_gpu_timing_snapshot_read(afterReset,&out) && out.stats.sampleCount==0);
  shutdown();unavailable(afterReset);
  assert(aurora_gpu_timing_snapshot_request()==0 && snapshotPending.load()==0);
  supported=true;reset_history(request.load());
  const auto afterShutdown=aurora_gpu_timing_snapshot_request();
  assert(afterShutdown>afterReset);
  unavailable(afterReset);
  publish();
  assert(aurora_gpu_timing_snapshot_read(afterShutdown,&out));
  snapshotNextId=UINT64_MAX;
  assert(aurora_gpu_timing_snapshot_request()==0); // Never wrap/reuse IDs.
}
'''
        self.compile_run(harness)

    def test_header_is_c_compatible_and_tracy_stubs_fail_closed(self):
        self.compile_run('#include <aurora/gpu_timing_snapshot.h>\n'
                         'int main(void) { AuroraGpuTimingSnapshot s = {0}; return s.requestId != 0; }\n',
                         language='c')
        tracy = self.source[self.source.rindex('#ifdef TRACY_ENABLE'):]
        self.compile_run('#include <aurora/gpu_timing_snapshot.h>\n#include <cassert>\n'
                         '#define TRACY_ENABLE\n'+tracy+r'''
int main() {
  AuroraGpuTimingSnapshot out{};out.requestId=10;
  assert(aurora_gpu_timing_snapshot_request()==0);
  assert(!aurora_gpu_timing_snapshot_read(10,&out) && out.requestId==0);
  assert(!aurora_gpu_timing_snapshot_read(10,nullptr));
}
''')


if __name__ == '__main__':
    unittest.main()
