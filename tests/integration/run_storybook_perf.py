"""Native, hidden menu throughput. Compare identical settings and resolution."""
from pathlib import Path
import os
import math
import re
import subprocess
import sys
import json

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags

_PERF_FIELDS = {
    'update_ms': float,
    'render_ms': float,
    'frame_ms': float,
    'batches': float,
    'rml_renders': int,
    'geometry_builds': int,
    'geometry_reuses': int,
    'text_generations': int,
    'text_cache': int,
    'render_calls': int,
}
_PERF_RE = re.compile(
    r'^\[STORYBOOK-PERF\]\s+(?P<width>\d+)x(?P<height>\d+)\s+'
    r'(?P<mode>\w+)\s+(?P<fields>.*)$')


def parse_perf_row(line):
    match = _PERF_RE.match(line)
    assert match, f'Unparseable STORYBOOK-PERF row: {line}'
    fields = dict(token.split('=', 1) for token in match.group('fields').split()
                  if '=' in token)
    row = {
        'width': int(match.group('width')),
        'height': int(match.group('height')),
        'mode': match.group('mode'),
        'render_scope': fields.pop('render_scope', None),
        'frame_scope': fields.pop('frame_scope', None),
        'raw': line,
    }
    for key, converter in _PERF_FIELDS.items():
        assert key in fields, f'{row["width"]}x{row["height"]} {row["mode"]}: missing {key}'
        value = converter(fields.pop(key))
        if isinstance(value, float):
            assert math.isfinite(value), f'{key} is not finite: {line}'
        assert value >= 0, f'{key} is negative: {line}'
        row[key] = value
    assert not fields, f'Unknown STORYBOOK-PERF fields: {sorted(fields)}'
    assert row['render_scope'] == 'explicit_offscreen', line
    assert row['frame_scope'] == 'explicit+aurora_end_frame', line
    assert row['rml_renders'] == 2, line
    return row


def main():
    name = sys.argv[1]
    assert name.replace('-', '').replace('_', '').isalnum()
    exe = ROOT / 'build/settings-qa-perf/release/mp6native.exe'
    output = ROOT / 'build/settings-qa-perf' / name
    output.mkdir(exist_ok=False)
    (exe.parent / 'mp6_config.json').write_text(json.dumps({'video.vsync': False}))
    env = {k:v for k,v in os.environ.items() if not k.startswith('MP6_')}
    env.update(MP6_LAUNCHER='1', MP6_DISC_ROOT=str(ROOT/'build/disc-cache/orig/GP6E01'),
               MP6_GPU_CACHE_PATH=str(ROOT/'build/settings-qa-perf/gpu-cache'), MP6_WINDOW_SIZE='1280x720')
    isolate_background(env, exe)
    with (output/'run.log').open('w') as log:
        result = subprocess.run([str(exe)], cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=90, creationflags=background_process_flags())
    log = (output/'run.log').read_text(errors='replace')
    raw_rows = [line for line in log.splitlines() if '[STORYBOOK-PERF]' in line]
    rows = [parse_perf_row(line) for line in raw_rows]
    print('\n'.join(raw_rows))
    expected = {(width, height, mode)
                for width, height in ((1280, 720), (2560, 1440))
                for mode in ('animated', 'turning', 'static')}
    actual = {(row['width'], row['height'], row['mode']) for row in rows}
    assert result.returncode == 0 and actual == expected and len(rows) == len(expected) \
        and 'Validation Error' not in log, log[-5000:]
    static_rows = [row for row in rows if row['mode'] == 'static']
    assert all(row['geometry_reuses'] > 0 for row in static_rows), rows
    assert all(row['render_calls'] >= 2 * 40 for row in rows), rows
    (output/'metrics.json').write_text(json.dumps(rows, indent=2) + '\n')
    from PIL import Image
    for path in output.glob('*.ppm'):
        Image.open(path).save(path.with_suffix('.png'))

if __name__ == '__main__':
    main()
