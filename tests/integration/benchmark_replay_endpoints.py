"""Alternating CPU A/B against an explicitly supplied pre-change builder.

The baseline is a frozen fi_build_replay function, not another executable or
machine. This measures replay preparation, never whole-game or Android FPS.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT/'tests')]
from test_frame_replay_batch import replay_test_source
from tools import build


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    source = replay_test_source()
    start = source.index('static uint32_t fi_build_replay_reference(')
    end = source.index('void mp6_fi_savestate_reset(void)', start)
    baseline = args.baseline.read_text().replace('fi_build_replay(', 'fi_build_replay_reference(')
    source = source[:start]+baseline+'\n'+source[end:]
    (args.out/'replay-under-test.inc').write_text(source)
    exe = args.out/'benchmark.exe'
    subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', '-DMP6_BENCHMARK',
        '-DMP6_BENCHMARK_ENDPOINTS', '-ffunction-sections', '-fdata-sections',
        '-Wl,--gc-sections', '-I'+str(args.out.resolve()), '-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'src/host'), '-I'+str(ROOT/'build/android-aurora-source/include'),
        str(ROOT/'tests/native/frame_replay_batch_selftest.c'), '-o', str(exe.resolve())],
        check=True)
    result = subprocess.run([str(exe.resolve())], capture_output=True, text=True, timeout=180)
    (args.out/'results.txt').write_text(result.stdout+result.stderr)
    result.check_returncode()
    rows = re.findall(r'WINDOW trial=(\d+) replays=(\d+) reference_us=([\d.]+) candidate_us=([\d.]+)',
                      result.stdout)
    assert len(rows) == 160
    summary = {}
    for count in (1, 2, 4, 8):
        subset = [r for r in rows if int(r[1]) == count]
        before = statistics.median(float(r[2]) for r in subset)
        after = statistics.median(float(r[3]) for r in subset)
        summary[count] = dict(reference_us=before, candidate_us=after,
                              reduction_percent=100*(1-after/before), trials=len(subset))
    (args.out/'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
