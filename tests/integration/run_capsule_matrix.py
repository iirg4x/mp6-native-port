"""Bounded capsule-entry audit; log success is not a visual pass.

Each case starts an isolated board/card and enters the actual effect dispatcher.
Placement is tested separately from activation. Never overwrites an older run.
"""
import argparse
import json
import re
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from check_board_qa import validate

SELF = tuple(range(8))
SPACE = (10, 11, 12, 13, 15, 16, 17, 20, 21, 22, 23, 24, 25)
CASES = ([(f'use-{i:02}', i, 'audit-use') for i in SELF] +
         [(f'place-{i:02}', i, 'audit-place') for i in SPACE] +
         [(f'land-{i:02}', i, 'audit-land') for i in SPACE if i not in range(20,25)] +
         [(f'pass-{i:02}', i, 'audit-pass') for i in range(20,25)] +
         [('boo', 46, 'audit-boo'), ('boo-light', 31, 'audit-boo'),
          ('boo-house', 46, 'audit-boo-house')] +
         [(f'special-{i}', i, 'audit-special') for i in (41, 42, 43, 44)] +
         [(f'pass-long-{i}', i, 'audit-pass-long') for i in (21,23,24)] +
         [('land-owned-16',16,'audit-land-owned'),('bullet-move',4,'audit-bullet')])

def seed_precedes_turns(log, save_at):
    turns=[int(t) for t in re.findall(r'\[BOARD-QA\] turn\.begin tick=(\d+)',log)]
    return isinstance(save_at,int) and save_at>0 and not any(t<=save_at for t in turns)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--only', help='comma-separated case names')
    parser.add_argument('--seconds', type=int, default=100)
    parser.add_argument('--ao', default='0', choices=['0','1','2'])
    parser.add_argument('--window-size', default='960x540')
    parser.add_argument('--frames', type=int, default=24)
    parser.add_argument('--jobs', type=int, choices=(1,2), default=1)
    parser.add_argument('--cache-seed',help='optional prior GPU cache inside build')
    parser.add_argument('--seed-state',help='same-build day-board quick state before the first turn; Boo starts fresh')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+',args.name): parser.error('simple run name required')
    chosen = set(args.only.split(',')) if args.only else {c[0] for c in CASES}
    if chosen - {c[0] for c in CASES}: parser.error('unknown case')
    if args.seed_state:
        seed=(ROOT/args.seed_state).resolve(strict=True)
        if not seed.is_relative_to((ROOT/'build/board-qa-runs').resolve()):
            parser.error('seed must be an isolated board QA state')
        try:
            seed_report=json.loads((seed.parent/'result.json').read_text())
            seed_log=(seed.parent/'game.log').read_text(errors='replace')
            safe=seed_precedes_turns(seed_log,seed_report['request']['save_at'])
        except (OSError,ValueError,KeyError): safe=False
        if not safe:
            parser.error('seed was not captured before the first turn; use a fresh run or earlier QA state')
    out = ROOT/'build/board-qa-runs'/args.name
    out.mkdir(exist_ok=False)
    reports = []
    def run_case(case):
        name,capsule,route=case
        run_name = f'{args.name}-{name}'
        trigger = 'placement.hit.begin' if route=='audit-place' else (
                  'boo.fade.begin' if route=='audit-boo-house' else 'capsule.effect')
        command = [sys.executable, 'tests/integration/run_board_qa.py', '--name', run_name,
                   '--route', route, '--capsule', str(capsule), '--seconds', str(args.seconds),
                   '--widescreen', '--window-size', args.window_size, '--ao', args.ao,
                   '--capture-frames', str(args.frames), '--capture-stride', '12',
                   '--capture-trigger', trigger]
        if args.cache_seed: command += ['--cache-seed',args.cache_seed]
        if args.seed_state and not route.startswith('audit-boo'):
            command += ['--load-at','180','--state-path',args.seed_state]
        subprocess.run(command, cwd=ROOT, check=True)
        run = ROOT/'build/board-qa-runs'/run_name
        events = ['audit.begin','audit.end']
        if route=='audit-place': events += ['placement.throw.end','placement.hit.end']
        else: events += ['capsule.begin','capsule.return']
        if route=='audit-boo-house': events += ['boo.fade.begin','boo.fade.end']
        evidence = validate(run, events, min_turns=int(route.startswith('audit-pass') or route=='audit-bullet'))
        log = (run/'game.log').read_text(errors='replace')
        if route!='audit-place':
            expected=46 if route.startswith('audit-boo') else capsule
            for stage in ('begin','return'):
                if not re.search(r'\[BOARD-QA\] capsule\.'+stage+r' .*?value='+str(expected)+r' ',log):
                    evidence['errors'].append(f'missing capsule {expected} {stage}')
        if route.startswith('audit-boo') and not re.search(r'\[BOARD-QA\] audit.begin .*?time=1 ',log):
            evidence['errors'].append('Boo needs real night assets, not the daytime refusal')
        if route=='audit-bullet':
            for stage in ('begin','return'):
                if not re.search(r'\[BOARD-QA\] capsule\.'+stage+r' .*?value=40 ',log):
                    evidence['errors'].append(f'Bullet Bill movement handler did not {stage}')
        if route=='audit-land-owned':
            owners=re.findall(r'ownership\.(before|after) space=(\d+) owner=(\d+)',log)
            if len(owners)!=2 or owners[0][1]!=owners[1][1] or owners[0][2]==owners[1][2]:
                evidence['errors'].append('Kamek did not transfer the owned space')
        captures=len(list((run/'frames').glob('*.mfd')))
        if args.frames and not captures:
            evidence['errors'].append('requested effect capture produced no frames')
        report=dict(case=name, capsule=capsule, route=route, evidence=evidence,
                            captures=captures,
                            visual_review='pending', minigame_stub='[MINIGAME]' in log)
        print(f'{name}: {"FAIL" if evidence["errors"] else "state/return PASS; visual pending"}',flush=True)
        return report
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures=[pool.submit(run_case,case) for case in CASES if case[0] in chosen]
        for future in as_completed(futures):
            reports.append(future.result())
            (out/'matrix.json').write_text(json.dumps(reports,indent=2)+'\n')
    return int(any(r['evidence']['errors'] for r in reports))

if __name__ == '__main__':
    raise SystemExit(main())
