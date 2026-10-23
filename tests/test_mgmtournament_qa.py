"""Tournament QA harness contracts are bounded and source-backed."""
import re
import subprocess
import sys
import unittest
from pathlib import Path

from tests.integration.build_mgmtournament_qa import (
    TOURNAMENT_INSTRUMENTERS,
    instrument_instruction,
    instrument_tournament_characters,
    instrument_tournament_scene,
    instrument_tournament_window,
)
from tools import apply_patches, build
from tests.integration.run_mgmtournament_qa import (
    SCRIPTS,
    expected_events,
    validate_log,
)


ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / 'build' / 'deps' / 'marioparty6' / 'src' / 'REL'


def collected_unit_paths():
    """Return source-directory/file pairs from the real build census."""
    return {
        (Path(source).parent.name, Path(source).name)
        for source, _flags, _object, _flavor in build.collect_units(headless=True)
    }


def patched(module, unit):
    """Return one real decomp source after the checked-in compat patch."""
    decomp = Path(apply_patches.require_pinned_decomp())
    rel = f'src/REL/{module}/{unit}.c'
    source = (decomp / rel).read_text(encoding='utf-8')
    patch = ROOT / 'compat' / 'decomp' / (rel + '.patch')
    return apply_patches.apply_unified_diff(
        source, patch.read_text(encoding='utf-8'), rel)


def brace_balance(source):
    """Count C braces while ignoring comments and string/char literals."""
    balance = 0
    state = 'code'
    escaped = False
    index = 0
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ''
        if state == 'code':
            if char == '/' and next_char == '/':
                state = 'line_comment'
                index += 2
                continue
            if char == '/' and next_char == '*':
                state = 'block_comment'
                index += 2
                continue
            if char == '"':
                state = 'string'
                escaped = False
            elif char == "'":
                state = 'char'
                escaped = False
            elif char == '{':
                balance += 1
            elif char == '}':
                balance -= 1
            index += 1
            continue
        if state == 'line_comment':
            if char in '\r\n':
                state = 'code'
            index += 1
            continue
        if state == 'block_comment':
            if char == '*' and next_char == '/':
                state = 'code'
                index += 2
            else:
                index += 1
            continue
        if escaped:
            escaped = False
        elif char == '\\':
            escaped = True
        elif (state == 'string' and char == '"') or (state == 'char' and char == "'"):
            state = 'code'
        index += 1
    return balance


