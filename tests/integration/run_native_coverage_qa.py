"""Finite, isolated native-loader smoke checks; not a gameplay/visual audit."""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags

NEW_MODULES = (
    'w04dll', 'w02dll', 'w05dll', 'w06dll', 'w10dll', 'w11dll',
    's01dll', 's02dll', 's03dll', 'mdmicdll', 'mdsingdll',
    'mgmbattledll', 'mgmbingodll', 'mgmdecathlondll', 'actmandll',
    'endingdll', 'meschkdll', 'micquizseldll', 'miraclebookdll',
    'motchkdll', 'openingdll', 'optiondll', 'resultdll', 'safdll',
    'sequencedll', 'staffdll',
)
ERROR_MARKERS = (
    '[FATAL]', '[CRASH]', '[MP6-CRASH]', '[AURORA ERROR', 'Validation Error',
    'memory free error', 'unmapped vtx attr',
    'GXEnd: vertex data not evenly divisible', 'unrecognized module id',
    'minigame-stub',
    'Error: Create Motion Over!', 'Error: Create Model Over!', 'Error: ObjMax Over!',
    'Error: M657ComPlayerSet()',
)


def run_module(module, output, executable, timeout):
    directory = output / module
    directory.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('MP6_')}
    env.update(
        MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel',
        MP6_TEST_LOAD_DLL=module, MP6_TEST_LOAD_DLL_HOLD='1',
        MP6_TICK_HZ='0', MP6_ENH_HEAP_SCALE='4', MP6_WINDOW_SIZE='640x480',
        MP6_VSYNC='0', MP6_UNLOCKED_FPS='0', MP6_ENH_AA='0',
        MP6_ENH_WIDESCREEN='0', MP6_WIDESCREEN='0',
        MP6_ENH_AMBIENT_OCCLUSION='0',
    )
    isolate_background(env, executable)
    timed_out = False
    with (directory / 'game.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(
            [str(executable), '1000000', '--input-script',
             'period:20;timeout:100000;pressuntil:start/ovl.start/mdseldll;wait:100000'],
            cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
            creationflags=background_process_flags())
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            code = process.wait(timeout=10)
    text = (directory / 'game.log').read_text(encoding='utf-8', errors='replace')
    lowered = text.lower()
    # This fresh directory deliberately has no card save. Failure to open the
    # absent MARIPA6 file is the file-selector's normal first-use path.
    errors = list(dict.fromkeys(line for line in text.splitlines()
                               if any(marker in line for marker in ERROR_MARKERS)
                               and '[AURORA ERROR: aurora::card] Failed to open file: MARIPA6'
                               not in line))
    bound = f'oslink: bound {module} prolog/epilog' in lowered
    entered = f'[event] ovl.start={module}' in lowered
    exited = '[EVENT] qa.coverage.exit=requested' in text
    prolog = f'objdll> dll/{module}.rel prolog end' in lowered
    boot = ('oslink: bound bootdll prolog/epilog' in lowered and
            'objdll> dll/bootdll.rel prolog end' in lowered)
    result = dict(module=module, exit_code=code, timed_out=timed_out,
                  native_bound=bound, entered=entered, prolog_finished=prolog,
                  boot_verified=boot,
                  exit_requested=exited, error_count=len(errors), errors=errors[:40],
                  passed=code == 0 and not timed_out and bound and entered
                  and prolog and not errors)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n',
                                           encoding='utf-8')
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', nargs='+')
    parser.add_argument('--all', action='store_true', help='all 89; boot is verified on every launch')
    parser.add_argument('--timeout', type=float, default=45)
    args = parser.parse_args()
    if args.all:
        if args.modules:
            parser.error('--all and --modules are mutually exclusive')
        from tests.test_native_inventory import FAMILIES
        # Boot performs one-shot system initialization. Verify its real entry
        # on every run rather than forcing an unsupported second boot.
        args.modules = sorted(name + 'dll' for name in set().union(*FAMILIES.values())
                              if name != 'boot')
    elif args.modules is None:
        args.modules = NEW_MODULES
    if not 0 < args.timeout <= 60:
        parser.error('--timeout must be in (0,60] seconds')
    executable = ROOT / 'build/native-coverage-qa/release/mp6native.exe'
    if not executable.is_file():
        parser.error('Build tests.integration.build_native_coverage_qa first')
    output = ROOT / 'build/native-coverage-qa/runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output.mkdir(parents=True)
    (output / 'manifest.json').write_text(json.dumps(dict(
        executable=str(executable), sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        executable_mtime=executable.stat().st_mtime, modules=args.modules,
        scope='native entry smoke, not full gameplay or visual audit',
        widescreen=False, ao=False,
    ), indent=2) + '\n', encoding='utf-8')
    print('Evidence:', output, flush=True)
    results = [run_module(module.lower(), output, executable, args.timeout)
               for module in args.modules]
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n',
                                       encoding='utf-8')
    passed = all(row['passed'] for row in results)
    if args.all:
        passed = passed and len(results) == 88 and all(row['boot_verified'] for row in results)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
