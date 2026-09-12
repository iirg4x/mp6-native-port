"""Run the standing leak gate in a fresh, isolated production board session."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--name', required=True)
parser.add_argument('--duration', type=int, default=300)
parser.add_argument('--warmup', type=int, default=60)
parser.add_argument('--audio-stress', action='store_true',
                    help='Opt-in real stream/SFX play-stop stress alongside the board')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
    parser.error('simple run name required')
if args.warmup < 60 or args.duration < args.warmup + 60:
    parser.error('at least 60 seconds each of warmup and sampling are required')
directory = ROOT / 'build/board-qa-runs' / args.name
directory.mkdir(parents=True, exist_ok=False)
exe = ROOT / 'build/release/mp6native.exe'
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='mdparty', MP6_AUTO_START_TICKS='60,150',
           MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
           MP6_WINDOW_SIZE='1920x1080', MP6_ENH_WIDESCREEN='1',
           MP6_WIDESCREEN='1', MP6_FREE_ASPECT='1', MP6_ENH_AA='2',
           MP6_ENH_AMBIENT_OCCLUSION='2', MP6_ENH_SHADOW_QUALITY='1',
           MP6_GPU_CACHE_PATH=str(directory / 'gpu-cache'), MP6_TICK_HZ='0',
           MP6_UNLOCKED_FPS='0', MP6_VSYNC='0')
if args.audio_stress:
    env.update(MP6_AUDIO_LEAKTEST_STREAM='0', MP6_AUDIO_LEAKTEST_SE='24')
# Free-running input reaches the stationary board introduction well before the
# 60-second warmup ends. Nothing here is compiled into the production binary.
command = [sys.executable, str(ROOT / 'tools/leakgate.py'), str(exe),
           '--duration', str(args.duration), '--warmup', str(args.warmup), '--threshold-kb-min', '500',
           '--csv', str(directory / 'memory.csv'),
           '--capture-stdout', str(directory / 'game.log'),
           '--lockfile', str(ROOT.parent / '.visual_test.lock'),
           '--input-script',
           'period:30;timeout:10000000;pressuntil:a/w01.live/120;wait:10000000']
with (directory / 'gate.log').open('w') as output:
    result = subprocess.run(command, cwd=directory, env=env,
                            stdout=output, stderr=subprocess.STDOUT)
with exe.open('rb') as binary:
    digest = hashlib.file_digest(binary, 'sha256').hexdigest()
(directory / 'result.json').write_text(json.dumps(dict(
    command=command, sha256=digest, environment={k: v for k, v in env.items() if k.startswith('MP6_')},
    exit_code=result.returncode), indent=2)+'\n')
print((directory / 'gate.log').read_text(), end='')
raise SystemExit(result.returncode)
