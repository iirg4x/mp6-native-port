"""Run the production image/palette caches with mocked GPU uploads only."""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/android-aurora-source'
CASES = ('palette_version', 'palette_selection', 'palette_pointer', 'palette_format',
         'palette_count', 'palette_missing', 'image_pointer', 'image_size',
         'image_format', 'image_mips', 'null_retry', 'static_clear', 'anonymous_palette',
         'dynamic_zero', 'dynamic_pointer', 'dynamic_format', 'dynamic_count', 'dynamic_version',
         'dynamic_uncached', 'dynamic_anonymous', 'palette_removed', 'palette_empty',
         'palette_invalid_slot', 'retained_destroy', 'unchanged_reuse', 'shared_palette_upload')

def compile_subject(folder, source=SOURCE):
    gx = (source/'lib/gx/gx.cpp').read_text()
    header = (source/'lib/gfx/texture.hpp').read_text()
    start = gx.index('struct DynamicPaletteKey {')
    (folder/'texture_cache_private.inc').write_text(gx[start:gx.index('template <typename T>', start)])
    start = gx.index('void evict_texture_object(')
    (folder/'texture_cache_public.inc').write_text(gx[start:gx.index('static inline wgpu::BlendFactor', start)])
    start = header.index('struct GXTexObj_ {')
    (folder/'texture_cache_types.inc').write_text(header[start:])
    exe = folder/'texture-cache.exe'
    result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG', '-DTARGET_PC',
        '-I'+str(folder), '-I'+str(SOURCE/'include'),
        str(ROOT/'tests/native/texture_cache_dependencies_selftest.cpp'), '-o', str(exe)],
        capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return exe

class TextureCacheDependencies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix='texture-cache-', dir=ROOT/'build')
        cls.exe = compile_subject(Path(cls.folder.name))

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_image_palette_dependencies_and_reuse(self):
        for case in CASES:
            with self.subTest(case=case):
                result = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    args = parser.parse_args()
    if args.probe:
        args.probe.mkdir(parents=True, exist_ok=False)
        exe = compile_subject(args.probe)
        results = {}
        for case in CASES:
            p = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=30)
            results[case] = dict(exit=p.returncode, output=p.stdout+p.stderr)
        (args.probe/'results.json').write_text(json.dumps(results, indent=2))
        print(json.dumps({k:v['exit'] for k,v in results.items()}, indent=2))
    else:
        unittest.main(argv=[__file__])
