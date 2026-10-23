"""Structural checks for the QA-only AO CPU profiling transform."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/integration'))

from engine_benchmark_metrics import frame_profile
from frame_profile_source import HEADER, instrument_ao


class AoCpuProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_path = ROOT / 'src/gx/ambient_occlusion.cpp'
        cls.source = cls.source_path.read_text(encoding='utf-8')
        cls.transformed = instrument_ao(cls.source)

    def test_current_source_gets_callback_entry_timers(self):
        """Record timers cover each callback from entry through Draw."""
        encode_anchor = (
            'void encode(const gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &encoder,\n'
            '            const void *payload, size_t, void *) {\n'
            '    auto &job = payload_job(payload);'
        )
        composite_entry = (
            'void composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,\n'
            '               const void *payload, size_t, void *) {\n'
        )
        composite_anchor = composite_entry + '    auto &job = payload_job(payload);'
        foliage_entry = (
            'void foliage_seed(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,\n'
            '                  const void *payload, size_t, void *) {\n'
        )
        foliage_anchor = foliage_entry + '    FoliageSeedJob *job;'
        composite_draw = '    pass.SetBindGroup(0, bindings);\n    pass.Draw(3);'
        foliage_draw = '    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);'
        self.assertEqual(self.source.count(encode_anchor), 1)
        self.assertEqual(self.source.count(composite_anchor), 1)
        self.assertEqual(self.source.count(foliage_anchor), 1)
        self.assertEqual(self.transformed.count('profileAoEncode'), 1)
        self.assertEqual(self.transformed.count('Stats::aoEncode'), 1)
        self.assertEqual(self.transformed.count('profileAoCompositeRecord'), 1)
        self.assertEqual(self.transformed.count('Stats::aoCompositeRecord'), 1)
        self.assertEqual(self.transformed.count('profileAoFoliageSeedRecord'), 1)
        self.assertEqual(self.transformed.count('Stats::aoFoliageSeedRecord'), 1)

        timer = self.transformed.index('profileAoEncode')
        callback = self.transformed.index(encode_anchor)
        worker = self.transformed.index('init_worker(ctx.device);', callback)
        self.assertLess(callback, timer)
        self.assertLess(timer, worker)

        composite_entry_offset = self.transformed.index(composite_entry)
        composite_timer = self.transformed.index('profileAoCompositeRecord')
        composite_lookup = self.transformed.index('auto &job = payload_job(payload);', composite_entry_offset)
        # The draw itself is in draw_composite, which the composite calls.
        composite_draw_offset = self.transformed.index(composite_draw)
        self.assertLess(composite_entry_offset, composite_timer)
        self.assertLess(composite_timer, composite_lookup)
        self.assertLess(composite_draw_offset, composite_entry_offset)
        self.assertIn('draw_composite(ctx, pass, job.composite',
                      self.transformed[composite_lookup:self.transformed.index(foliage_entry)])

        foliage_entry_offset = self.transformed.index(foliage_entry)
        foliage_timer = self.transformed.index('profileAoFoliageSeedRecord')
        foliage_payload = self.transformed.index('std::memcpy(&job,payload,sizeof(job));', foliage_timer)
        foliage_draw_offset = self.transformed.index(foliage_draw, foliage_timer)
        self.assertLess(foliage_entry_offset, foliage_timer)
        self.assertLess(foliage_timer, foliage_payload)
        self.assertLess(foliage_payload, foliage_draw_offset)

        # The draw wrappers are unique and retain every production fullscreen
        # draw; only the two named callbacks acquire attribution timers.
        self.assertEqual(self.transformed.count('pass.Draw(3);'), self.source.count('pass.Draw(3);'))
        self.assertEqual(self.transformed.count(composite_draw), self.source.count(composite_draw))
        self.assertEqual(self.transformed.count(foliage_draw), self.source.count(foliage_draw))

        # Transforming the actual source must not mutate the checkout.
        self.assertEqual(self.source_path.read_text(encoding='utf-8'), self.source)

    def test_transform_fails_closed_for_missing_or_duplicate_anchor(self):
        broken = self.source.replace(
            '    auto &job = payload_job(payload);',
            '    auto &job = payload_job(payload /* missing profiling anchor */);',
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(broken)

        with self.assertRaises(ValueError):
            instrument_ao(self.transformed)

        composite_entry = (
            'void composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,\n'
            '               const void *payload, size_t, void *) {\n'
        )
        composite_anchor = composite_entry + '    auto &job = payload_job(payload);'
        foliage_entry = (
            'void foliage_seed(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,\n'
            '                  const void *payload, size_t, void *) {\n'
        )
        foliage_anchor = foliage_entry + '    FoliageSeedJob *job;'
        with self.assertRaises(ValueError):
            instrument_ao(self.source.replace(composite_entry, composite_entry.replace('void composite', 'void composite_missing'), 1))
        with self.assertRaises(ValueError):
            instrument_ao(self.source.replace(composite_anchor, composite_anchor + '\n' + composite_anchor, 1))
        with self.assertRaises(ValueError):
            instrument_ao(self.source.replace(foliage_entry, foliage_entry.replace('void foliage_seed', 'void foliage_seed_missing'), 1))
        with self.assertRaises(ValueError):
            instrument_ao(self.source.replace(foliage_anchor, foliage_anchor + '\n' + foliage_anchor, 1))

        missing_composite_draw = self.source.replace(
            '    pass.SetBindGroup(0, bindings);\n    pass.Draw(3);',
            '    pass.SetBindGroup(0, bindings);\n    pass.Draw(3 /* missing profiling anchor */);',
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(missing_composite_draw)

        duplicate_foliage = self.source.replace(
            '    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);',
            '    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);\n'
            '    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);',
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(duplicate_foliage)

    def test_header_aggregates_and_reports_nested_ao_duration(self):
        self.assertEqual(HEADER.count('aoEncode=0'), 1)
        self.assertIn('MP6_SUM(encode); MP6_SUM(aoEncode);', HEADER)
        self.assertIn('MP6_SUM(aoCompositeRecord); MP6_SUM(aoFoliageSeedRecord);', HEADER)
        self.assertIn('aoEncode=%.6f', HEADER)
        self.assertIn('aoCompositeRecord=%.6f', HEADER)
        self.assertIn('aoFoliageSeedRecord=%.6f', HEADER)
        self.assertIn('AO record CPU scopes cover the complete custom draw callbacks', HEADER)
        self.assertEqual(HEADER.count('[FRAME-AO-RECORD-SCOPE] callbacks=complete\\n'), 1)
        self.assertNotIn('[FRAME-AO-RECORD-SCOPE] callbacks=complete\\\\n', HEADER)

        # The AO attribution lives on the established profile line. The
        # metrics parser accepts it for new reports and old reports remain
        # valid without fabricating a zero value.
        profile_line = re.search(r'FRAME-PROFILE.*', HEADER)
        self.assertIsNotNone(profile_line)
        self.assertIn('aoEncode=', profile_line.group(0))
        self.assertIn('aoCompositeRecord=', profile_line.group(0))
        self.assertIn('aoFoliageSeedRecord=', profile_line.group(0))

    def test_metrics_accept_new_field_without_fabricating_old_data(self):
        common = (
            '[FRAME-WORK] passes=1 gxDraws=1 sourceVertices=1 indices=1 '
            'drawElements=1 copies=0 copyBytes=0\n'
            '[FRAME-PASS] index=0 label=unit-test\n'
        )
        old = (
            '[FRAME-PROFILE] frames=4000 frameAdmission=1 stagingAdmission=2 '
            'encode=3 acquire=4 finish=5 submit=6 present=7 queueDelayMax=8\n'
            + common
        )
        legacy = old.replace(
            'encode=3 ',
            'encode=3 aoEncode=0.5 aoCompositeRecord=0.2 aoFoliageSeedRecord=0.1 ',
        )
        old_result = frame_profile(old)
        legacy_result = frame_profile(legacy)
        self.assertNotIn('aoEncode', old_result['cpu_average_ms'])
        self.assertIsNone(old_result['ao_record_scope'])
        self.assertEqual(legacy_result['cpu_average_ms']['aoEncode'], 0.5)
        self.assertEqual(legacy_result['cpu_average_ms']['aoCompositeRecord'], 0.2)
        self.assertEqual(legacy_result['cpu_average_ms']['aoFoliageSeedRecord'], 0.1)
        self.assertEqual(legacy_result['ao_record_scope'], 'legacy draw-command recording only; nested inside encode; not GPU execution')
        self.assertIn('legacy draw-command recording only', legacy_result['scope'])
        self.assertNotIn('complete callback CPU recording', legacy_result['scope'])

        marked = legacy.replace('[FRAME-WORK]', '[FRAME-AO-RECORD-SCOPE] callbacks=complete\n[FRAME-WORK]')
        marked_result = frame_profile(marked)
        self.assertEqual(marked_result['ao_record_scope'], 'complete callback CPU recording (lookup/binding/setup/draw); nested inside encode; not GPU execution')
        self.assertIn('complete callback CPU recording (lookup/binding/setup/draw)', marked_result['scope'])
        self.assertIn('nested inside encode', marked_result['scope'])
        with self.assertRaises(ValueError):
            frame_profile(marked.replace('callbacks=complete', 'callbacks=draw-only'))
        with self.assertRaises(ValueError):
            frame_profile(marked + '[FRAME-AO-RECORD-SCOPE] callbacks=complete\n')
        with self.assertRaises(ValueError):
            frame_profile(marked.replace('aoFoliageSeedRecord=0.1 ', ''))

    def test_disabled_timer_path_does_not_sample_clock(self):
        # The shared Timer is intentionally reused: a disabled profile leaves
        # start at zero, so AO adds no clock read or accumulation when off.
        self.assertIn(
            'explicit Timer(uint64_t Stats::*f):field(f),start(current ? now() : 0) {}',
            HEADER,
        )
        self.assertIn('~Timer() { if(start && current)', HEADER)


if __name__ == '__main__':
    unittest.main()
