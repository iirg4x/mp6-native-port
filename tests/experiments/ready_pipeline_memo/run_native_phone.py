"""Run the two already-built memo correctness oracles on the owned S22, app closed."""
import hashlib
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/ready-pipeline-memo-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell,PACKAGE,thermal_status
SERIAL='RZCTB00SF3W'
REMOTE='/data/local/tmp/mp6-ready-memo-20260912'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def installed():
    path=shell(SERIAL,'pm','path',PACKAGE).removeprefix('package:')
    assert path.startswith('/data/app/') and '\n' not in path
    return shell(SERIAL,'sha256sum',path).split()[0]
def main():
    assert not (OUT/'android-native.json').exists(),'Do not overwrite phone proof'
    assert shell(SERIAL,'getprop','ro.product.model')=='SM-S906E'
    assert not shell(SERIAL,'pidof',PACKAGE,check=False),'Close the owned game before running CPU tests'
    before=installed()
    thermal=shell(SERIAL,'dumpsys','thermalservice')
    assert thermal_status(thermal)<2,'Do not run CPU tests while throttled'
    cases=[]
    for name,report,key in (('selftest','native-tests.json','windows_output'),
                            ('lookup_selftest','lookup-tests.json','output')):
        proof=json.loads((OUT/report).read_text())
        for path,digest in proof['inputs'].items(): assert sha(Path(path))==digest
        path=OUT/name
        assert sha(path)==proof['binaries'][str(path)]
        cases.append((path,proof[key],sha(OUT/report)))
    shell(SERIAL,'mkdir',REMOTE) # A pre-existing root is not ours to remove.
    results=[]
    try:
        for path,expected,proof_hash in cases:
            remote=REMOTE+'/'+path.name
            try:
                adb(SERIAL,'push',str(path),remote)
                assert shell(SERIAL,'sha256sum',remote).split()[0]==sha(path)
                shell(SERIAL,'chmod','700',remote)
                output=shell(SERIAL,remote)
                assert output==expected.strip(),output
                results.append(dict(test=path.name,binary_sha256=sha(path),proof_sha256=proof_hash,output=output))
                print(output)
            finally:
                shell(SERIAL,'rm','-f',remote)
    finally:
        shell(SERIAL,'rmdir',REMOTE)
    assert installed()==before
    result=dict(serial=SERIAL,model='SM-S906E',cases=results,installed_apk_unchanged=before,
                temporary_executables_removed=True,thermal_before=thermal,
                thermal_after=shell(SERIAL,'dumpsys','thermalservice'),android_game_fps_evidence=False)
    (OUT/'android-native.json').write_text(json.dumps(result,indent=2))
    print('Standalone correctness only; APK unchanged, temporary files removed.')
if __name__=='__main__':main()
