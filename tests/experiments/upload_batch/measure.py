"""Bounded same-process upload A/B/B/A; whole successful-present intervals only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tests/integration'))
from android_device_profile import adb, shell, mp6_is_foreground, thermal_status, PACKAGE

PATTERN = re.compile(r'\[MP6-(?:UPLOAD|DENOISE)-ABBA\] phase=(\d+) mode=(\d+) valid=(\d+) frames=(\d+) draws=(\d+) first_ns=(\d+) last_ns=(\d+)')


def parse(text, used_frames=216):
    rows = []
    for match in PATTERN.finditer(text):
        row = dict(zip(('phase','mode','valid','frames','draws','first_ns','last_ns'), map(int,match.groups())))
        assert row['mode'] == int(row['phase'] % 4 in (1,2))
        assert not rows or row['phase'] > rows[-1]['phase']
        if row['valid']:
            assert row['frames'] == used_frames and row['last_ns'] > row['first_ns']
            row['fps'] = (row['frames']-1)*1e9/(row['last_ns']-row['first_ns'])
        rows.append(row)
    return rows


def compare(rows, minimum, maximum):
    phases = {r['phase']:r for r in rows if minimum <= r['phase'] <= maximum and r['valid']}
    groups = []
    for start in sorted(phases):
        if start % 4 or any(start+i not in phases for i in range(4)): continue
        group = [phases[start+i] for i in range(4)]
        def rate(mode):
            subset = [r for r in group if r['mode']==mode]
            return sum(r['frames']-1 for r in subset)*1e9/sum(r['last_ns']-r['first_ns'] for r in subset)
        control, candidate = rate(0), rate(1)
        groups.append(dict(first_phase=start, baseline_fps=control, candidate_fps=candidate,
            change_percent=(candidate/control-1)*100,
            draws_per_frame=[r['draws']/r['frames'] for r in group]))
    return groups


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', required=True)
    p.add_argument('--name', required=True)
    p.add_argument('--seconds', type=int, choices=range(10,46), default=30)
    p.add_argument('--revision', default='r1')
    p.add_argument('--pause', action='store_true')
    p.add_argument('--experiment', choices=('upload-batch','ao-denoise'),default='upload-batch')
    a = p.parse_args()
    assert all(re.fullmatch('[a-z0-9-]+',s) for s in (a.session,a.name))
    session = ROOT/'build'/a.session; run = session/a.name
    request = json.loads((run/'request.json').read_text())
    serial, pid = request['serial'], request['pid']
    identity = json.loads((session/'session-owner.json').read_text())
    assert serial==identity['serial'] and shell(serial,'getprop','ro.product.model')==identity['model']
    mode_key='MP6_UPLOAD_MODE=2' if a.experiment=='upload-batch' else 'MP6_DENOISE_MODE=2'
    assert mode_key in request['args'] and not (run/'upload-comparison.json').exists()
    assert re.fullmatch('r[0-9]+',a.revision)
    apk_name='candidate' if a.revision=='r1' else 'candidate-'+a.revision
    apk = json.loads((session/f'apk-{apk_name}/provenance.json').read_text())
    assert apk['sha256']==request['apk_sha256']
    package = shell(serial,'pm','path',PACKAGE).removeprefix('package:')
    assert package.startswith('/data/app/') and '\n' not in package
    assert shell(serial,'sha256sum',package).split()[0]==apk['sha256']
    proof = json.loads((ROOT/f'build/{a.experiment}-20260912/android-{a.revision}/provenance.json').read_text())
    used_frames=108 if proof.get('short_phases') else 216
    for path,digest in proof['artifacts'].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest
    assert Path(apk['native_override']).resolve() in [Path(p).resolve() for p in proof['artifacts']]
    observations = []; reason='completed'; minimum=None
    try:
        deadline = time.monotonic()+45
        while True:
            assert shell(serial,'pidof',PACKAGE,check=False)==pid and mp6_is_foreground(serial)
            report = shell(serial,'dumpsys','thermalservice')
            assert thermal_status(report)<3, 'Severe thermal cutoff'
            log = adb(serial,'logcat','-d','--pid='+pid,'-v','brief').stdout.decode(errors='replace')
            board = re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)',log)
            ticks = re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)',log)
            rows=parse(log,used_frames)
            if board and ticks and int(ticks[-1])>=int(board[1])+120 and rows:
                minimum=rows[-1]['phase']+2
                break
            assert time.monotonic()<deadline, 'Board warmup deadline'
            time.sleep(1)
        (run/'upload-before.log').write_text(log)
        start=time.monotonic()
        while time.monotonic()-start<a.seconds:
            assert shell(serial,'pidof',PACKAGE,check=False)==pid and mp6_is_foreground(serial)
            report=shell(serial,'dumpsys','thermalservice')
            observations.append(dict(elapsed=time.monotonic()-start,status=thermal_status(report),report=report))
            if observations[-1]['status']>=3:
                reason='severe thermal cutoff'; break
            time.sleep(1)
    finally:
        if shell(serial,'pidof',PACKAGE,check=False)==pid:
            if a.pause and mp6_is_foreground(serial):
                shell(serial,'input','keyevent','KEYCODE_HOME')
            else:
                shell(serial,'am','force-stop',PACKAGE)
        log=adb(serial,'logcat','-d','--pid='+pid,'-v','brief').stdout
        (run/'upload-after.log').write_bytes(log)
        (run/'upload-thermal.json').write_text(json.dumps(observations,indent=2))
    rows=parse(log.decode(errors='replace'),used_frames); maximum=rows[-1]['phase']-1 if rows else -1
    groups=compare(rows,minimum,maximum) if minimum is not None else []
    states={o['status'] for o in observations}
    eligible=bool(groups and len(states)==1 and reason=='completed')
    result=dict(experiment=a.experiment,eligible=eligible, reason=reason, thermal_states=sorted(states),
        minimum_phase=minimum,maximum_phase=maximum,groups=groups,rows=rows,
        apk_sha256=apk['sha256'], request=request,
        limitations=['Same-process short balanced windows, not sustained performance.',
            'CPU/GPU clocks are not locked. Thermal state changes invalidate the window.',
            'Counter is successful queue presentations, not physical display refresh rate.',
            'No quality reduction; separate image/API validation is required.'])
    (run/'upload-comparison.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(dict(eligible=eligible,thermal_states=sorted(states),groups=groups),indent=2))


if __name__=='__main__': main()
