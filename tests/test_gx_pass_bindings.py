"""Check production GX draw-state reuse against independent draw semantics."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class GxPassBindings(unittest.TestCase):
    def test_draw_state_and_index_addressing(self):
        base = ROOT / 'build/aurora-release-source/lib'
        for unit in ('gx/pipeline.hpp', 'gx/pipeline.cpp', 'gfx/common.cpp'):
            self.assertEqual((base/unit).read_text(),
                (ROOT/'build/android-aurora-source/lib'/unit).read_text())
        header = (base/'gx/pipeline.hpp').read_text()
        source = (base/'gx/pipeline.cpp').read_text()
        draw = header[header.index('struct DrawData {'):header.index('constexpr uint32_t GXPipelineConfigVersion')]
        state = header[header.index('struct RenderState {'):header.index('void render(')]
        render = source[source.index('void render('):source.rindex('} // namespace')]
        common = (base/'gfx/common.cpp').read_text()
        dispatch = common[common.index('static void render_pass('):common.index('void render_pass(const wgpu::RenderPassEncoder& pass, u32')]
        self.assertIn('gx::RenderState gxState{};', dispatch)
        self.assertIn('gx::render(draw.gx, pass, gxState);', dispatch)
        # Every state-changing foreign draw invalidates pass-local GX bindings.
        for marker, end in (('case ShaderType::Clear:', 'break;'),
                            ('case ShaderType::Rml:', 'break;'),
                            ('case CommandType::CustomDraw:', '} break;')):
            block = dispatch[dispatch.index(marker):]
            self.assertIn('gxState = {};', block[:block.index(end)])
        subject = 'namespace aurora::gx {\n' + draw + state + render + '}\n'
        with tempfile.TemporaryDirectory(prefix='gx-bindings-', dir=ROOT/'build') as temporary:
            directory = Path(temporary)
            (directory/'gx_bindings_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(directory), '-I'+str(base.parent/'include'),
                str(ROOT/'tests/native/gx_pass_bindings_selftest.cpp'),
                '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
