"""Bounded, app-only CPU sample after the isolated board is warmed; never FPS evidence."""
import argparse
import json
import re
import shlex
import time
from android_device_profile import adb, shell, session_paths, thermal_status, PACKAGE, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--model', required=True, choices=('SM-S906E',))
    parser.add_argument('--session', required=True)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple run name required')
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
    remote_tool = '/data/local/tmp/mp6-s22-simpleperf-20260911'
    remote_data = '/data/local/tmp/mp6-s22-cpu-'+args.name+'.data'
    if (run/'perf.data').exists():
        raise RuntimeError('never overwrite a CPU capture')
    if adb(args.serial, 'shell', shlex.join(['test', '-e', remote_data]), check=False).returncode == 0:
        raise RuntimeError('remote recording already exists; do not overwrite it')
    recording_started = False
    try:
        deadline = time.monotonic()+45
        while True:
            if shell(args.serial, 'pidof', PACKAGE, check=False) != pid:
                raise RuntimeError('owned process was lost')
            thermal = shell(args.serial, 'dumpsys', 'thermalservice')
            if thermal_status(thermal) >= 2:
                raise RuntimeError('thermal cutoff before CPU capture')
            log = adb(args.serial, 'logcat', '-d', '--pid='+pid, '-v', 'brief').stdout
            text = log.decode(errors='replace')
            event = re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)', text)
            ticks = re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)', text)
            if event and ticks and int(ticks[-1]) >= int(event[1])+600:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('board did not warm within bounded startup')
            time.sleep(1)
        command = [remote_tool, 'record', '--app', PACKAGE, '-p', pid, '-f', '500',
                   '-e', 'cpu-cycles:u', '--call-graph', 'dwarf,8192', '--duration', '8',
                   '--size-limit', '32M', '--no-inherit', '-o', remote_data]
        write_json(run/'cpu-capture-request.json', dict(command=command, thermal=thermal,
                                                       tick=int(ticks[-1]), scope='MP6 user-space CPU samples only'))
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
