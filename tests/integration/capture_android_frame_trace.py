"""Capture a bounded scheduler/GPU handoff trace of an owned MP6 run.

No sampled stacks, app data, log clearing, clock changes or rendering changes.
System scheduler events provide context; reports must select the owned MP6 PID.
"""
import argparse
import hashlib
import json
import re
import subprocess
import time

from android_device_profile import ADB, PACKAGE, ROOT, adb, shell, mp6_is_foreground, thermal_status


CONFIG = '''duration_ms: 5000
buffers { size_kb: 32768 fill_policy: DISCARD }
data_sources { config { name: "linux.ftrace" ftrace_config {
  ftrace_events: "sched/sched_switch"
  ftrace_events: "sched/sched_waking"
  ftrace_events: "power/cpu_frequency"
  ftrace_events: "power/cpu_idle"
  ftrace_events: "kgsl/kgsl_pwrlevel"
  ftrace_events: "kgsl/kgsl_issueibcmds"
  ftrace_events: "kgsl/kgsl_cmdbatch_submitted"
  ftrace_events: "kgsl/kgsl_cmdbatch_retired"
  ftrace_events: "dma_fence/dma_fence_wait_start"
  ftrace_events: "dma_fence/dma_fence_wait_end"
  atrace_categories: "gfx"
  atrace_apps: "com.mp6.game"
} } }
data_sources { config { name: "linux.process_stats" process_stats_config {
  scan_all_processes_on_start: true
} } }
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', required=True)
    p.add_argument('--name', required=True)
    p.add_argument('--max-status', type=int, choices=(1, 2), default=1,
                   help='Diagnostic thermal cohort only; never an FPS improvement measurement')
    a = p.parse_args()
    assert all(re.fullmatch('[a-z0-9-]+', s) for s in (a.session, a.name))
    session = ROOT / 'build' / a.session
    run = session / a.name
    identity = json.loads((session / 'session-owner.json').read_text())
    request = json.loads((run / 'request.json').read_text())
    serial, pid = identity['serial'], request['pid']
    assert serial == request['serial'] and pid.isdigit()
    assert shell(serial, 'getprop', 'ro.product.model') == identity['model']
    assert not (run / 'frame.perfetto-trace').exists()
    remote = '/data/misc/perfetto-traces/mp6-frame-' + a.session + '-' + a.name
    assert adb(serial, 'shell', 'test -e ' + remote, check=False).returncode != 0
    process = None
    observations = []
    try:
        deadline = time.monotonic() + 45
        while True:
            assert shell(serial, 'pidof', PACKAGE, check=False) == pid and mp6_is_foreground(serial)
            thermal = shell(serial, 'dumpsys', 'thermalservice')
            assert thermal_status(thermal) <= a.max_status, 'Thermal cutoff'
            log = adb(serial, 'logcat', '-d', '--pid=' + pid, '-v', 'brief').stdout.decode(errors='replace')
            board = re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)', log)
            ticks = re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)', log)
            if board and ticks and int(ticks[-1]) >= int(board[1]) + 120:
                break
            assert time.monotonic() < deadline, 'Board did not warm in time'
            time.sleep(1)
        (run / 'frame.pbtx').write_text(CONFIG)
        process = subprocess.Popen([str(ADB), '-s', serial, 'shell', 'perfetto', '--txt',
                                   '-c', '-', '-o', remote], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        process.stdin.write(CONFIG.encode())
        process.stdin.close()
        process.stdin = None
        start = time.monotonic()
        while process.poll() is None:
            assert time.monotonic() - start < 20, 'Trace did not finish'
            assert shell(serial, 'pidof', PACKAGE, check=False) == pid and mp6_is_foreground(serial)
            thermal = shell(serial, 'dumpsys', 'thermalservice')
            observations.append(dict(elapsed=time.monotonic() - start, thermal=thermal))
            if thermal_status(thermal) > a.max_status:
                # Stop the workload immediately, but retain the trace prefix
                # instead of deleting evidence of the thermal transition.
                shell(serial, 'am', 'force-stop', PACKAGE)
                process.wait(timeout=10)
                break
            time.sleep(1)
        stdout, stderr = process.communicate(timeout=3)
        (run / 'trace-command.txt').write_bytes(stdout + stderr)
        assert process.returncode == 0, stderr.decode(errors='replace')
        adb(serial, 'pull', remote, str(run / 'frame.perfetto-trace'))
        data = (run / 'frame.perfetto-trace').read_bytes()
        assert len(data) > 1024
        (run / 'frame-trace-proof.json').write_text(json.dumps(dict(
            pid=pid, serial=serial, apk_sha256=request['apk_sha256'],
            trace_sha256=hashlib.sha256(data).hexdigest(), trace_bytes=len(data),
            thermal=observations, max_status=a.max_status,
            profiling_changes_timing=True, shipping_fps_evidence=False), indent=2))
        print('Captured', len(data), 'bytes; MP6 PID', pid)
    finally:
        if process and process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        if shell(serial, 'pidof', PACKAGE, check=False) == pid:
            shell(serial, 'am', 'force-stop', PACKAGE)
        (run / 'frame-logcat.txt').write_bytes(adb(serial, 'logcat', '-d', '--pid=' + pid, '-v', 'brief').stdout)
        (run / 'frame-thermal.json').write_text(json.dumps(observations, indent=2))
        shell(serial, 'rm', '-f', remote)


if __name__ == '__main__':
    main()
