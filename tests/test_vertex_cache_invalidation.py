"""Run actual FIFO dispatch across in-place array edits and cache invalidation."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'build/android-aurora-source'


class VertexCacheInvalidation(unittest.TestCase):
    def run_subject(self, old=False):
        with tempfile.TemporaryDirectory(prefix='vertex-invalidate-', dir=ROOT/'build') as temp:
            folder = Path(temp)
            source = (SOURCE/'lib/gx/command_processor.cpp').read_text()
            constants = source[source.index('// GX FIFO opcodes'):source.index('// Read helpers')]
            dispatch = source[source.index('void process(const u8* data'):source.index('// Helper to extract bit fields')]
            if old:
                dispatch = dispatch.replace('      invalidate_vertex_cache();', '')
            helper = ''
            if 'static void invalidate_vertex_cache()' in source:
                start = source.index('static void invalidate_vertex_cache()')
                helper = source[start:source.index('\nvoid process(', start)]
            (folder/'dispatch.inc').write_text(constants+helper+dispatch)
            header = (SOURCE/'lib/gx/gx.hpp').read_text()
            start = header.index('struct AttrArray {')
            (folder/'array.inc').write_text(header[start:header.index('\ninline bool operator==', start)])
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(folder), '-I'+str(SOURCE/'include'),
                str(ROOT/'tests/native/vertex_cache_invalidation_selftest.cpp'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

    def test_invalidation_reuploads_and_separates_draws(self):
        result = self.run_subject()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_ignored_invalidation_reproduces_stale_geometry(self):
        result = self.run_subject(old=True)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('Assertion failed', result.stderr)


if __name__ == '__main__':
    unittest.main()
