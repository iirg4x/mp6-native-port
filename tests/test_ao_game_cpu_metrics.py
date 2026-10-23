"""Strict optional game-thread AO replay timing schema."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'integration'))
from engine_benchmark_metrics import frame_profile

BASE = (
    '[FRAME-PROFILE] frames=4000 frameAdmission=0 stagingAdmission=0 '
    'encode=1 acquire=0 finish=0 submit=0 present=0 queueDelayMax=0\n'
    '[FRAME-WORK] passes=1 gxDraws=1 sourceVertices=1 indices=1 '
    'drawElements=1 copies=0 copyBytes=0\n[FRAME-PASS] scene\n'
)
ROW = ('[FRAME-AO-GAME] renderInclusive=0.4 renderExclusive=0.1 '
       'faceDrawInclusive=0.3 faceDrawExclusive=0.3 '
       'renderCalls=1 faceDrawCalls=8\n')


class AoGameCpuMetricsTests(unittest.TestCase):
    def test_absence_is_unmeasured(self):
        self.assertIsNone(frame_profile(BASE)['ao_game_average'])

    def test_nested_values_and_scope(self):
        result = frame_profile(BASE + ROW)
        self.assertEqual(result['ao_game_average']['faceDrawCalls'], 8)
        self.assertIn('Do not sum these', result['scope'])
        self.assertIn('nested AO scopes only', result['scope'])

    def test_invalid_records_fail_closed(self):
        for invalid in (
            ROW + ROW,
            ROW.replace('renderCalls=1 ', ''),
            ROW.replace('renderCalls=1', 'renderCalls=-1'),
            ROW.replace('renderCalls=1', 'renderCalls=nan'),
            ROW.replace('renderExclusive=0.1', 'renderExclusive=0.5'),
            ROW.replace('faceDrawInclusive=0.3', 'faceDrawInclusive=0.5'),
            ROW.replace('faceDrawExclusive=0.3', 'faceDrawExclusive=0.4'),
            ROW.replace('renderCalls=1', 'renderCalls=0'),
            ROW.replace('faceDrawCalls=8', 'faceDrawCalls=0'),
            ROW.rstrip() + ' garbage\n',
            ROW.replace('renderCalls=1', 'renderCalls=1 renderCalls=1'),
            ROW.replace('renderCalls=1', 'renderCalls=1 unknown=1'),
        ):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                frame_profile(BASE + invalid)


if __name__ == '__main__':
    unittest.main()
