"""Execute the production uniform writer against the original scratch-copy path."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


def span(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


class UniformDirect(unittest.TestCase):
    def test_full_serialization_and_writer_contract(self):
        source = ROOT/'build/aurora-release-source'
        android = ROOT/'build/android-aurora-source'
        for unit in ('lib/gfx/common.hpp', 'lib/gfx/common.cpp', 'lib/gfx/uniform_writer.hpp',
                     'lib/gx/gx.hpp', 'lib/gx/shader_info.cpp'):
            self.assertEqual((source/unit).read_text(), (android/unit).read_text(), unit)
        candidate = (source/'lib/gx/shader_info.cpp').read_text()
        reference = candidate
        # Reverse only the buffer plumbing, preserving every production branch,
        # arithmetic operation, and serialized field in the comparison.
        for new, old in (
            ('  auto buf = gfx::begin_uniform(info.uniformSize);\n'
             '  if (!buf) {\n    g_gxState.stateDirty = false;\n    return {};\n  }',
             '  static ByteBuffer buf;\n  buf.clear();\n  buf.reserve_extra(info.uniformSize);'),
            ('  return buf.finish();', '  return gfx::push_uniform(buf.data(), buf.size());'),
        ):
            self.assertEqual(reference.count(new), 1)
            reference = reference.replace(new, old)
        common = (source/'lib/gfx/common.hpp').read_text()
        gx = (source/'lib/gx/gx.hpp').read_text()
        types = ''.join(span(gx, first, last) for first, last in (
            ('struct ColorChannelConfig {', 'struct TcgConfig {'),
            ('struct FogState {', 'struct TevSwap {'),
            ('struct IndTexMtxInfo {', 'struct VtxAttrFmt {'),
            ('struct PnMtx {', 'struct AttrArray {'),
        ))
        start = gx.index('struct ShaderInfo {')
        end = gx.index('\n};', gx.index('struct BindGroupRanges {', start)) + 4
        types += gx[start:end]
        with tempfile.TemporaryDirectory(prefix='uniform-direct-', dir=ROOT/'build') as temporary:
            folder = Path(temporary)
            (folder/'buffer.inc').write_text(span(common, 'class ByteBuffer {', '} // namespace aurora'))
            (folder/'gx_types.inc').write_text(types)
            (folder/'helpers.inc').write_text(
                span(reference, 'Vec4<float> texture_size_bias(', '\nvoid color_arg_reg_info(')
                + span(reference, 'static f32 tex_offset(', 'gfx::Range build_uniform('))
            for name, text in (('reference', reference), ('subject', candidate)):
                body = text[text.index('gfx::Range build_uniform('):text.rindex('} // namespace aurora::gx')]
                body = body.replace('gfx::Range build_uniform(', '[[gnu::noinline]] gfx::Range build_uniform(')
                if name == 'reference':
                    body = body.replace('gfx::push_uniform(buf.data(), buf.size())', 'reference_push(buf.data(), buf.size())')
                (folder/(name+'.inc')).write_text(body)
            for reversed_z in (0, 1):
                exe = folder/f'uniform-z{reversed_z}.exe'
                result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                    '-DTARGET_PC=1', '-DTEST_REVERSED_Z='+str(reversed_z),
                    '-I'+str(folder), '-I'+str(source), '-I'+str(source/'include'),
                    str(ROOT/'tests/native/uniform_direct_selftest.cpp'), '-o', str(exe)],
                    capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                for mode, expected in (('', 0), ('overflow', 73), ('interleaved', 73),
                                       ('alignment', 73), ('double-finish', 73), ('invalid', 73),
                                       ('borrowed-capacity', 73), ('after-finish', 73), ('negative', 1)):
                    with self.subTest(reversed_z=reversed_z, mode=mode):
                        result = subprocess.run([str(exe)]+([mode] if mode else []),
                            capture_output=True, text=True, timeout=45)
                        self.assertEqual(result.returncode, expected, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
