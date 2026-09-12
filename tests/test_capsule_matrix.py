"""Keep the diagnostic matrix aligned with implemented capsule entry points."""
from pathlib import Path
import re
import unittest
from tools import build
from integration.run_capsule_matrix import CASES, SELF, SPACE, seed_precedes_turns

ROOT=Path(__file__).resolve().parents[1]

class CapsuleMatrixCoverage(unittest.TestCase):
    def test_mid_turn_state_is_not_an_audit_seed(self):
        log='[BOARD-QA] turn.begin tick=8875 turn=1\n[BOARD-QA] turn.begin tick=9315 turn=1\n'
        self.assertFalse(seed_precedes_turns(log,9600))
        self.assertFalse(seed_precedes_turns(log,8875))
        self.assertTrue(seed_precedes_turns(log,7000))
        self.assertFalse(seed_precedes_turns(log,None))

    def test_all_implemented_gameplay_handlers_have_a_case(self):
        source=(Path(build.DECOMP)/'src/board/capevent.c').read_text()
        table=source.split('static EVCAPSULEDATA ev_CapsuleData[] = {',1)[1].split('\n};',1)[0]
        handlers=re.findall(r'^\s*\{\s*(\w+),',table,re.M)
        active={i for i,name in enumerate(handlers[:50]) if name not in ('NULL','mbev_CapNull')}
        covered={capsule for _,capsule,_ in CASES}
        # The player controller, not the item dispatcher, enters handler 40
        # after Bullet Bill's actual dice roll. The runner checks both returns.
        self.assertIn(('bullet-move',4,'audit-bullet'),CASES)
        covered.add(40)
        self.assertEqual(active-covered,set())
        self.assertEqual(covered-active,{31}) # Passive flashlight through Boo.

    def test_placement_and_movement_are_not_conflated(self):
        self.assertEqual(len({name for name,_,_ in CASES}),len(CASES))
        self.assertEqual({i for _,i,r in CASES if r=='audit-place'},set(SPACE))
        self.assertEqual({i for _,i,r in CASES if r=='audit-use'},set(SELF))
        self.assertEqual({i for _,i,r in CASES if r=='audit-pass'},set(range(20,25)))
        self.assertIn(('land-owned-16',16,'audit-land-owned'),CASES)
        self.assertTrue(all(('pass-long-'+str(i),i,'audit-pass-long') in CASES for i in (21,23,24)))

if __name__=='__main__': unittest.main()
