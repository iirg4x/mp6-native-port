"""CPU contracts for the isolated upstream thin-falloff source adapter."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/integration'))
from xegtao_thin_probe import build_probe_bridge


class ThinProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / 'src/gx/xegtao/bridge.hlsl').read_text()

    def test_only_expected_edits(self):
        for value, literal in ((0, '0.0'), (.3, '0.3'), (.7, '0.7')):
            expected = self.source.replace(
                '#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 0',
                '#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 1').replace(
                '#include "../../../third_party/xegtao/XeGTAO.h"',
                '#include "XeGTAO.h"\n#undef XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION\n'
                '#define XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION (' + literal + 'f)').replace(
                '#include "../../../third_party/xegtao/XeGTAO.hlsli"', '#include "XeGTAO.hlsli"')
            self.assertEqual(build_probe_bridge(self.source, value), expected)

    def test_invalid_values(self):
        for value in (True, False, None, '0.3', float('nan'), float('inf'),
                      float('-inf'), -.001, .701, 1, 10**1000):
            with self.subTest(value=value), self.assertRaises(ValueError):
                build_probe_bridge(self.source, value)

    def test_missing_duplicate_and_changed_anchors(self):
        for anchor in ('#define XE_GTAO_USE_DEFAULT_CONSTANTS 1',
                       '#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 0',
                       '#include "../../../third_party/xegtao/XeGTAO.h"',
                       '#include "../../../third_party/xegtao/XeGTAO.hlsli"'):
            for source in (self.source.replace(anchor, ''), self.source + '\n' + anchor):
                with self.subTest(anchor=anchor), self.assertRaises(ValueError):
                    build_probe_bridge(source, .3)

    def test_ambiguous_macros_and_includes(self):
        for extra in ('#undef XE_GTAO_USE_DEFAULT_CONSTANTS',
                      '#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 1',
                      '#define XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION .3',
                      '#include "XeGTAO.h"', '#include <XeGTAO.hlsli>'):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                build_probe_bridge(self.source + '\n' + extra, .3)

    def test_include_order_and_adjacency(self):
        head = '#include "../../../third_party/xegtao/XeGTAO.h"'
        body = '#include "../../../third_party/xegtao/XeGTAO.hlsli"'
        for replacement in (body + '\n' + head, head + '\n#define SOMETHING 1\n' + body):
            with self.assertRaises(ValueError):
                build_probe_bridge(self.source.replace(head + '\n' + body, replacement), .3)


if __name__ == '__main__':
    unittest.main()
