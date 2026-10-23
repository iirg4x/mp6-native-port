"""Native Full-only depth format selection and per-job ownership contracts."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]
AMBIENT = ROOT / 'src/gx/ambient_occlusion.cpp'


class NativePreparedCanonical(unittest.TestCase):
    def test_actual_selection_expression_requires_admitted_full_upstream(self):
        source = AMBIENT.read_text()
        expression = re.search(r'job\.canonicalDepth = (.*?);', source, re.S).group(1)
        facade = r'''
#include <cassert>
namespace mp6::xegtao { enum class Resolution { Full, Half }; }
struct { mp6::xegtao::Resolution resolution; } g_xegtaoPolicy;
struct { bool canonicalDepth; } job;
int main() {
    for (bool upstream : {false, true})
        for (bool g_canonicalPreparationRequested : {false, true})
            for (auto resolution : {mp6::xegtao::Resolution::Full, mp6::xegtao::Resolution::Half}) {
                g_xegtaoPolicy.resolution = resolution;
                job.canonicalDepth = EXPRESSION;
                assert(job.canonicalDepth == (upstream && g_canonicalPreparationRequested &&
                                              resolution == mp6::xegtao::Resolution::Full));
            }
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>').replace('EXPRESSION', expression)
        with tempfile.TemporaryDirectory(prefix='ao-native-canonical-', dir=ROOT/'build') as folder:
            cpp, exe = Path(folder)/'selection.cpp', Path(folder)/'selection.exe'
            cpp.write_text(facade)
            compiled = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                                       str(cpp), '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_opt_in_is_latched_and_shader_cache_keys_include_format(self):
        source = AMBIENT.read_text()
        self.assertEqual(source.count('std::getenv("MP6_AO_XEGTAO_PREPARED_CANONICAL")'), 1)
        self.assertIn('bool g_canonicalPreparationRequested = false;', source)
        self.assertIn('canonical && std::strcmp(canonical, "1") == 0', source)
        self.assertIn('g_attachmentDepthPipelines[msaa][decals][canonicalDepth]', source)
        self.assertIn('g_upstreamCompositeShaders[canonicalDepth]', source)
        # The composite pipeline lookup (draw_composite) keys on the job's flag.
        self.assertIn('p.canonicalDepth == canonicalDepth', source)
        self.assertIn('draw_composite(ctx, pass, job.composite, job.preparedDepth, job.upstreamSignal, job.canonicalDepth)', source)
        self.assertIn('g_canonicalPreparationRequested = g_canonicalPreparationLogged = false;', source)

    def test_adapter_upload_precedes_preparation_and_failures_restore_viewz(self):
        source = AMBIENT.read_text().split('void encode(', 1)[1].split('void composite(', 1)[0]
        self.assertLess(source.index('g_xegtao.prepare_adapter('), source.index('encoder.BeginRenderPass'))
        self.assertIn('.buffer = job.upstream.adapter', source)
        fallback = source.index('g_canonicalPreparationRequested = false;')
        self.assertLess(fallback, source.index('encoder.BeginRenderPass'))
        self.assertIn('job.canonicalDepth = false;', source[fallback:])
        self.assertIn('.clearValue = {job.canonicalDepth ? 1.0 : 0.0, 0, 0, 0}', source)
        self.assertIn('DepthInput::CanonicalNdc : mp6::xegtao::DepthInput::ViewZ', source)
        self.assertLess(source.index('pass.End();'), source.index('g_xegtao.encode('))

    def test_external_depth_groups_are_keyed_by_every_input_and_output_view(self):
        source = (ROOT/'src/gx/xegtao/native.hpp').read_text()
        encode = source.split('    void encode(', 1)[1]
        canonical = encode.split('if (input == DepthInput::CanonicalNdc) {', 1)[1].split('} else {', 1)[0]
        self.assertEqual(canonical.count('.textureView = cleanDepth'), 2)
        for level in range(5):
            self.assertIn(f'.binding = {level+1}, .textureView = s.mipViews[{level}]', canonical)
        self.assertIn('.binding = 1, .textureView = s.normalView', canonical)
        self.assertIn('cached_bind_group(prefilterDesc)', canonical)
        self.assertIn('cached_bind_group(normalDesc)', canonical)
        self.assertNotIn('s.rawView =', canonical)
        self.assertNotIn('s.edgeView =', canonical)
        self.assertNotIn('s.prefilter =', canonical)
        self.assertNotIn('s.normal =', canonical)
        self.assertNotIn('Destroy()', canonical)
        self.assertIn('DepthInput input = DepthInput::ViewZ', encode)
        self.assertIn('policy.resolution == Resolution::Full', encode)
        self.assertIn('input == DepthInput::CanonicalNdc ? externalPrefilter : s.prefilter', encode)
        self.assertIn('input == DepthInput::CanonicalNdc ? externalNormal : s.normal', encode)

    def test_qa_selector_forwards_canonical_mode_to_all_shader_variants(self):
        source = (ROOT/'tests/integration/build_board_qa.py').read_text()
        selector = source.split('inline std::string qa_ao_shader(', 1)[1].split("'''", 1)[0]
        self.assertIn('bool canonicalDepth=false', selector)
        self.assertEqual(selector.count('multisampledDepth,decals,canonicalDepth)'), 2)


if __name__ == '__main__':
    unittest.main()
