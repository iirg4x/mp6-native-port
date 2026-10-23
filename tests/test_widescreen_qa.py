"""Capture scheduling must not silently lose later-camera coverage."""
import unittest
import json
from pathlib import Path
import subprocess
import tempfile
from tools import build
from tools import audit_widescreen
from tests.integration.verify_widescreen_mesh import verify
from tests.integration.build_widescreen_qa import MINIGAME_SETUP, PARTY_MENU_ENTRIES, party_menu_probe, RESIZE_DRIVER, resize_probe
from tests.integration.run_widescreen_scenes import capture_budget, returned_from_scene, resize_errors, verified_card_seed, card_hashes
from tests.integration.run_title_qa import errors_in_log
from tests.integration.run_widescreen_scenes import exited_to_scene

ROOT = Path(__file__).resolve().parents[1]


class WidescreenCaptureSchedule(unittest.TestCase):
    def test_bus_exit_measurement_rejects_visible_or_incomplete_restart(self):
        from tests.integration.analyze_bus_motion import analyze
        def row(tick, phase, left, right):
            return (f'[BUS-EXIT] tick={tick} phase={phase} timer=0 '
                    f'k=1.75 left={left} right={right} width=1120\n')
        good = row(1, 1079, 1160, 1200) + row(2, 0.571429, -80, -40)
        self.assertTrue(analyze(good)['passed'])
        for bad in (row(1, 617, 500, 540), row(1, 1079, 1100, 1140)):
            self.assertFalse(analyze(bad + row(2, 0.571429, -80, -40))['passed'])
        self.assertFalse(analyze('')['passed'])

    def test_board_exit_requires_ordered_requested_transition(self):
        start = '[EVENT] ovl.start=w03dll num=125\n'
        request = '[EVENT] qa.scene.exit=requested num=0\n'
        kill = '[EVENT] ovl.kill=- num=1\n'
        results = '[EVENT] ovl.start=mdpresultdll num=92\n'
        self.assertTrue(exited_to_scene(start + request + kill + results, 'w03dll', 'mdpresultdll'))
        for log in (start + results, request + kill + results,
                    start + kill + results, start + request + results,
                    start + request + kill + '[EVENT] ovl.start=mdseldll num=93\n'):
            self.assertFalse(exited_to_scene(log, 'w03dll', 'mdpresultdll'))

    def test_inventory_includes_every_compiled_rel_family(self):
        compiled = {source for source, _ in build.native_rel_sources()}
        audited = {row['source'] for row in audit_widescreen.inventory()
                   if row['source'].startswith('src/REL/')}
        self.assertEqual(audited, compiled)
        for family in ('mgmfreedll', 'mgmrenshodll', 'instDll'):
            self.assertTrue(any(f'/REL/{family}/' in source for source in audited), family)

    def test_card_seed_is_an_attested_isolated_copy_only(self):
        parent = ROOT/'build/filesel-qa-runs'
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='seed-contract-', dir=parent) as tmp:
            seed = Path(tmp)
            (seed/'saves').mkdir()
            card = seed/'saves/test-card'
            card.write_bytes(b'test fixture')
            result = {'passed': True, 'cards_after': card_hashes(seed/'saves')}
            (seed/'result.json').write_text(json.dumps(result))
            self.assertEqual(verified_card_seed(seed), seed/'saves')
            card.write_bytes(b'changed')
            with self.assertRaises(ValueError): verified_card_seed(seed)
            result['cards_after'] = card_hashes(seed/'saves'); result['passed'] = False
            (seed/'result.json').write_text(json.dumps(result))
            with self.assertRaises(ValueError): verified_card_seed(seed)
        with self.assertRaises(ValueError): verified_card_seed(ROOT)

    def test_resize_probe_is_frame_boundary_only_and_gate_requires_every_state(self):
        original = (ROOT/'src/gx/aurora_bridge.c').read_text()
        probed = resize_probe(original)
        restored = probed.removesuffix(RESIZE_DRIVER).replace(
            'static void qa_resize_tick(void);\n\n', '').replace('    qa_resize_tick();\n', '')
        self.assertEqual(restored, original)
        self.assertIn('SDL_WINDOW_HIDDEN', RESIZE_DRIVER)
        self.assertNotIn('SDL_ShowWindow', RESIZE_DRIVER)
        self.assertNotIn('SDL_RaiseWindow', RESIZE_DRIVER)
        rows = [f'[WIDE-RESIZE] settled step={i} tick={600+i*120} window={w}x480 '
                f'wide={wide} logical={w if wide else 640} hidden=1' for i,(w,wide) in
                enumerate(zip((640,848,1120,1120,1120,848,640,640,848),(0,1,1,0,1,1,1,0,1)))]
        self.assertEqual(resize_errors('\n'.join(rows)), [])
        for bad in ('\n'.join(rows[:-1]), '\n'.join(rows[::-1]),
                    '\n'.join(rows).replace('hidden=1','hidden=0'),
                    '\n'.join(rows).replace('logical=1120','logical=640')):
            self.assertTrue(resize_errors(bad))

    def test_party_menu_probe_only_adds_observation(self):
        source = '\n'.join(declaration+'\n{\n    original();\n}\n'
                           for declaration, _ in PARTY_MENU_ENTRIES)
        observed = party_menu_probe(source)
        self.assertEqual(observed.count('mp6_event_post('), len(PARTY_MENU_ENTRIES))
        observed = observed.removeprefix('#include "mp6_events.h"\n')
        for _, stage in PARTY_MENU_ENTRIES:
            observed = observed.replace(
                f'\n    mp6_event_post("qa.party.stage", 0, "{stage}");', '')
        self.assertEqual(observed, source)
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            party_menu_probe('')
        renamed = source.replace('arg0', 'returningFromPreviousStep')
        self.assertEqual(party_menu_probe(renamed).count('mp6_event_post('), len(PARTY_MENU_ENTRIES))
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            party_menu_probe(source.replace('s16 arg0', 's32 arg0'))

    def test_inventory_typedef_signatures_and_framebuffer_wrappers(self):
        source = '''HUWINID HuWinCreate(float x, float y, int w, int h);
size_t mp6_widescreen_savestate_blob_size(void) { return 0; }
HUWINID HuWinCreate(float x, float y, int w, int h) {
    Hu3DFbCopyExec(0,0,640,480,fmt,1,image);
    Hu3DFbCopyLayerSet(1,0,0,640,480,fmt,1,image);
    return HuWinCreateF(x,y,w,h);
}
auto setting = [](bool value) { mp6_widescreen_set_enabled(value); };
'''
        rows=list(audit_widescreen.calls(source))
        self.assertEqual([r['api'] for r in rows], ['Hu3DFbCopyExec',
            'Hu3DFbCopyLayerSet','HuWinCreateF','mp6_widescreen_set_enabled'])
        self.assertEqual([r['function'] for r in rows[:3]], ['HuWinCreate']*3)

    def test_inventory_tracks_call_owner_and_excludes_prototypes_and_strings(self):
        source='''void HuWinPosSet(int id, float x, float y);
void HuWinPosSet(int id, float x, float y) {
    puts("HuWinPosSet(1,2,3); }");
    C_MTXOrtho(m, 0, 480, 0, width, 0, 1);
}
static void scene(void) {
    HuWinPosSet(id, offset + nested(2,3), 400);
    mp6_widescreen_extend_sides(id, "object(with parentheses)", 1);
}
'''
        rows=list(audit_widescreen.calls(source))
        self.assertEqual([r['api'] for r in rows],
                         ['C_MTXOrtho','HuWinPosSet','mp6_widescreen_extend_sides'])
        self.assertEqual([r['function'] for r in rows],['HuWinPosSet','scene','scene'])
        self.assertEqual(rows[-1]['args'],'id, "object(with parentheses)", 1')
        self.assertEqual(rows[1]['args'],'id, offset + nested(2,3), 400')

    def test_minigame_return_requires_ordered_real_lifecycle(self):
        start = '[EVENT] ovl.start=m633dll num=38 tick=100 seq=1\n'
        returning = '[EVENT] ovl.return=0 num=0 tick=400 seq=2\n'
        menu = '[EVENT] ovl.start=mdseldll num=80 tick=420 seq=3\n'
        self.assertTrue(returned_from_scene(start+returning+menu, 'm633dll'))
        for log in (returning+menu+start, start+returning, start+menu,
                    start+returning+start, menu, ''):
            self.assertFalse(returned_from_scene(log, 'm633dll'))
        self.assertFalse(returned_from_scene(start+'[EVENT] minigame.unavailable=board\n'+returning+menu, 'm633dll'))

    def test_fresh_title_run_exempts_only_expected_missing_save(self):
        missing = '[AURORA ERROR: aurora::card] Failed to open file: MARIPA6'
        self.assertEqual(errors_in_log(missing), [])
        for error in ('Validation Error', '[AURORA ERROR: aurora::gpu] bad texture',
                      missing+'-backup', '[CRASH]', 'memory free error'):
            self.assertEqual(errors_in_log(missing+'\n'+error), [error])

    def test_port_inventory_covers_offscreen_coordinate_adapters(self):
        rows = {row['source']: row for row in audit_widescreen.port_inventory()}
        self.assertIn('src/gx/aurora_bridge.c', rows)
        ao = rows['src/hsf/mp6_ambient_occlusion.c']
        self.assertTrue({'GXSetViewportRender', 'GXSetScissorRender'} <= {c['api'] for c in ao['calls']})
        calls = list(audit_widescreen.calls('/* GXSetScissor(1,2,3,4) */\nGXSetViewportJitter(0,0,w,h,0,1,f);'))
        self.assertEqual([c['api'] for c in calls], ['GXSetViewportJitter'])

    def test_mesh_census_rejects_native_changes_and_truncated_strips(self):
        prefix = '[WIDE-MESH] wide-ovl40-model0/green face='
        native = prefix+'0 type=2 mat=9 c0=v0(1,2,3)/uv0(0,0)\n'
        with tempfile.TemporaryDirectory(prefix='wide-census-', dir=ROOT/'build') as tmp:
            before, after = Path(tmp)/'before.log', Path(tmp)/'after.log'
            before.write_text(native)
            after.write_text(native+prefix+'1 type=3 mat=9 c0=v0(1,2,3)/uv0(0,0)\n')
            self.assertIn('1 added QUADs', verify(before, after, 'green', 1))
            after.write_text(native.replace('1,2,3', '9,2,3'))
            with self.assertRaisesRegex(ValueError, 'native face 0 changed'):
                verify(before, after, 'green')
            before.write_text(native.replace('type=2', 'type=4'))
            with self.assertRaisesRegex(ValueError, 'strip tails'):
                verify(before, after, 'green')
            before.write_text(native+native.replace('model0', 'model2'))
            after.write_text(before.read_text())
            with self.assertRaisesRegex(ValueError, 'ambiguous duplicate'):
                verify(before, after, 'green')
            self.assertIn('1 native faces identical', verify(before, after, 'green', model='wide-ovl40-model2'))

    def test_direct_scene_launch_has_valid_groups_and_character_preloads(self):
        with tempfile.TemporaryDirectory(prefix='wide-setup-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'setup_subject.inc').write_text(MINIGAME_SETUP)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                '-I'+str(directory), str(ROOT/'tests/native/widescreen_setup_selftest.c'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_engine_stride_limits_and_budget(self):
        for stride in (0, -1, 1001, 1800):
            with self.assertRaisesRegex(ValueError, 'stride 1..1000'):
                capture_budget(0, stride, 2)
        self.assertEqual(capture_budget(500, 450, 20), 9110)
        self.assertEqual(capture_budget(0, 1000, 2), 1060)
        self.assertEqual(capture_budget(0, 1, 1, True), 900)
        for args in ((-1, 1, 1), (0, 1, 0), (100000, 1, 1), (0, 1000, 101)):
            with self.assertRaises(ValueError):
                capture_budget(*args)


if __name__ == '__main__':
    unittest.main()
