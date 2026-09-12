"""Bounded ADB profiling of MP6 only, with an isolated device content/save root.

Never clears application data, logcat, or device caches. Device snapshots stay
under build/ and are not release assets. Normal-speed and Fast Forward workloads
are explicitly distinguished; neither changes the player's saved settings.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
ADB = ROOT/'build/android-sdk/platform-tools/adb.exe'
PACKAGE = 'com.mp6.game'
USER_ROOT = '/sdcard/Android/data/com.mp6.game/files/mp6'
DEVICE_ROOT = '/data/user/0/com.mp6.game/files/perf-20260910'
OUTPUT = ROOT/'build/tablet-performance-20260910'


def adb(serial, *args, timeout=30, check=True):
    return subprocess.run([str(ADB), '-s', serial, *args], capture_output=True,
                          timeout=timeout, check=check)


def shell(serial, *args, **kwargs):
    return adb(serial, 'shell', shlex.join(args), **kwargs).stdout.decode(errors='replace').strip()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')


def session_paths(serial, model, session):
    if not re.fullmatch(r'[A-Za-z0-9_-]+', session):
        raise ValueError('simple session name required')
    if session == 'tablet-performance-20260910':
        if (model, serial) != ('SM-X920', 'R52XA02MKDF'):
            raise ValueError('existing tablet session belongs to another device')
        device_root = '/data/user/0/com.mp6.game/files/perf-20260910'
    else:
        device_root = '/data/user/0/com.mp6.game/files/perf-'+session
    output = ROOT/'build'/session
    identity = dict(serial=serial, model=model, device_root=device_root)
    owner = output/'session-owner.json'
    if owner.exists() and json.loads(owner.read_text()) != identity:
        raise ValueError('profiling session belongs to another device')
    return output, device_root, identity


def thermal_status(report):
    match = re.search(r'^Thermal Status: (\d+)\s*$', report, re.MULTILINE)
    if not match:
        raise ValueError('missing thermal status; do not start an unmonitored run')
    return int(match[1])


def temperature_limit(value):
    try:
        limit = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('temperature limit must be a number in 0..80 C') from error
    if not math.isfinite(limit) or not 0 <= limit <= 80:
        raise argparse.ArgumentTypeError('temperature limit must be finite and in 0..80 C')
    return limit


def verify_start_thermal(report, max_status, limits):
    if thermal_status(report) > max_status:
        raise RuntimeError('let the phone cool to the selected thermal cohort before starting')
    for name, limit in limits.items():
        if limit is None:
            continue
        values = re.findall(r'Temperature\{mValue=([^,]+), mType=\d+, mName='+re.escape(name)+r',', report)
        try:
            values = [float(value) for value in values]
        except ValueError as error:
            raise RuntimeError('invalid temperature for required sensor: '+name) from error
        if not values or any(not math.isfinite(value) for value in values):
            raise RuntimeError('missing or invalid temperature for required sensor: '+name)
        # Cached and HAL readings can briefly differ; admit only when both fit.
        if max(values) > limit:
            raise RuntimeError(f'let {name} cool to <= {limit:g} C before starting (observed {max(values):g} C)')


def mp6_is_foreground(serial):
    activities = shell(serial, 'dumpsys', 'activity', 'activities')
    resumed = [line for line in activities.splitlines()
               if 'mResumedActivity:' in line or 'topResumedActivity=' in line]
    return len(resumed) == 1 and PACKAGE+'/' in resumed[0]


def verify_composite_probe(output, installed_hash):
    probe = json.loads((output/'probe/provenance.json').read_text())
    wrapper = json.loads((output/'apk-probe/provenance.json').read_text())
    expected_native = (output/'probe/stripped/libmp6game.so').resolve()
    if (probe.get('release_asset') is not False or wrapper.get('release_asset') is not False
            or wrapper.get('sha256') != installed_hash
            or Path(wrapper.get('native_override', '')).resolve() != expected_native
            or hashlib.sha256(expected_native.read_bytes()).hexdigest() != probe.get('stripped_sha256')):
        raise RuntimeError('AO ablation requires this session\'s verified local diagnostic wrapper')


def main():
    global OUTPUT, DEVICE_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['setup', 'isolate', 'launch', 'sample', 'capture', 'stop', 'measure', 'resume'])
    parser.add_argument('--serial', required=True)
    parser.add_argument('--model', choices=('SM-X920', 'SM-S906E'), default='SM-X920')
    parser.add_argument('--session', default='tablet-performance-20260910',
                        help='Separate backup and isolated-data owner for each physical device')
    parser.add_argument('--name', default='baseline')
    parser.add_argument('--ao', type=int, choices=(0, 1, 2), default=0)
    parser.add_argument('--fxaa', type=int, choices=(0, 1), default=0)
    parser.add_argument('--vsync', type=int, choices=(0, 1), default=1)
    parser.add_argument('--tick-hz', type=int, choices=(0, 60), default=60)
    parser.add_argument('--shadow-quality', type=int, choices=(1, 2, 4, 8, 16), default=1)
    parser.add_argument('--ticks', type=int, default=24000)
    parser.add_argument('--seconds', type=int, default=75, choices=range(10, 121), metavar='10..120')
    parser.add_argument('--pause', action='store_true', help='Keep the owned board loaded in the background at the end of a window')
    parser.add_argument('--ui', action='store_true',
                        help='Include the normal launcher/overlay renderer; press Play before the scripted board drive')
    parser.add_argument('--skip-ao-composite', action='store_true',
                        help='Diagnostic probe only: omit the final AO draw, not a shipping optimization')
    parser.add_argument('--frame-upload-mode', type=int, choices=(0, 1, 2),
                        help='Private upload scheduling diagnostic: control, batched, or ABBA')
    parser.add_argument('--ao-denoise-mode', type=int, choices=(0, 1, 2),
                        help='Private AO denoise diagnostic: control, tiled, or ABBA')
    parser.add_argument('--max-start-status', type=int, choices=(0, 1, 2), default=0,
                        help='Explicit launch cohort; status 2 is for short diagnostics only, never FPS measurements')
    parser.add_argument('--max-start-ap-c', type=temperature_limit,
                        help='Optional upper bound on the AP sensor; missing readings refuse launch/resume')
    parser.add_argument('--max-start-skin-c', type=temperature_limit,
                        help='Optional upper bound on the SKIN sensor; use the same bound for both variants')
    parser.add_argument('--max-window-status', type=int, choices=(0, 1), default=1,
                        help='Highest allowed thermal status during measurement; 0 makes a strict cool window')
    args = parser.parse_args()
    if args.max_start_status == 2 and args.action != 'launch':
        parser.error('moderate-thermal launch is diagnostic-only; normal measurements retain their cutoff')
    temperature_limits = dict(AP=args.max_start_ap_c, SKIN=args.max_start_skin_c)
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple run name required')
    if not 1200 <= args.ticks <= 120000:
        parser.error('tick budget must be 1200..120000')
    serial = args.serial
    if shell(serial, 'getprop', 'ro.product.model') != args.model:
        parser.error('connected device does not match --model')
    OUTPUT, DEVICE_ROOT, identity = session_paths(serial, args.model, args.session)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    owner = OUTPUT/'session-owner.json'
    if owner.exists() and json.loads(owner.read_text()) != identity:
        raise RuntimeError('profiling session belongs to another device')
    run = OUTPUT/args.name
    if args.action == 'setup':
        backup = OUTPUT/'original'
        backup.mkdir(exist_ok=False)
        write_json(owner, identity)
        for relative in ('mp6_config.json', 'saves'):
            adb(serial, 'pull', USER_ROOT+'/'+relative, str(backup/relative), timeout=60)
        package_path = shell(serial, 'pm', 'path', PACKAGE).removeprefix('package:')
        if not package_path.startswith('/data/app/') or '\n' in package_path:
            raise RuntimeError('expected one installed base APK')
        adb(serial, 'pull', package_path, str(backup/'installed.apk'), timeout=60)
        records = {p.relative_to(backup).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in backup.rglob('*') if p.is_file()}
        write_json(OUTPUT/'original-manifest.json', records)
        print('Backed up installed APK, settings and card')
        print('Installed APK SHA256:', records['installed.apk'])
        return
    if not (OUTPUT/'original-manifest.json').exists():
        raise RuntimeError('run setup before touching the app')
    if args.session != 'tablet-performance-20260910' and not owner.exists():
        raise RuntimeError('missing session identity; do not reuse another device backup')
    if args.action == 'isolate':
        # run-as is available only in the local profiling wrapper. Share game
        # content read-only by path, never the player's card or configuration.
        probe = adb(serial, 'shell', shlex.join(['run-as', PACKAGE, 'test', '-e', DEVICE_ROOT]), check=False)
        if probe.returncode == 0:
            raise RuntimeError('isolated device directory already exists')
        shell(serial, 'run-as', PACKAGE, 'mkdir', '-p', DEVICE_ROOT+'/gpu-cache')
        shell(serial, 'run-as', PACKAGE, 'ln', '-s', USER_ROOT+'/GP6E01', DEVICE_ROOT+'/GP6E01')
        shell(serial, 'run-as', PACKAGE, 'test', '-r', DEVICE_ROOT+'/GP6E01/sys/fst.bin')
        print('Created isolated saves/config/cache at', DEVICE_ROOT)
        return
    if args.action == 'launch':
        if shell(serial, 'pidof', PACKAGE, check=False):
            raise RuntimeError('MP6 is already running; inspect/stop the owned run first')
        start_thermal = shell(serial, 'dumpsys', 'thermalservice')
        verify_start_thermal(start_thermal, args.max_start_status, temperature_limits)
        run.mkdir(exist_ok=False)
        env = dict(MP6_HOST_BASE=DEVICE_ROOT, MP6_GPU_CACHE_PATH=DEVICE_ROOT+'/gpu-cache',
                   MP6_LAUNCHER='1' if args.ui else '0', MP6_BOOT_TO='mdparty', MP6_AUTO_START_TICKS='60,150',
                   MP6_TICK_HZ=str(args.tick_hz), MP6_VSYNC=str(args.vsync), MP6_ENH_WIDESCREEN='1',
                   MP6_ENH_SHADOW_QUALITY=str(args.shadow_quality), MP6_ENH_AMBIENT_OCCLUSION=str(args.ao),
                   MP6_ENH_AA='2' if args.fxaa else '0', MP6_FXAA=str(args.fxaa),
                   MP6_UNLOCKED_FPS='1', MP6_TICK_RATE_LOG='1', MP6_PRESENT_RATE_LOG='1',
                   MP6_GPU_TIMINGS='1', MP6_AO_DIAG='1')
        if args.skip_ao_composite:
            env['MP6_PROFILE_SKIP_AO_COMPOSITE'] = '1'
        if args.frame_upload_mode is not None:
            env['MP6_UPLOAD_MODE'] = str(args.frame_upload_mode)
        if args.ao_denoise_mode is not None:
            env['MP6_DENOISE_MODE'] = str(args.ao_denoise_mode)
        tokens = [f'{key}={value}' for key, value in env.items()]
        tokens += [str(args.ticks), '--input-script',
                   f'period:30;timeout:{args.ticks-1000};pressuntil:a/w01.live/120;wait:120000']
        request = dict(serial=serial, args=tokens, start_time=time.time(),
                       ui_workload='interactive renderer' if args.ui else 'automation renderer (no RmlUi)')
        apk_path = shell(serial, 'pm', 'path', PACKAGE).removeprefix('package:')
        if not apk_path.startswith('/data/app/') or '\n' in apk_path:
            raise RuntimeError('expected exactly one installed MP6 base APK')
        request['apk_sha256'] = shell(serial, 'sha256sum', apk_path).split()[0]
        if args.skip_ao_composite:
            verify_composite_probe(OUTPUT, request['apk_sha256'])
            request['diagnostic'] = 'AO composite ablation: not shipping quality or a speedup'
        request['thermal_start'] = start_thermal
        request['thermal_admission'] = dict(max_status=args.max_start_status, limits_c=temperature_limits)
        request['launch'] = shell(serial, 'am', 'start', '-W', '-n', PACKAGE+'/.Mp6Activity',
                                  '--es', 'args', ' '.join(tokens))
        request['pid'] = shell(serial, 'pidof', PACKAGE, check=False)
        write_json(run/'request.json', request)
        print(request['launch'])
        print('Owned profiling PID:', request['pid'], 'run:', run)
        return
    request = json.loads((run/'request.json').read_text())
    if request['serial'] != serial:
        raise RuntimeError('run belongs to another device')
    pid = request['pid']
    if not pid.isdigit():
        raise RuntimeError('invalid recorded PID')
    if args.action == 'resume':
        if shell(serial, 'pidof', PACKAGE, check=False) != pid:
            raise RuntimeError('owned process was lost; do not create a new unconfigured game')
        report = shell(serial, 'dumpsys', 'thermalservice')
        verify_start_thermal(report, args.max_start_status, temperature_limits)
        log = adb(serial, 'logcat', '-d', '--pid='+pid, '-v', 'brief').stdout.decode(errors='replace')
        ticks = re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)', log)
        if not ticks:
            raise RuntimeError('no game tick evidence; cannot select a resumed measurement window')
        write_json(run/('resume-'+str(time.time_ns())+'.json'),
                   dict(time=time.time(), thermal=report, last_reported_tick=int(ticks[-1]),
                        thermal_admission=dict(max_status=args.max_start_status, limits_c=temperature_limits)))
        print(shell(serial, 'am', 'start', '-W', '-n', PACKAGE+'/.Mp6Activity'))
        return
    if args.action == 'measure':
        if shell(serial, 'pidof', PACKAGE, check=False) != pid:
            raise RuntimeError('owned game process is not running')
        if not mp6_is_foreground(serial):
            raise RuntimeError('MP6 must be foreground before measuring; resume the owned run first')
        samples = []
        end = time.monotonic()+args.seconds
        reason = 'bounded window completed'
        try:
            while True:
                report = shell(serial, 'dumpsys', 'thermalservice')
                samples.append(dict(time=time.time(), status=thermal_status(report), report=report))
                if samples[-1]['status'] > args.max_window_status:
                    reason = f'stopped at thermal status >= {args.max_window_status+1}; not a cool comparison'
                    break
                if time.monotonic() >= end:
                    break
                if shell(serial, 'pidof', PACKAGE, check=False) != pid:
                    reason = 'owned process exited'
                    break
                time.sleep(min(5, max(0, end-time.monotonic())))
        finally:
            write_json(run/('thermal-window-'+str(time.time_ns())+'.json'),
                       dict(reason=reason, samples=samples, pause_requested=args.pause,
                            max_allowed_status=args.max_window_status,
                            thermal_comparison_valid=bool(samples) and
                            all(sample['status'] <= args.max_window_status for sample in samples)))
            if shell(serial, 'pidof', PACKAGE, check=False) == pid:
                if args.pause:
                    # Only background MP6 when it is actually the resumed app;
                    # never press Home in another app the user opened meanwhile.
                    if not mp6_is_foreground(serial):
                        raise RuntimeError('MP6 is no longer foreground; do not navigate another app')
                    shell(serial, 'input', 'keyevent', 'KEYCODE_HOME')
                else:
                    shell(serial, 'am', 'force-stop', PACKAGE)
        print(reason)
        args.action = 'sample'
    if args.action == 'sample':
        output = adb(serial, 'logcat', '-d', '--pid='+pid, '-v', 'brief').stdout
        (run/'logcat.txt').write_bytes(output)
        (run/'thermal.txt').write_text(shell(serial, 'dumpsys', 'thermalservice'))
        rows = [line for line in output.decode(errors='replace').splitlines()
                if any(key in line for key in ('MP6-TICKRATE', 'MP6-PRESENTRATE', 'MP6-GPU', 'ENGINE-GPU',
                                               '[FATAL]', 'MP6-CRASH', 'w01.live', '[MP6-AO]'))]
        print('\n'.join(rows[-24:]))
        print('Current PID:', shell(serial, 'pidof', PACKAGE, check=False), 'log bytes:', len(output))
    elif args.action == 'capture':
        if shell(serial, 'pidof', PACKAGE, check=False) != pid:
            raise RuntimeError('owned game process is not running')
        if not mp6_is_foreground(serial):
            raise RuntimeError('only capture MP6, not the launcher or another app')
        image = adb(serial, 'exec-out', 'screencap', '-p').stdout
        if not mp6_is_foreground(serial):
            raise RuntimeError('foreground changed during capture; discard the image')
        if not image.startswith(b'\x89PNG\r\n\x1a\n'):
            raise RuntimeError('invalid screenshot')
        path = run/('screen-'+str(time.time_ns())+'.png')
        path.write_bytes(image)
        print(path)
    elif args.action == 'stop':
        if shell(serial, 'pidof', PACKAGE, check=False) != pid:
            raise RuntimeError('refusing to stop an unowned game process')
        shell(serial, 'am', 'force-stop', PACKAGE)
        print('Stopped isolated profiling run; player saves/config were not used')


if __name__ == '__main__':
    main()
