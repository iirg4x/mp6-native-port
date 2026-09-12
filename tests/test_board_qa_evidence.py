import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('check_board_qa',
    Path(__file__).parent / 'integration/check_board_qa.py')
qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qa)


class BoardQaEvidence(unittest.TestCase):
    def run_case(self, line, reason='exited', code=0, completion=True):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'result.json').write_text(json.dumps(
                dict(reason=reason, exit_code=code, sha256='test-binary')))
            (directory / 'game.log').write_text(line +
                ('\n[EVENT] qa.complete=done' if completion else ''))
            return qa.validate(directory, ['slide.end'], min_turns=0)

    def test_requires_event_and_clean_completion(self):
        line = ('[BOARD-QA] slide.end tick=500 turn=1 time=0 p=0 '
                'pos=(1,2,3) eye=(4,5,6) center=(7,8,9)')
        self.assertFalse(self.run_case(line)['errors'])
        self.assertTrue(self.run_case(line.replace('slide.end', 'heartbeat'))['errors'])
        self.assertTrue(self.run_case(line, completion=False)['errors'])
        self.assertTrue(self.run_case(line, reason='wall-clock limit')['errors'])
        self.assertTrue(self.run_case(line, code=1)['errors'])
        self.assertTrue(self.run_case(line+'\n[TEST] input-script: unrecognized button name')['errors'])
        self.assertTrue(self.run_case(line+'\n[EVENT] script.timeout=qa.complete')['errors'])

    def test_rejects_nonfinite_and_unbounded_camera(self):
        for value in ['nan', 'inf', '-inf', '1.96e27', 'broken']:
            line = (f'[BOARD-QA] slide.end tick=500 turn=1 time=0 p=0 '
                    f'pos=(1,2,3) eye=(4,{value},6) center=(7,8,9)')
            self.assertTrue(self.run_case(line)['errors'], value)

    def test_hyphenated_fixture_names_are_checked_not_silently_skipped(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory/'result.json').write_text(json.dumps(
                dict(reason='exited', exit_code=0, sha256='test')))
            fixture = ('[BOARD-QA] FIXTURE.star-purchase tick=500 turn=1 time=0 p=0 '
                       'pos=(1,2,3) eye=(4,5,6) center=(7,8,9)')
            for line, valid in ((fixture, True),
                                (fixture.replace('star-purchase','star-position'), False),
                                (fixture.replace('eye=(4,5,6)','eye=(4,nan,6)'), False)):
                (directory/'game.log').write_text(line+'\n[EVENT] qa.complete=done')
                evidence=qa.validate(directory, ['FIXTURE.star-purchase'], min_turns=0)
                self.assertEqual(not evidence['errors'],valid,evidence)

    def test_full_match_requires_every_turn_and_results_boundary(self):
        def state(event,turn,player,value=-1):
            return (f'[BOARD-QA] {event} tick=500 turn={turn} time={turn%2} p={player} '
                    f'value={value} pos=(1,2,3) eye=(4,5,6) center=(7,8,9)')
        completed=[state('turn.end',t,p) for t in range(1,21) for p in range(4)]
        ending=[state('last5.begin',16,0),state('last5.end',16,0),
                state('board.finished',21,-1,20),
                 *[f'[RESULT-QA] results.{stage} tick=501'
                   for stage in ('enter','ceremony','ranking','graphs','saved','returned')],
                '[EVENT] qa.complete=done']
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            (directory/'result.json').write_text(json.dumps(dict(reason='exited',exit_code=0,
                sha256='test',request=dict(route='cpu-soak',stop_round=0,capsule=-1))))
            for lines,valid in [(completed+ending,True),
                                (completed[1:]+ending,False),
                                (completed+ending[:-2]+ending[-1:],False),
                                (completed+[state('turn.begin',21,0)]+ending,False),
                                (completed+[completed[-1]]+ending,False)]:
                (directory/'game.log').write_text('\n'.join(lines))
                evidence=qa.validate(directory,full_match=True)
                self.assertEqual(not evidence['errors'],valid,evidence)

    def test_results_rejects_stub_incomplete_and_unordered_ceremony(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            (directory/'result.json').write_text(json.dumps(
                dict(reason='exited',exit_code=0,sha256='test')))
            finish='[BOARD-QA] board.finished tick=500 turn=21 time=0 p=-1 value=20 pos=(1,2,3) eye=(4,5,6) center=(7,8,9)'
            stages=[f'[RESULT-QA] results.{stage} tick=501'
                    for stage in ('enter','ceremony','ranking','graphs','saved','returned')]
            for lines, valid in [
                ([finish]+stages,True),
                ([finish]+stages[:-1],False),
                ([finish]+stages[::-1],False),
                ([finish,'[BOARD-QA] results.stub tick=501 completed_rounds=20'],False),
                (stages,False),
                ([stages[0],finish]+stages[1:],False),
            ]:
                (directory/'game.log').write_text('\n'.join(lines+['[EVENT] qa.complete=done']))
                evidence=qa.validate(directory,min_turns=0,min_round=0,results=True)
                self.assertEqual(not evidence['errors'],valid,evidence)

    def test_motion_gate_rejects_replays_and_missing_probes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory/'result.json').write_text(json.dumps(
                dict(reason='exited', exit_code=0, sha256='test')))
            start = '[BOARD-QA] motion.start tick=10 p=0 char=0 slot=9 attr=00000000 serial=1'
            end = '[BOARD-QA] motion.end tick=54 p=0 slot=9 attr=00000000 serial=1 at=44 end=44'
            wrap = '[BOARD-QA] motion.wrap tick=54 p=0 slot=9 attr=00000001 serial=1 before=43 after=0 end=44'
            for lines, valid in [([start,end], True),
                                 ([start.replace('attr=00000000','attr=40000001'),end], False),
                                 ([start,end,wrap], False),
                                 ([start], False), ([],False),
                                 ([start,end,wrap.replace('slot=9','slot=6')],True)]:
                (directory/'game.log').write_text('\n'.join(lines+['[EVENT] qa.complete=done']))
                evidence = qa.validate(directory,min_turns=0,min_round=0,
                                       player_motions=True,scared_end=True)
                self.assertEqual(not evidence['errors'],valid,evidence)
