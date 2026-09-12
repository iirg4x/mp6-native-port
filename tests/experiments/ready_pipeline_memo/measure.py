"""Freeze a bounded, thermally monitored same-process pipeline-cache comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[3]
SESSION=ROOT/'build/s22-ready-memo-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell,mp6_is_foreground
SERIAL='RZCTB00SF3W'
PACKAGE='com.mp6.game'
PATTERN=re.compile(r'\[MP6-READYMEMO\] phase=(\d+) mode=(\d+) valid=(\d+) frames=(\d+) calls=(\d+) hits=(\d+) lookup_ns=(\d+) first_ns=(\d+) last_ns=(\d+)')
KEYS=('phase','mode','valid','frames','calls','hits','lookup_ns','first_ns','last_ns')

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def rows(text): return [dict(zip(KEYS,map(int,m.groups()))) for m in PATTERN.finditer(text)]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name',required=True)
    p.add_argument('--label',required=True)
    p.add_argument('--seconds',type=int,default=25,choices=range(10,61))
    p.add_argument('--warmup',action='store_true')
    p.add_argument('--resume',action='store_true')
    p.add_argument('--capture',action='store_true')
    a=p.parse_args()
    assert all(re.fullmatch('[a-z0-9-]+',x) for x in (a.name,a.label))
    assert shell(SERIAL,'getprop','ro.product.model')=='SM-S906E'
    run=SESSION/a.name
    request=json.loads((run/'request.json').read_text())
    assert request['serial']==SERIAL
    pid=request['pid']
    assert pid.isdigit() and shell(SERIAL,'pidof',PACKAGE,check=False)==pid
    wrapper=json.loads((SESSION/'apk-candidate/provenance.json').read_text())
    package=shell(SERIAL,'pm','path',PACKAGE).removeprefix('package:')
    assert package.startswith('/data/app/') and '\n' not in package
    assert shell(SERIAL,'sha256sum',package).split()[0]==wrapper['sha256']==request['apk_sha256']
    native=ROOT/'build/ready-pipeline-diagnostic-20260912/provenance.json'
    proof=json.loads(native.read_text())
    for path,digest in proof['artifacts'].items(): assert sha(Path(path))==digest
    assert Path(wrapper['native_override']).resolve() in [Path(x).resolve() for x in proof['artifacts']]
    if a.resume:
        resume=['powershell','-NoProfile','-File',str(Path(__file__).with_name('phone_window.ps1')),
                '-Action','resume','-Name',a.name]
        if a.warmup: resume+=['-Warmup']
        subprocess.run(resume,cwd=ROOT,check=True)
    assert mp6_is_foreground(SERIAL)
    if a.capture:
        subprocess.run(['powershell','-NoProfile','-File',str(Path(__file__).with_name('phone_window.ps1')),
                        '-Action','capture','-Name',a.name],cwd=ROOT,check=True)
    prior=adb(SERIAL,'logcat','-d','--pid='+pid,'-v','brief').stdout
    decoded=prior.decode(errors='replace')
    previous=rows(decoded)
    assert previous, 'no phase ownership evidence'
    ticks=re.findall(r'\[MP6-TICKRATE\].*?tick=(\d+)',decoded)
    board=re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)',decoded)
    assert ticks and board
    last_tick=int(ticks[-1]); board_tick=int(board[1])
    if not a.warmup: assert last_tick>=board_tick+1800, 'board has not finished settling'
    output=run/a.label
    output.mkdir(exist_ok=False)
    (output/'before.log').write_bytes(prior)
    # The last logged phase has completed; skip its partially elapsed successor.
    # The following phase starts after this marker (and has 60 settling frames).
    minimum_phase=previous[-1]['phase']+2
    marker=dict(time=time.time(),minimum_phase=minimum_phase,last_tick=last_tick,
                board_tick=board_tick,warmup=a.warmup,request_sha256=sha(run/'request.json'),
                native_proof_sha256=sha(native),wrapper_sha256=wrapper['sha256'])
    (output/'marker.json').write_text(json.dumps(marker,indent=2))
    before_files=set(run.glob('thermal-window-*.json'))
    cmd=['powershell','-NoProfile','-File',str(Path(__file__).with_name('phone_window.ps1')),
         '-Action','measure','-Name',a.name,'-Seconds',str(a.seconds)]
    if a.warmup: cmd+=['-Warmup']
    result=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
    (output/'command.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0, result.stdout+result.stderr
    thermal_files=set(run.glob('thermal-window-*.json'))-before_files
    assert len(thermal_files)==1
    thermal_file=thermal_files.pop()
    thermal=json.loads(thermal_file.read_text())
    log=run/'logcat.txt'
    (output/'after.log').write_bytes(log.read_bytes())
    samples=rows(log.read_text(errors='replace'))
    admitted=[r for r in samples if r['phase']>=minimum_phase and r['valid'] and r['frames']==540]
    thermal_ok=(thermal['reason']=='bounded window completed' and thermal['samples'] and
                all(x['status']==0 for x in thermal['samples']))
    for r in admitted:
        assert r['last_ns']>r['first_ns'] and r['calls']>0 and r['hits']<=r['calls']
        assert r['mode']==int(r['phase']%4 in (1,2))
        assert r['mode'] or r['hits']==0
        r['presents_per_second']=(r['frames']-1)*1e9/(r['last_ns']-r['first_ns'])
        r['lookup_ns_per_call']=r['lookup_ns']/r['calls']
        r['lookup_ms_per_frame']=r['lookup_ns']/r['frames']/1e6
        r['hit_percent']=100*r['hits']/r['calls']
    evidence=dict(warmup=a.warmup,thermal_valid=bool(thermal_ok),
                  eligible_for_comparison=bool(thermal_ok) and not a.warmup,
                  rows=admitted,thermal_file=str(thermal_file),
                  hashes={str(path):sha(path) for path in
                          (output/'before.log',output/'after.log',output/'marker.json',thermal_file,native)},
                  limitations=['Per-call timing and mode checks add diagnostic overhead to both modes.',
                               'No fixed-clock guarantee; use balanced ABBA groups and report temperature.',
                               'No automatic production adoption or release packaging.'])
    (output/'measurement.json').write_text(json.dumps(evidence,indent=2))
    print(json.dumps({k:evidence[k] for k in ('eligible_for_comparison','thermal_valid','rows')},indent=2))

if __name__=='__main__': main()
