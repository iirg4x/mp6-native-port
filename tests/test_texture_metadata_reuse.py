"""Run the actual FIFO metadata decoder, including its Release bounds checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/android-aurora-source'


def compile_subject(folder, reference_body=None, candidate_source=None):
    cp = candidate_source if candidate_source is not None else (SOURCE/'lib/gx/command_processor.cpp').read_text()
    start = cp.index('    CHECK(pos + 34 <= size, "GX_AURORA_LOAD_TEXOBJ read overrun");')
    end = cp.index('  } else if (subCmd == GX_AURORA_LOAD_TLUT)', start)
    body = cp[start:end]
    (folder/'texture_metadata_subject.inc').write_text(body)
    if reference_body is not None:
        (folder/'texture_metadata_reference.inc').write_text(reference_body)
    header = (SOURCE/'lib/gfx/texture.hpp').read_text()
    start = header.index('struct GXTexObj_ {')
    (folder/'texture_metadata_types.inc').write_text(header[start:header.index('struct GXTlutObj_', start)])
    exe = folder/'texture-metadata.exe'
    command = [build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG', '-DTARGET_PC', '-I'+str(folder),
        '-I'+str(SOURCE/'include'), str(ROOT/'tests/native/texture_metadata_selftest.cpp'), '-o', str(exe)]
    if reference_body is not None: command.insert(2, '-DMETADATA_BENCH')
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode: raise RuntimeError(result.stderr)
    return exe


class TextureMetadataReuse(unittest.TestCase):
    def test_production_decoder_and_draw_barriers(self):
        with tempfile.TemporaryDirectory(prefix='texture-metadata-', dir=ROOT/'build') as tmp:
            exe = compile_subject(Path(tmp))
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__': unittest.main()
