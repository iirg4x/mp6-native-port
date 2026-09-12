"""Open an isolated settings UI for manual/live-toggle QA; never writes user config."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
name = sys.argv[1]
assert re.fullmatch(r'[A-Za-z0-9_-]+', name)
directory = ROOT / 'build/ao-ui-qa' / name
directory.mkdir(parents=True, exist_ok=False)
source = ROOT / 'build/release'
for file in [source / 'mp6native.exe', *source.glob('*.dll')]:
    shutil.copy2(file, directory / file.name)
shutil.copytree(source / 'res', directory / 'res')
config = {
    'launcher.skip': False, 'video.window_mode': 'windowed', 'video.vsync': False,
    'video.show_fps': True, 'game.tick_hz': 60, 'enhancements.preset': 'vanilla',
    'enhancements.ambient_occlusion': 0,
    'game.content_root': str(ROOT / 'build/disc-cache/orig/GP6E01'),
}
(directory / 'mp6_config.json').write_text(json.dumps(config, indent=2))
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
env.update(MP6_LAUNCHER='1', MP6_WINDOW_SIZE='1280x960', MP6_AO_DIAG='1',
           MP6_GPU_CACHE_PATH=str(directory / 'gpu-cache'),
           MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'))
with (directory / 'game.log').open('w') as log:
    proc = subprocess.Popen([str(directory / 'mp6native.exe')], cwd=directory,
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    print(f'Isolated AO UI: PID={proc.pid}; directory={directory}', flush=True)
    try:
        code = proc.wait(timeout=300)
    except subprocess.TimeoutExpired:
        proc.terminate()
        code = proc.wait(timeout=10)
print('UI exit:', code, flush=True)
(directory / 'result.json').write_text(json.dumps(dict(exit_code=code,
    sha256=hashlib.sha256((directory / 'mp6native.exe').read_bytes()).hexdigest()), indent=2))
