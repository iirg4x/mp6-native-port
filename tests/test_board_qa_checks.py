"""A successful board process must not hide a renderer or allocator failure."""
import json
from pathlib import Path
import tempfile
import unittest

from tests.integration.check_board_qa import validate, validate_history


class BoardEvidenceChecks(unittest.TestCase):
    def history_log(self):
        lines = []
        for turn in range(1, 21):
            lines.append(f'[BOARD-QA] round.complete tick=100 turn={turn} time=0')
            for player in range(4):
                lines.append(f'[BOARD-QA] player={player} space=1 coin={turn + player} '
                             f'star={turn // 5 + player} diceMode=0')
        for mode in range(2):
            for player in range(4):
                values = [0] + [turn + player if mode == 0 else turn // 5 + player
                                for turn in range(1, 21)]
                if mode:
                    values.append(values[-1] + 2)  # ceremony bonus isn't a board round
                lines.append(f'[RESULT-HISTORY] mode={mode} player={player} values=' +
                             ','.join(map(str, values)))
        return '\n'.join(lines)

    def test_history_matches_actual_round_totals_and_keeps_bonus_separate(self):
        self.assertEqual(validate_history(self.history_log()), [])

    def test_history_rejects_missing_and_corrupted_points(self):
        log = self.history_log()
        self.assertTrue(validate_history(''))
        self.assertTrue(validate_history(log.replace('round.complete', 'turn.end')))
        self.assertTrue(validate_history(log.replace('mode=1', 'mode=2')))
        # Old empty history, shifted/truncated history, and a single wrong point.
        marker = '[RESULT-HISTORY] mode=0 player=0 values='
        for values in (','.join(['0'] * 21), '0,1,2',
                       '0,999,' + ','.join(str(t) for t in range(2, 21))):
            lines = log.splitlines()
            lines = [marker + values if line.startswith(marker) else line for line in lines]
            self.assertTrue(validate_history('\n'.join(lines)))

    def check_log(self, extra):
        with tempfile.TemporaryDirectory(prefix='board-evidence-') as tmp:
            root = Path(tmp)
            (root / 'result.json').write_text(json.dumps({
                'reason': 'exited', 'exit_code': 0, 'sha256': 'test'}))
            (root / 'game.log').write_text(
                '[BOARD-QA] turn.end tick=100 turn=1 time=0 p=0 value=0 '
                'pos=(0,0,0) eye=(1,1,1) center=(0,0,0)\n'
                '[EVENT] qa.complete=done\n' + extra)
            return validate(root)['errors']

    def test_accepts_clean_run_and_exact_empty_card_miss(self):
        self.assertEqual(self.check_log(''), [])
        self.assertEqual(self.check_log(
            '[AURORA ERROR: aurora::card] Failed to open file: MARIPA6'), [])

    def test_rejects_render_and_allocator_errors_after_exit_zero(self):
        for message in (
                'Validation Error: texture usage conflict',
                '[AURORA ERROR: aurora::gpu] invalid render pass',
                '[CRASH] failed', 'memory free error',
                '[AURORA ERROR: aurora::card] Failed to open file: MARIPA6-backup'):
            with self.subTest(message=message):
                self.assertTrue(any(message in error for error in self.check_log(message)))


if __name__ == '__main__':
    unittest.main()
