"""Regression gate for one-shot reaction call sites, including numeric flags."""
import unittest
from tools import audit_player_motions as audit


class PlayerMotionAudit(unittest.TestCase):
    def test_all_known_stock_one_shots_request_non_looping_playback(self):
        records = audit.inventory()
        self.assertGreater(sum(r['slot'] in audit.ONE_SHOTS for r in records), 90)
        self.assertGreater(sum(r['slot'] == 9 for r in records), 10)
        self.assertEqual([r for r in records if r['bad_one_shot_loop']], [])
        before = [r for r in audit.inventory(False) if r['bad_one_shot_loop']]
        # CapSpecial's recovered owner now fixes slot 9 upstream as well.
        self.assertEqual(before, [])

    def test_parser_handles_numeric_flags_nested_calls_and_named_slots(self):
        source = '''
        enum { CAP_JUMP = 4, CAP_IDLE = 1, };
        void mbPlayerMotionSet(int playerNo, int slot, u32 attr);
        /* mbPlayerMotionSet(0, 9, HU3D_MOTATTR_LOOP); */
        mbPlayerMotionShiftSet(find(0,1), CAP_JUMP, 0, 8, 0x40000001);
        mbPlayerMotionSet(0, CAP_IDLE, HU3D_MOTATTR_LOOP);
        mbev_CapPlayerMotShiftWait(0, 9, HU3D_MOTATTR_SHIFT_LOOP, TRUE);
        mbPlayerMotionSet(0, dynamicSlot, attr);
        '''
        calls = list(audit.calls(source))
        self.assertEqual(len(calls), 4)
        self.assertEqual([r['bad_one_shot_loop'] for r in calls], [True, False, True, False])
        self.assertIsNone(calls[-1]['slot'])


if __name__ == '__main__':
    unittest.main()
