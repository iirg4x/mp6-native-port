"""Measure actual bus-sign vertices around restarts, not screenshot guesses."""
import argparse
import json
from pathlib import Path
import re

PATTERN = re.compile(
    r'\[BUS-EXIT\] tick=(\d+) phase=([\d.-]+) timer=([\d.-]+) '
    r'k=([\d.-]+) left=([\d.-]+) right=([\d.-]+) width=([\d.-]+)')


def analyze(text):
    rows = [dict(zip(('tick', 'phase', 'timer', 'k', 'left', 'right', 'width'),
                     map(float, match.groups()))) for match in PATTERN.finditer(text)]
    restarts, errors, velocities = [], [], []
    for previous, current in zip(rows, rows[1:]):
        if current['phase'] + 1 < previous['phase']:
            visible = previous['right'] >= 0 and previous['left'] <= previous['width']
            restart = dict(tick=int(current['tick']), phase=previous['phase'],
                           left=previous['left'], right=previous['right'],
                           width=previous['width'], visible=visible)
            restarts.append(restart)
            if visible:
                errors.append('restart while sign intersects viewport: ' + repr(restart))
            if previous['phase'] < 1078:
                errors.append('restart before authored motion completes: ' + repr(restart))
        delta = current['phase'] - previous['phase']
        if (0 < delta < 1.1 and current['phase'] < 1078 and
                abs(current['k'] - previous['k']) < 1e-6):
            if abs(delta * current['k'] - 1) > 0.001:
                errors.append('motion speed no longer matches native clock')
            # Restrict to visible travel, not holds, jumps or viewport changes.
            distance = (current['left'] + current['right'] -
                        previous['left'] - previous['right']) * 0.5
            if (previous['right'] > 0 and previous['left'] < previous['width'] and
                    abs(distance) > 0.01 and abs(distance) < 10):
                velocities.append(abs(distance))
    if not rows:
        errors.append('no actual-vertex measurements')
    if not restarts:
        errors.append('no complete restart observed')
    velocities.sort()
    return dict(samples=len(rows), restarts=restarts, errors=errors,
                median_pixels_per_tick=velocities[len(velocities)//2] if velocities else None,
                passed=not errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    result = analyze(args.log.read_text(errors='replace'))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['passed'] else 1)
