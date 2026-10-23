from pathlib import Path
import unittest

from tests.integration.ao_foreground_probe import (
    ANCHOR, MARKER, NEEDLE, instrument_upstream_foreground,
)


ROOT = Path(__file__).resolve().parents[1]


class AOForegroundProbeTests(unittest.TestCase):
    def test_current_source_and_both_native_shader_variants(self):
        source = (ROOT / 'src/gx/ambient_occlusion.cpp').read_text()
        transformed = instrument_upstream_foreground(source)
        self.assertIn('if (std::getenv("MP6_QA_FOREGROUND_AO"))', transformed)
        self.assertIn(MARKER, transformed)
        self.assertIn('code.replace(at, needle.size(), "return vec4f(foreground,0);")', transformed)
        self.assertEqual(source.count('pass.Draw(3)'), transformed.count('pass.Draw(3)'))
        for name in ('composite_shader.hpp', 'half_composite_shader.hpp'):
            shader = (ROOT / 'src/gx/xegtao' / name).read_text()
            self.assertEqual(shader.count(NEEDLE), 1, name)
        self.assertLess(transformed.index('code.replace(at'), transformed.index('upstreamCompositeSource'))

    def test_changed_or_duplicate_anchor_fails_closed(self):
        with self.assertRaises(ValueError):
            instrument_upstream_foreground('')
        with self.assertRaises(ValueError):
            instrument_upstream_foreground(ANCHOR + '\n' + ANCHOR)
        with self.assertRaises(ValueError):
            instrument_upstream_foreground(instrument_upstream_foreground(ANCHOR))

    def test_generated_builder_uses_probe_and_records_provenance(self):
        builder = (ROOT / 'tests/integration/build_board_qa.py').read_text()
        self.assertIn('contents = instrument_upstream_foreground(contents)', builder)
        self.assertIn("'ao_foreground_probe_sha256'", builder)


if __name__ == '__main__':
    unittest.main()
