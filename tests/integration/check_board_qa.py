"""Validate bounded QA evidence, not just a successful harness exit.

This is a state/log gate. A passing run still needs visual and gameplay review
for effects the instrumentation does not observe.
"""
import argparse
import collections
import json
import math
from pathlib import Path
import re


def validate(directory, required_events=(), min_turns=1, min_round=1, full_match=False,
             player_motions=False, scared_end=False, results=False):
    report = json.loads((directory / 'result.json').read_text())
    log = (directory / 'game.log').read_text(errors='replace')
    errors = []
    if report['reason'] != 'exited' or report['exit_code'] != 0:
        errors.append(f"process did not finish cleanly: {report['reason']} / {report['exit_code']}")
    if '[EVENT] qa.complete=done' not in log:
        errors.append('missing QA completion marker')
    if '[MP6-CRASH]' in log or '[FATAL]' in log or '[AURORA FATAL:' in log:
        errors.append('crash/fatal marker present')
    if '[TEST] input-script: unrecognized' in log or '[EVENT] script.timeout=' in log:
        errors.append('script input was unrecognized or an event wait timed out')
    events = collections.Counter()
    max_round = 0
    times = set()
    for number, line in enumerate(log.splitlines(), 1):
        match = re.match(r'\[BOARD-QA\] ([\w.-]+) tick=\d+ turn=(\d+) time=(\d+) ', line)
        if not match:
            continue
        events[match[1]] += 1
        max_round = max(max_round, int(match[2]))
        times.add(int(match[3]))
        vectors = re.findall(r'(?:pos|eye|center)=\(([^)]+)\)', line)
        if len(vectors) != 3:
            errors.append(f'line {number}: missing player/camera vector')
        for vector in vectors:
            try:
                components = [float(x) for x in vector.split(',')]
                valid = len(components) == 3 and all(math.isfinite(x) and abs(x) < 1e7
                                                     for x in components)
            except ValueError:
                valid = False
            if not valid:
                errors.append(f'line {number}: invalid/out-of-board vector {vector}')
    for event in required_events:
        if events[event] == 0:
            errors.append(f'missing event: {event}')
    if events['turn.end'] < min_turns:
        errors.append(f"only {events['turn.end']} completed turns; expected at least {min_turns}")
    if max_round < min_round:
        errors.append(f'only reached round {max_round}; expected at least {min_round}')
    if full_match:
        request = report.get('request', {})
        if (request.get('route') != 'cpu-soak' or request.get('stop_round') != 0 or
            request.get('capsule', -1) != -1 or request.get('load_at') is not None):
            errors.append('full-match gate requires fresh ordinary CPU turns, without round/capsule/state fixtures')
        completed = [(int(turn),int(player)) for turn,player in re.findall(
            r'\[BOARD-QA\] turn.end tick=\d+ turn=(\d+) time=\d+ p=(\d+)',log)]
        expected = [(turn,player) for turn in range(1,21) for player in range(4)]
        if completed != expected:
            errors.append(f'expected exactly all 80 player turns in order, got {len(completed)}')
        finish = re.search(r'\[BOARD-QA\] board.finished tick=\d+ turn=21 time=\d+ p=-1 value=20 ',log)
        boundary = re.search(r'\[RESULT-QA\] results.enter tick=\d+',log)
        if not finish or not boundary or finish.start() >= boundary.start():
            errors.append('missing ordered board finish and recovered results entry')
        if not events['last5.begin'] or not events['last5.end'] or times != {0,1}:
            errors.append('missing Last Five Turns or day/night coverage')
        if re.search(r'\[BOARD-QA\] turn.begin tick=\d+ turn=2[1-9] ',log):
            errors.append('played a turn after the final round')
        if 'FIXTURE.last5-transition' in log:
            errors.append('accelerated Last Five Turns fixture is not a full match')
    result_stages = re.findall(r'\[RESULT-QA\] (results\.\w+) tick=\d+',log)
    if results or full_match:
        expected_stages = ['results.enter', 'results.ceremony', 'results.ranking',
                           'results.graphs', 'results.saved', 'results.returned']
        if result_stages != expected_stages:
            errors.append(f'missing/out-of-order results ceremony and return: {result_stages}')
        if 'results.stub' in log or 'recovered results reached unavailable fallback' in log:
            errors.append('recovered results took the unavailable overlay route')
        finish = log.find('[BOARD-QA] board.finished ')
        entry = log.find('[RESULT-QA] results.enter ')
        if finish < 0 or entry < finish:
            errors.append('results were not reached through the board finish dispatcher')
    motions = collections.Counter()
    if player_motions or scared_end:
        one_shots = {4,5,7,8,9,10,11,12,13}
        for line in log.splitlines():
            m = re.match(r'\[BOARD-QA\] motion\.(start|wrap|end) .*?slot=(\d+) attr=([\da-f]+)', line)
            if not m:
                continue
            stage, slot, attr = m[1], int(m[2]), int(m[3], 16)
            motions[stage] += 1
            if slot == 9:
                motions['scared.'+stage] += 1
            if slot in one_shots and (stage == 'wrap' or (stage == 'start' and attr & 9)):
                errors.append('looping one-shot: '+line)
        if not motions['start'] or not motions['end']:
            errors.append('missing instrumented motion requests/end samples')
        if scared_end and not motions['scared.end']:
            errors.append('missing completed scared reaction')
    return dict(errors=errors, events=dict(events), max_round=max_round,
                times=sorted(times), motions=dict(motions), results=result_stages,
                sha256=report['sha256'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--event', action='append', default=[])
    parser.add_argument('--min-turns', type=int, default=1)
    parser.add_argument('--min-round', type=int, default=1)
    parser.add_argument('--full-match', action='store_true', help='require all 20 rounds through the recovered results ceremony and menu return')
    parser.add_argument('--results', action='store_true', help='require board finish, ceremony, rankings, graphs, save and return (allows final-round fixture)')
    parser.add_argument('--player-motions', action='store_true', help='reject looping stock one-shots using live playback probes')
    parser.add_argument('--scared-end', action='store_true', help='also require a scared reaction to reach its end')
    args = parser.parse_args()
    evidence = validate(args.directory, args.event, args.min_turns, args.min_round, args.full_match,
                         args.player_motions, args.scared_end, args.results)
    print(json.dumps(evidence, indent=2))
    raise SystemExit(bool(evidence['errors']))
