"""Runtime integration: requires a freshly built release-headless binary and local disc cache."""
import concurrent.futures
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def run(module):
    with tempfile.TemporaryDirectory(prefix='overlay-return-', dir=root / 'build') as run_dir:
        env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
        env.update(MP6_LAUNCHER='0', MP6_TICK_HZ='0', MP6_TEST_LOAD_DLL=module,
                   MP6_BOOT_TO='mdsel',
                   MP6_DISC_ROOT=str(root / 'build/disc-cache/orig/GP6E01'))
        log = root / 'build' / f'overlay-return-{module}.log'
        with log.open('w') as out:
            result = subprocess.run([str(root / 'build/release-headless/mp6native_headless.exe'),
                                     '10000000', '--input-script',
                                     'period:30;timeout:9900000;pressuntil:start/ovl.start/mdseldll'],
                                    cwd=run_dir, env=env,
                                    stdout=out, stderr=subprocess.STDOUT, timeout=30)
        text = log.read_text(errors='replace')
        lines = [s for s in text.splitlines()
                 if '[STUB]' in s or 'DLLSTUB-TEST' in s or
                 'overlay.skipped' in s or 'ovl.start' in s]
        print(module, 'exit', result.returncode, '\n' + '\n'.join(lines))
        assert result.returncode == 0, str(log)
        match = re.search(r'\[EVENT\] overlay.skipped=.*?num=(\d+).*?\n', text)
        assert match, 'No return event: ' + str(log)
        assert re.search(r'\[EVENT\] ovl.start=.*?num=' + match[1] + r'\b',
                         text[match.end():]), 'No destination started: ' + str(log)
        return module

with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    print('Verified:', list(pool.map(run, ['m601dll', 'instdll', 'mgmfreedll'])))
