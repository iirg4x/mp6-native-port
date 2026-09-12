"""Capture MP6-only CPU samples, binding the diagnostic ELF and temporary tool."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[3]
SESSION=ROOT/'build/s22-ready-memo-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell,mp6_is_foreground
SERIAL='RZCTB00SF3W'
PACKAGE='com.mp6.game'
REMOTE='/data/local/tmp/mp6-s22-simpleperf-20260911'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    run=SESSION/'normal-off'
    assert not (run/'perf.data').exists()
    assert shell(SERIAL,'getprop','ro.product.model')=='SM-S906E'
    request=json.loads((run/'request.json').read_text())
    assert request['serial']==SERIAL and shell(SERIAL,'pidof',PACKAGE,check=False)==request['pid']
    native=ROOT/'build/ready-pipeline-diagnostic-20260912/provenance.json'
    proof=json.loads(native.read_text())
    for path,digest in proof['artifacts'].items(): assert sha(Path(path))==digest
    wrapper=json.loads((SESSION/'apk-candidate/provenance.json').read_text())
    package=shell(SERIAL,'pm','path',PACKAGE).removeprefix('package:')
    assert package.startswith('/data/app/') and '\n' not in package
    assert shell(SERIAL,'sha256sum',package).split()[0]==wrapper['sha256']==request['apk_sha256']
    assert adb(SERIAL,'shell','test -e '+REMOTE,check=False).returncode!=0, 'do not replace an existing tool'
    local=ROOT/'build/android-sdk/ndk/27.3.13750724/simpleperf/bin/android/arm64/simpleperf'
    expected=sha(local)
    adb(SERIAL,'push',str(local),REMOTE)
    try:
        assert shell(SERIAL,'sha256sum',REMOTE).split()[0]==expected
        shell(SERIAL,'chmod','700',REMOTE)
        subprocess.run(['powershell','-NoProfile','-File',str(Path(__file__).with_name('phone_window.ps1')),
                        '-Action','resume','-Name','normal-off','-Warmup'],cwd=ROOT,check=True)
        assert mp6_is_foreground(SERIAL)
        subprocess.run(['powershell','-NoProfile','-File',str(Path(__file__).with_name('phone_window.ps1')),
                        '-Action','capture','-Name','normal-off'],cwd=ROOT,check=True)
        subprocess.run([sys.executable,str(ROOT/'tests/integration/capture_android_cpu_profile.py'),
            '--serial',SERIAL,'--model','SM-S906E','--session',SESSION.name,'--name','normal-off'],cwd=ROOT,check=True)
        result=dict(apk_sha256=wrapper['sha256'],native_proof_sha256=sha(native),
                    perf_sha256=sha(run/'perf.data'),tool_sha256=expected,
                    diagnostic_overhead_present=True,fps_evidence=False,
                    unstripped_elf=str(ROOT/'build/ready-pipeline-diagnostic-20260912/libmp6game.so'))
        (run/'cpu-provenance.json').write_text(json.dumps(result,indent=2))
    finally:
        assert shell(SERIAL,'sha256sum',REMOTE).split()[0]==expected, 'temporary tool changed; do not remove it'
        shell(SERIAL,'rm',REMOTE)

if __name__=='__main__': main()
