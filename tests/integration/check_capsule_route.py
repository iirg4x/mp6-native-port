"""Play the fixture's real Red Mushroom menu/effect route, including GPU draws."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
unfixed = '--unfixed' in sys.argv
directory = ROOT / ('build/capsule-route-unfixed' if unfixed else 'build/capsule-route')
exe = directory / 'release/mp6native.exe'
log = directory / 'red-mushroom.log'
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='mdparty', MP6_AUTO_START_TICKS='60,150',
           MP6_TICK_HZ='0', MP6_WINDOW_SIZE='1024x768',
           MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'))
with tempfile.TemporaryDirectory(prefix='capsule-play-', dir=ROOT / 'build') as temporary:
    with log.open('w', encoding='utf-8') as output:
        process = subprocess.Popen(
            [str(exe), '60000', '--input-script',
             'period:30;timeout:59000;pressuntil:a/capsule.used/red-mushroom;wait:600'],
            cwd=temporary, env=env, stdout=output, stderr=subprocess.STDOUT)
        print(f'Capsule test PID: {process.pid}', flush=True)
        try:
            result = process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
            raise
text = log.read_text(errors='replace')
print('\n'.join(line for line in text.splitlines()
                if 'CAPSULE-TEST' in line or 'capsule.used' in line or
                '[MP6-CRASH]' in line or 'tick budget' in line))
if unfixed:
    assert result != 0, f'Negative control did not crash: {log}'
    assert '0x0000000400000000' in text and 'ev_CapEffDraw' in text, str(log)
    assert '[CAPSULE-TEST] Red Mushroom returned;' not in text, str(log)
    print('Unfixed negative control reproduced the reported capsule draw crash: PASS')
    raise SystemExit(0)
assert result == 0, f'Game exited with {result}: {log}'
assert '[CAPSULE-TEST] Red Mushroom returned; diceMode=1 remaining=-1' in text, str(log)
assert re.search(r'\[EVENT\] capsule.used=', text), str(log)
assert '[MP6-CRASH]' not in text, str(log)
print('Red Mushroom consumed, double-dice mode armed, 600 ticks without a crash: PASS (not a complete board-turn test)')
