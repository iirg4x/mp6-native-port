"""Freeze a thermal-cohort-labelled same-process bundle comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from abba_report import parse,groups
ROOT=Path(__file__).resolve().parents[3]
SESSION=ROOT/'build/s22-bundle-abba-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell,mp6_is_foreground,thermal_status,verify_start_thermal
SERIAL='RZCTB00SF3W'; PACKAGE='com.mp6.game'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v): p.write_text(json.dumps(v,indent=2)+'\n')
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',required=True)
    parser.add_argument('--label',required=True)
    parser.add_argument('--cohort',type=int,choices=(0,1),default=0)
    parser.add_argument('--seconds',type=int,choices=range(10,61),default=25)
    parser.add_argument('--resume',action='store_true')
    a=parser.parse_args()
    assert all(re.fullmatch('[a-z0-9-]+',v) for v in (a.name,a.label))
    run=SESSION/a.name
    request=json.loads((run/'request.json').read_text())
    assert request['serial']==SERIAL and shell(SERIAL,'getprop','ro.product.model')=='SM-S906E'
    pid=request['pid']; assert pid.isdigit() and shell(SERIAL,'pidof',PACKAGE,check=False)==pid
    apk=json.loads((SESSION/'apk-candidate/provenance.json').read_text())
    package=shell(SERIAL,'pm','path',PACKAGE).removeprefix('package:')
    assert package.startswith('/data/app/') and '\n' not in package
    assert shell(SERIAL,'sha256sum',package).split()[0]==apk['sha256']==request['apk_sha256']
    native=ROOT/'build/render-bundle-abba-20260912/android/provenance.json'
    proof=json.loads(native.read_text())
    for path,digest in proof['artifacts'].items(): assert sha(Path(path))==digest
    assert Path(apk['native_override']).resolve() in [Path(p).resolve() for p in proof['artifacts']]
    report=shell(SERIAL,'dumpsys','thermalservice')
    verify_start_thermal(report,a.cohort,dict(AP=38.0,SKIN=37.0))
    assert thermal_status(report)==a.cohort,'do not mix thermal cohorts'
    if a.resume:
        subprocess.run(['powershell','-NoProfile','-File',str(Path(__file__).with_name('abba_phone.ps1')),
            '-Action','resume','-Name',a.name,'-Cohort',str(a.cohort)],cwd=ROOT,check=True)
    assert mp6_is_foreground(SERIAL)
    prior=adb(SERIAL,'logcat','-d','--pid='+pid,'-v','brief').stdout
    text=prior.decode(errors='replace'); previous=parse(text)
    assert previous,'no phase ownership evidence'
    ticks=re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)',text)
    board=re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)',text)
    assert ticks and board
    last_tick=int(ticks[-1]); board_tick=int(board[1])
    assert last_tick>=board_tick+600,'need at least 600 settled board ticks before timing'
    output=run/a.label; output.mkdir(exist_ok=False)
    (output/'before.log').write_bytes(prior)
    minimum=previous[-1]['phase']+2
    marker=dict(time=time.time(),minimum_phase=minimum,board_tick=board_tick,last_tick=last_tick,
        cohort=a.cohort,workload=('normal-speed unlocked' if 'MP6_TICK_HZ=60' in request['args'] else 'Fast Forward'),
        request_sha256=sha(run/'request.json'),native_sha256=sha(native),thermal_start=report)
    save(output/'marker.json',marker)
    old=set(run.glob('thermal-window-*.json'))
    result=subprocess.run(['powershell','-NoProfile','-File',str(Path(__file__).with_name('abba_phone.ps1')),
        '-Action','measure','-Name',a.name,'-Cohort',str(a.cohort),'-Seconds',str(a.seconds)],
        cwd=ROOT,capture_output=True,text=True)
    (output/'command.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    files=set(run.glob('thermal-window-*.json'))-old; assert len(files)==1
    thermal_file=files.pop(); thermal=json.loads(thermal_file.read_text())
    log=(run/'logcat.txt').read_bytes(); (output/'after.log').write_bytes(log)
    samples=parse(log.decode(errors='replace'))
    # Conservatively discard the final logged phase as well as the partially
    # elapsed first one. The renderer queues at most two frame packets, not a
    # whole 240-frame phase, around the final thermal sample/Home transition.
    maximum=samples[-1]['phase']-1
    selected=[r for r in samples if minimum<=r['phase']<=maximum]
    balanced=groups(samples,minimum,maximum)
    stable=thermal['reason']=='bounded window completed' and bool(thermal['samples']) and all(
        x['status']==a.cohort for x in thermal['samples'])
    temperatures={sensor:[float(value) for sample in thermal['samples'] for value in re.findall(
        r'Temperature\{mValue=([^,]+), mType=\d+, mName='+sensor+r',',sample['report'])] for sensor in ('AP','SKIN')}
    record=dict(cohort=a.cohort,thermal_label='cool' if a.cohort==0 else 'steady light thermal status; not cool',
        stable_thermal_status=bool(stable),eligible=bool(stable and balanced),rows=selected,
        groups=balanced,minimum_phase=minimum,maximum_phase=maximum,temperature_readings_c=temperatures,
        hashes={str(p):sha(p) for p in (output/'before.log',output/'after.log',output/'marker.json',thermal_file,native)},
        limitations=['Balanced same-process comparison does not lock CPU/GPU clocks.',
            'Two clock reads per render pass measure CPU encoding, not GPU execution.',
            'A cohort change invalidates the entire window. Do not merge warm and cool runs.',
            'Repeated normal-speed and AO coverage is required before adopting the change.'])
    save(output/'measurement.json',record)
    print(json.dumps(dict(eligible=record['eligible'],cohort=a.cohort,groups=balanced,
        temperatures_c={k:[min(v),max(v)] if v else [] for k,v in temperatures.items()}),indent=2))
if __name__=='__main__': main()