class TournamentQaTests(unittest.TestCase):
    def test_builder_transforms_actual_lifecycle_sources(self):
        characters = patched('mgmtournamentdll', 'characters')
        scene = patched('mgmtournamentdll', 'scene')
        window = patched('mgmtournamentdll', 'window')
        paths = collected_unit_paths()
        self.assertIn(('instDll', 'setup.c'), paths)
        self.assertNotIn(('instdll', 'setup.c'), paths)
        self.assertIn(('instDll', 'setup.c'), TOURNAMENT_INSTRUMENTERS)
        setup_sources = [
            Path(source)
            for source, _flags, _object, _flavor in
            build.collect_units(headless=True)
            if Path(source).parent.name == 'instDll' and
            Path(source).name == 'setup.c'
        ]
        self.assertEqual(len(setup_sources), 1)
        instruction = setup_sources[0].read_text(encoding='utf-8')

        observed = instrument_tournament_characters(characters)
        self.assertIn('qa.mgmtournament.enter', observed)
        self.assertIn('qa.mgmtournament.epilog', observed)
        self.assertEqual(brace_balance(observed), 0)

        observed = instrument_tournament_scene(scene)
        for marker in ('qa.mgmtournament.fixture', 'qa.mgmtournament.state',
                       'qa.mgmtournament.route', 'qa.mgmtournament.coin'):
            self.assertIn(marker, observed)
        self.assertIn('DLL_m662dll', observed)
        self.assertNotIn('DLL_m602dll', observed)
        self.assertIn('GwPlayerConf[qaPlayer].type', observed)
        self.assertIn('(qaPlayer == 0) ? 0 : 1', observed)
        self.assertEqual(brace_balance(observed), 0)

        window_observed = instrument_tournament_window(window)
        self.assertIn('qa.mgmtournament.prompt', window_observed)
        self.assertIn('qa.mgmtournament.choice', window_observed)
        self.assertIn('qa.mgmtournament.back', window_observed)
        self.assertEqual(window.count('HuWinChoiceGet('),
                         window_observed.count('HuWinChoiceGet('))
        self.assertEqual(window.count('HuWinExClose(lbl_1_bss_C8.windowId);'),
                         window_observed.count('HuWinExClose(lbl_1_bss_C8.windowId);'))
        self.assertEqual(window.count('return result;'),
                         window_observed.count('return result;'))
        self.assertEqual(brace_balance(window_observed), 0)

        observed = instrument_instruction(instruction)
        for marker in ('qa.mgmtournament.inst', 'qa.mgmtournament.inst_start'):
            self.assertIn(marker, observed)
        self.assertEqual(brace_balance(observed), 0)

        builder = (ROOT / 'tests' / 'integration' /
                   'build_mgmtournament_qa.py').read_text()
        self.assertNotIn('instrument_duel_prolog', builder)
        self.assertNotIn('MP6_QA_MGMTOURNAMENT_RETURN', builder)
        self.assertNotIn('m602dll', builder)

    def test_builder_rejects_changed_source_anchors(self):
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            instrument_tournament_characters('')
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            instrument_tournament_scene('')
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            instrument_tournament_window('')
        with self.assertRaisesRegex(RuntimeError, 'QA anchor changed'):
            instrument_instruction('')

    def test_scripts_use_real_overlay_markers_and_bounded_steps(self):
        for name, script in SCRIPTS.items():
            with self.subTest(name=name):
                self.assertIn('period:20;timeout:100000;', script)
                self.assertIn('ovl.start/mgmtournamentdll', script)
                self.assertIn('qa.mgmtournament.state/6', script)
                self.assertIn('qa.mgmtournament.complete/returned', script)
                self.assertLessEqual(script.count('stick:left;'), 80)
        self.assertIn('ovl.start/instdll', SCRIPTS['round'])
        self.assertIn('ovl.start/m662dll', SCRIPTS['round'])
        self.assertIn('pressuntil:a/qa.mgmtournament.prompt/round',
                      SCRIPTS['round'])
        self.assertIn('wait:180;stick:right;', SCRIPTS['round'])
        self.assertIn('press:a;', SCRIPTS['round'])
        self.assertIn('waitev:ovl.return;', SCRIPTS['round'])
        self.assertNotIn('waitev:ovl.return/-', SCRIPTS['round'])
        self.assertNotIn('m602dll', SCRIPTS['round'])
        self.assertNotIn('game_return', SCRIPTS['round'])
        self.assertIn("default='qa.mgmtournament.state=6'", (
            ROOT / 'tests' / 'integration' /
            'run_mgmtournament_qa.py').read_text())
        self.assertIn("default=240", (
            ROOT / 'tests' / 'integration' /
            'run_mgmtournament_qa.py').read_text())
        runner = (ROOT / 'tests' / 'integration' /
                  'run_mgmtournament_qa.py').read_text()
        self.assertIn("MP6_QA_MGMTOURNAMENT_UNLOCK='1'", runner)
        self.assertNotIn('MP6_QA_MGMTOURNAMENT_RETURN', runner)

    def test_validator_requires_native_lifecycle_and_frames(self):
        clean = '\n'.join(
            f'[EVENT] {key}={value} '
            f'num={10 if key == "qa.mgmtournament.coin" else 0} tick={i}'
            for i, (key, value) in enumerate(expected_events('round')))
        clean += '\n' + '\n'.join(
            f'[EVENT] qa.mgmtournament.coin=mgCoinBonus num=0 tick={100 + i}'
            for i in range(3))
        self.assertTrue(validate_log(clean, 'round', 6, 6)['passed'])
        zero_reward = clean.replace('coin=mgCoinBonus num=10',
                                    'coin=mgCoinBonus num=0')
        self.assertFalse(validate_log(zero_reward, 'round', 6, 6)['passed'])
        self.assertFalse(validate_log(clean, 'round', 5, 6)['passed'])
        without_return = '\n'.join(
            line for line in clean.splitlines()
            if 'ovl.return=-' not in line)
        self.assertFalse(validate_log(without_return, 'round', 6, 6)['passed'])
        self.assertTrue(validate_log(clean + '\n[AURORA ERROR] bad texture',
                                     'round', 6, 6)['errors'])

    def test_help_is_python_only_and_exposes_bounded_scenarios(self):
        runner = ROOT / 'tests' / 'integration' / 'run_mgmtournament_qa.py'
        result = subprocess.run(
            [sys.executable, str(runner), '--help'],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('{all,lifecycle,reload,round}', result.stdout)
        self.assertIn('--capture-count', result.stdout)
        self.assertIn('--timeout', result.stdout)


if __name__ == '__main__':
    unittest.main()
