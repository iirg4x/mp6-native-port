"""Static, no-game tests for the deterministic AO Party-menu harness."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tests.integration.build_widescreen_qa import (
    ao_scope_probe,
    layer_scope_probe,
    mode_select_probe,
)
from tests.integration.run_widescreen_scenes import (
    PARTY_MENU_SCRIPT,
    ao_execution_evidence,
    ao_trace_errors,
    fi_capture_errors,
    capture_ticks,
    deterministic_capture_errors,
    parse_capture_rect,
    validate_capture_trigger,
    verified_executable,
)


class AoMenuQaTests(unittest.TestCase):
    def test_requested_implementation_must_actually_execute(self):
        off = '[MP6-AO] encoded=0 composited=0 foliage=0 xegtao=0'
        legacy = '[MP6-AO] encoded=10 composited=10 foliage=3 xegtao=0'
        upstream = '[MP6-AO] encoded=10 composited=10 foliage=3 xegtao=10'
        self.assertFalse(ao_execution_evidence(off, 0, 'legacy', True)[1])
        self.assertFalse(ao_execution_evidence(legacy, 2, 'legacy', True)[1])
        self.assertFalse(ao_execution_evidence(upstream, 2, 'xegtao', True)[1])
        self.assertFalse(ao_execution_evidence(off, 2, 'xegtao', False)[1]) # artwork-only route
        for log, level, implementation in (('', 2, 'xegtao'), (legacy, 2, 'xegtao'),
                                            (off, 2, 'legacy'), (upstream, 0, 'xegtao'),
                                            (upstream, 2, 'legacy')):
            self.assertTrue(ao_execution_evidence(log, level, implementation, True)[1])

    def test_party_script_starts_at_mode_select(self):
        self.assertIn('qa.mdsel.choice/party', PARTY_MENU_SCRIPT)
        self.assertIn('qa.party.stage/intro', PARTY_MENU_SCRIPT)
        self.assertLess(PARTY_MENU_SCRIPT.index('qa.mdsel.choice/party'),
                        PARTY_MENU_SCRIPT.index('qa.party.stage/intro'))

    def test_mode_select_probe_observes_party_without_replacing_call(self):
        source = '''void f(void) {\n            omOvlCall(DLL_mdpartydll, 0, 0);\n}\n'''
        result = mode_select_probe(source)
        self.assertIn('qa.mdsel.choice', result)
        self.assertIn('omOvlCall(DLL_mdpartydll, 0, 0);', result)

    def test_layer_probe_distinguishes_hooks_from_depth_clears(self):
        source = '''void e(void) {\n                hookFunc(layer);\n}\nvoid Hu3DLayerHookReset(s16 layerNo)\n{\n}\nvoid Hu3DZClearLayerSet(s16 layerNo)\n{\n}\nvoid z(void) {\n    mp6_ao_depth_cleared(Hu3DCameraNo);\n}\n'''
        result = layer_scope_probe(source)
        self.assertIn('"prehook"', result)
        self.assertIn('qa.ao.layer.clear', result)
        self.assertIn('"depth-clear"', result)
        self.assertIn('qa.ao.layer.install', result)
        self.assertIn('qa.ao.layer.reset', result)

    def test_ao_probe_records_saved_view_and_epoch_count(self):
        source = '''void mp6_ao_camera_snapshot_reset(int camera)\n{\n}\nvoid e(int camera, int hasDepth, void *view) {\n    if (!hasDepth) {\n    }\n    if (!view) {\n    }\n    mp6_ao_apply(view);\n}\n'''
        source = '#include "mp6_enhancements.h"\n' + source
        result = ao_scope_probe(source)
        self.assertIn('qa_ao_scope_epoch_reset(camera);', result)
        self.assertIn('"apply"', result)
        self.assertIn('view->viewport', result)
        self.assertIn('epoch=%u', result)

    def test_capture_ticks_ignore_wall_clock(self):
        with tempfile.TemporaryDirectory() as temp:
            index = Path(temp)/'index.csv'
            index.write_text(
                'seq,present_index,tick,wall_ns\n'
                '0,3,700,999999\n'
                '1,4,850,1000000\n'
                '2,5,1000,1000001\n',
                encoding='utf-8')
            self.assertEqual(capture_ticks(temp), [700, 850, 1000])
        self.assertEqual(deterministic_capture_errors([700, 850, 1000], 3, 150), [])
        self.assertTrue(deterministic_capture_errors([700, 851], 2, 150))

    def test_fi_capture_allows_duplicate_ticks_but_requires_replay_when_requested(self):
        errors, replay_count = fi_capture_errors([700, 700, 701], [0, 1, 1], 3, True)
        self.assertEqual(errors, [])
        self.assertEqual(replay_count, 2)
        errors, replay_count = fi_capture_errors([700, 699], [0, 1], 2, False)
        self.assertTrue(errors)
        self.assertEqual(replay_count, 1)
        errors, replay_count = fi_capture_errors([700, 700], [0, 0], 2, True)
        self.assertTrue(errors)
        self.assertEqual(replay_count, 0)

    def test_capture_rect_and_trigger_are_strictly_validated(self):
        self.assertEqual(parse_capture_rect('0,2,640,480'), (0, 2, 640, 480))
        with self.assertRaises(ValueError):
            parse_capture_rect('0,0,0,480')
        self.assertEqual(validate_capture_trigger('qa.party.stage=board'),
                         'qa.party.stage=board')
        with self.assertRaises(ValueError):
            validate_capture_trigger('qa.party.stage=board;anything')

    def test_ao_trace_rejects_duplicate_epoch_apply(self):
        log = (
            '[AO-QA-TRACE] tick=100 camera=0 fov=45.0 vp=0,0,1,1 '
            'reason=apply count=1 epoch=7\n'
            '[AO-QA-TRACE] tick=101 camera=0 fov=45.0 vp=0,0,1,1 '
            'reason=apply count=2 epoch=7\n'
        )
        rows, errors = ao_trace_errors(log)
        self.assertEqual(len(rows), 2)
        self.assertTrue(errors)

    def test_ao_trace_requires_native_board_and_menu_clear_layers(self):
        log = (
            '[EVENT] qa.ao.layer.clear=depth-clear num=2 tick=10 seq=1\n'
            '[EVENT] qa.ao.layer.clear=depth-clear num=7 tick=11 seq=2\n'
        )
        rows, errors = ao_trace_errors(log, require_layers=True)
        self.assertEqual(rows, [])
        self.assertEqual(errors, [])

    def test_provenance_hash_is_required_for_nondefault_executable(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp)/'mp6native.exe'
            exe.write_bytes(b'qa-frozen-binary')
            digest = hashlib.sha256(exe.read_bytes()).hexdigest()
            provenance = Path(temp)/'ready.json'
            provenance.write_text(json.dumps({'exe': digest}), encoding='utf-8')
            record = verified_executable(exe, provenance)
            self.assertEqual(record['sha256'], digest)
            self.assertEqual(record['provenance'], str(provenance.resolve()))


if __name__ == '__main__':
    unittest.main()
