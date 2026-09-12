"""Publication/lifetime tests for the actual renderer statistics snapshot."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'build/android-aurora-source'

def compile_subject(folder, old_pointer=False):
    common = (SOURCE/'lib/gfx/common.cpp').read_text()
    pipeline = (SOURCE/'lib/gfx/pipeline_cache.cpp').read_text()
    start = common.index('static AuroraStats s_completedStats')
    (folder/'stats_publication.inc').write_text(common[start:common.index('uint32_t g_drawCallCount',start)])
    start = pipeline.index('static std::atomic<uint32_t> queuedPipelines')
    (folder/'stats_pipeline.inc').write_text(pipeline[start:pipeline.index('template <typename PipelineConfig>',start)])
    start = common.index('const AuroraStats* aurora_get_stats()')
    getter = common[start:common.index('float aurora_get_fps()',start)]
    if old_pointer:
        getter = 'const AuroraStats* aurora_get_stats() { return &aurora::gfx::s_completedStats; }\n'
    (folder/'stats_getter.inc').write_text(getter)
    exe = folder/'stats-snapshot.exe'
    result = subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',
        '-I'+str(folder),'-I'+str(SOURCE/'include'),
        str(ROOT/'tests/native/stats_snapshot_selftest.cpp'),'-o',str(exe)],
        capture_output=True,text=True,timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return exe

class StatsSnapshot(unittest.TestCase):
    def test_complete_frames_and_thread_local_lifetime(self):
        with tempfile.TemporaryDirectory(prefix='stats-snapshot-',dir=ROOT/'build') as path:
            exe = compile_subject(Path(path))
            p = subprocess.run([str(exe)],capture_output=True,text=True,timeout=45)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

    def test_exposing_live_pointer_reproduces_mutation(self):
        with tempfile.TemporaryDirectory(prefix='stats-snapshot-old-',dir=ROOT/'build') as path:
            exe = compile_subject(Path(path),True)
            p = subprocess.run([str(exe)],capture_output=True,text=True,timeout=45)
            self.assertEqual(p.returncode,1,p.stdout+p.stderr)
            self.assertIn('Assertion failed',p.stderr)

if __name__ == '__main__':
    unittest.main()
