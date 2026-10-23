"""CPU/source checks for the AO composite ablation transform."""

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests.integration import build_ao_composite_probe as probe


def _composite_body(source):
    start = source.index(probe.COMPOSITE_ENTRY_ANCHOR)
    end = source.index(probe.FOLIAGE_ENTRY_ANCHOR, start)
    return source[start:end]


class AoCompositeProbeTransformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_path = ROOT / 'src/gx/ambient_occlusion.cpp'
        cls.source = cls.source_path.read_text(encoding='utf-8')
        cls.transformed = probe.instrument_ao_composite(cls.source)

    def test_only_final_draw_is_guarded_and_draw_setup_is_unchanged(self):
        original = _composite_body(self.source)
        transformed = _composite_body(self.transformed)

        self.assertIn(probe.COMPOSITE_SETUP_ANCHOR, transformed)
        self.assertEqual(
            transformed.count('pass.SetViewport(0, 0, float(ctx.targetWidth), float(ctx.targetHeight), 0, 1);'),
            original.count('pass.SetViewport(0, 0, float(ctx.targetWidth), float(ctx.targetHeight), 0, 1);'),
        )
        self.assertEqual(
            transformed.count('pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);'),
            original.count('pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);'),
        )
        self.assertEqual(
            transformed.count('pass.SetPipeline(found->pipeline);'),
            original.count('pass.SetPipeline(found->pipeline);'),
        )
        self.assertEqual(
            transformed.count('pass.SetBindGroup(0, bindings);'),
            original.count('pass.SetBindGroup(0, bindings);'),
        )
        self.assertIn(
            'if (!skipComposite) {\n        pass.Draw(3);\n    }',
            transformed,
        )
        self.assertEqual(transformed.count('pass.Draw(3);'), original.count('pass.Draw(3);'))
        self.assertNotIn('if (skipComposite) {', transformed)
        self.assertNotIn('job.available.store(true, std::memory_order_release);\n        return;', transformed)

    def test_shared_cleanup_is_unchanged_and_still_after_draw(self):
        original = _composite_body(self.source)
        transformed = _composite_body(self.transformed)
        original_release = original.index(probe.COMPOSITE_RELEASE_ANCHOR)
        transformed_release = transformed.index(probe.COMPOSITE_RELEASE_ANCHOR)

        self.assertEqual(
            transformed[transformed_release:],
            original[original_release:],
        )
        self.assertLess(
            transformed.index('pass.Draw(3);'),
            transformed.index(probe.COMPOSITE_RELEASE_ANCHOR),
        )
        self.assertLess(
            transformed.index('++g_composited;'),
            transformed.index(probe.COMPOSITE_RELEASE_ANCHOR),
        )

    def test_skip_policy_requires_exact_environment_value_one(self):
        self.assertEqual(self.transformed.count(probe.ENV_VAR), 1)
        self.assertIn(
            'const bool skip = value && std::strcmp(value, "1") == 0;',
            self.transformed,
        )
        self.assertIn(
            '[MP6-COMPOSITE-PROBE] skip=1 local-diagnostic-only',
            self.transformed,
        )
        self.assertNotIn('skip=0', self.transformed)
        # Production may parse other variables; the policy adds no parsing.
        for token in ('atoi(', 'value[0]', 'value[1]'):
            self.assertEqual(self.transformed.count(token), self.source.count(token))

    def test_transform_is_pure_and_importable_without_android_helpers(self):
        self.assertEqual(
            self.source_path.read_text(encoding='utf-8'),
            self.source,
        )
        builder = (ROOT / 'tests/integration/build_ao_composite_probe.py').read_text(encoding='utf-8')
        module_body = builder.split('\ndef main():', 1)[0]
        self.assertNotIn('from android_local import', module_body)
        self.assertIs(probe.instrument_ao, probe.instrument_ao_composite)

    def test_anchor_drift_and_reinstrumentation_fail_closed(self):
        with self.assertRaises(ValueError):
            probe.instrument_ao_composite(
                self.source.replace(
                    '    pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);\n',
                    '',
                    1,
                )
            )
        with self.assertRaises(ValueError):
            probe.instrument_ao_composite(
                self.source.replace(
                    probe.COMPOSITE_DRAW_ANCHOR,
                    probe.COMPOSITE_DRAW_ANCHOR + '\n' + probe.COMPOSITE_DRAW_ANCHOR,
                    1,
                )
            )
        with self.assertRaises(ValueError):
            probe.instrument_ao_composite(
                self.source.replace(
                    probe.COMPOSITE_RELEASE_ANCHOR,
                    probe.COMPOSITE_RELEASE_ANCHOR + '\n' + probe.COMPOSITE_RELEASE_ANCHOR,
                    1,
                )
            )
        with self.assertRaises(ValueError):
            probe.instrument_ao_composite(self.transformed)


if __name__ == '__main__':
    unittest.main()
