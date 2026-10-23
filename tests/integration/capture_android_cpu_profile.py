"""Bounded, app-only CPU sample after the isolated board is warmed; never FPS evidence."""
import argparse
import json
import re
import shlex
import time
from android_device_profile import adb, shell, session_paths, thermal_status, PACKAGE, write_json


SUPPORTED_MODELS = ('SM-X920', 'SM-S906E')
DEFAULT_WARMUP_BOARD_TICKS = 1800
MAX_WARMUP_BOARD_TICKS = 6000
WARMUP_DEADLINE_SECONDS = 90
SAMPLE_DURATION_SECONDS = 8
SAMPLE_SIZE_LIMIT = '32M'
SIMPLE_NAME_RE = r'[A-Za-z0-9_-]+'


def simple_name(value, label='simple name'):
    """Validate a value used in a local or device-side artifact name."""
    if not isinstance(value, str) or not re.fullmatch(SIMPLE_NAME_RE, value):
        raise ValueError(f'{label} required')
    return value


def warmup_board_ticks(value):
    """Parse a bounded, settled-board warmup threshold for argparse."""
    try:
        ticks = int(value)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError('warmup board ticks must be an integer') from error
    if not DEFAULT_WARMUP_BOARD_TICKS <= ticks <= MAX_WARMUP_BOARD_TICKS:
        raise argparse.ArgumentTypeError(
            f'warmup board ticks must be in {DEFAULT_WARMUP_BOARD_TICKS}..{MAX_WARMUP_BOARD_TICKS}')
    return ticks


def remote_capture_paths(session, name):
    """Return unique, session-owned simpleperf paths for one capture."""
    simple_name(session, 'simple session name')
    simple_name(name, 'simple run name')
    return (f'/data/local/tmp/mp6-simpleperf-{session}',
            f'/data/local/tmp/mp6-cpu-{session}-{name}.data')


def package_is_foreground(activity_dump, package=PACKAGE):
    """Require exactly one resumed activity and require it to belong to MP6."""
    resumed = [line for line in activity_dump.splitlines()
               if 'mResumedActivity:' in line or 'topResumedActivity=' in line]
    return len(resumed) == 1 and package+'/' in resumed[0]


def require_owned_foreground(serial, pid):
    """Refuse sampling unless the recorded process is still the foreground app."""
    if shell(serial, 'pidof', PACKAGE, check=False) != pid:
        raise RuntimeError('owned process was lost')
    if not package_is_foreground(shell(serial, 'dumpsys', 'activity', 'activities')):
        raise RuntimeError('MP6 must be foreground before CPU capture')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--model', required=True, choices=SUPPORTED_MODELS)
    parser.add_argument('--session', required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--warmup-board-ticks', type=warmup_board_ticks,
                        default=DEFAULT_WARMUP_BOARD_TICKS,
                        help='Minimum settled intro age before attaching simpleperf')
    args = parser.parse_args()
    try:
        simple_name(args.name, 'simple run name')
    except ValueError as error:
        parser.error(str(error))
    if shell(args.serial, 'getprop', 'ro.product.model') != args.model:
        parser.error('connected device does not match --model')
    output, device_root, _ = session_paths(args.serial, args.model, args.session)
    if not (output/'session-owner.json').exists():
        raise RuntimeError('missing session owner')
    run = output/args.name
    request = json.loads((run/'request.json').read_text())
    pid = request['pid']
    if request['serial'] != args.serial or not pid.isdigit():
        raise RuntimeError('invalid owned process')
    remote_tool, remote_data = remote_capture_paths(args.session, args.name)
    if (run/'perf.data').exists():
        raise RuntimeError('never overwrite a CPU capture')
    # Refuse to sample a backgrounded run before any profiler work begins.
    require_owned_foreground(args.serial, pid)
    if adb(args.serial, 'shell', shlex.join(['test', '-e', remote_data]), check=False).returncode == 0:
        raise RuntimeError('remote recording already exists; do not overwrite it')
    recording_started = False
    try:
        deadline = time.monotonic()+WARMUP_DEADLINE_SECONDS
        while True:
            require_owned_foreground(args.serial, pid)
            thermal = shell(args.serial, 'dumpsys', 'thermalservice')
            if thermal_status(thermal) >= 2:
                raise RuntimeError('thermal cutoff before CPU capture')
            log = adb(args.serial, 'logcat', '-d', '--pid='+pid, '-v', 'brief').stdout
            text = log.decode(errors='replace')
            event = re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)', text)
            ticks = re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)', text)
            if event and ticks and int(ticks[-1]) >= int(event[1])+args.warmup_board_ticks:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('board did not warm within bounded startup')
            time.sleep(1)
        # Check again immediately before attaching simpleperf; the warmup
        # loop may have observed a foreground transition between iterations.
        require_owned_foreground(args.serial, pid)
        thermal = shell(args.serial, 'dumpsys', 'thermalservice')
        if thermal_status(thermal) >= 2:
            raise RuntimeError('thermal cutoff before CPU capture')
        command = [remote_tool, 'record', '--app', PACKAGE, '-p', pid, '-f', '500',
                   '-e', 'cpu-cycles:u', '--call-graph', 'dwarf,8192', '--duration',
                   str(SAMPLE_DURATION_SECONDS),
                   '--size-limit', SAMPLE_SIZE_LIMIT, '--no-inherit', '-o', remote_data]
        write_json(run/'cpu-capture-request.json', dict(command=command, thermal=thermal,
                                                       tick=int(ticks[-1]),
                                                       warmup_board_ticks=args.warmup_board_ticks,
                                                       scope='MP6 user-space CPU samples only'))
        recording_started = True
        result = adb(args.serial, 'shell', shlex.join(command), timeout=30, check=False)
        (run/'cpu-capture-result.txt').write_bytes(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout+result.stderr).decode(errors='replace'))
        # --app opens the output as shell, then switches the sampling to run-as.
        data = adb(args.serial, 'exec-out', shlex.join(['cat', remote_data])).stdout
        if not data.startswith(b'PERFILE2'):
            raise RuntimeError('invalid simpleperf recording')
        (run/'perf.data').write_bytes(data)
        print('App-only CPU recording:', run/'perf.data', 'bytes:', len(data))
    finally:
        if shell(args.serial, 'pidof', PACKAGE, check=False) == pid:
            shell(args.serial, 'am', 'force-stop', PACKAGE)
        if recording_started:
            shell(args.serial, 'rm', '-f', remote_data)
        # Sampling is a different workload. Preserve any clean FPS window that
        # was recorded before attaching simpleperf instead of silently replacing
        # its evidence with profiler-instrumented frames.
        (run/'cpu-logcat.txt').write_bytes(adb(args.serial, 'logcat', '-d', '--pid='+pid, '-v', 'brief').stdout)
        (run/'cpu-thermal.txt').write_text(shell(args.serial, 'dumpsys', 'thermalservice'))


if __name__ == '__main__':
    main()
