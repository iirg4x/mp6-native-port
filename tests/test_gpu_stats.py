"""Exercise the actual Release profiler state machine with deferred GPU callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]

class GpuStats(unittest.TestCase):
    def test_async_profiler(self):
        source = (ROOT/'build/android-aurora-source/lib/webgpu/gpu_prof.cpp').read_text()
        begin = source.index('#else\n\n#include <algorithm>') + len('#else\n')
        end = source.index('\n#endif', begin)
        subject = source[begin:end]
        with tempfile.TemporaryDirectory(prefix='gpu-stats-', dir=ROOT/'build') as temp:
            folder = Path(temp)
            (folder/'gpu_stats_subject.inc').write_text(subject)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',
                '-I',str(folder),'-I',str(ROOT/'build/android-aurora-source/include'),
                str(ROOT/'tests/native/gpu_stats_selftest.cpp'),'-o',str(exe)],
                capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS',result.stdout)

    def test_stat_windows_have_no_implementation_essays(self):
        source = (ROOT/'src/gx/console/console_stats.c').read_text()
        for text in ['GPU timestamps not yet implemented','Next increment, in this order',
                     'The two FI rows','Every row is sorted by the BYTES','cs_window_note(']:
            self.assertNotIn(text,source)
        self.assertIn('aurora_gpu_stats_read(&gpu)',source)

    def test_close_is_visible_and_does_not_disable_stats(self):
        source = (ROOT/'src/gx/ui/console.cpp').read_text()
        self.assertIn('close->SetId("console-close")',source)
        self.assertIn('hide(false);',source)
        self.assertNotIn('mp6_console_clear_overlays()',source)
        css = (ROOT/'res/rml/console.rcss').read_text()
        self.assertIn('min-height: 44dp;',css)

if __name__ == '__main__':
    unittest.main()
