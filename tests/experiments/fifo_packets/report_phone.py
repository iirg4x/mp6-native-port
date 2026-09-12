"""Verify frozen normal-speed windows; a single pair is not a proven FPS gain."""
import hashlib
import json
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/s22-fifo-packets-20260912'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    rows=[]
    for role in ('baseline','candidate'):
        run=OUT/(role+'-normal-off-1')
        frozen=json.loads((run/'frozen.json').read_text())
        proof=frozen['input_evidence']
        for name,key in (('logcat.txt','logcat_sha256'),('request.json','request_sha256'),
                         (frozen['resume_marker'],'resume_sha256'),(proof['thermal_window'],'thermal_sha256')):
            assert sha(run/name)==proof[key],name
        assert all(status==0 for status in frozen['thermal_statuses'])
        assert sha(OUT/('apk-'+role)/'profile.apk')==frozen['apk_sha256']
        rows.append(dict(role=role,run=run.name,frozen_sha256=sha(run/'frozen.json'),**frozen))
    assert rows[0]['args']==rows[1]['args'],'Different launch settings'
    candidate=json.loads((ROOT/'build/fifo-packets-20260912/android/provenance.json').read_text())
    native=str(ROOT/'build/fifo-packets-20260912/android/stripped/libmp6game.so')
    assert candidate['artifacts'][native]==rows[1]['native_sha256']==sha(Path(native))
    expected=ROOT/'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'
    assert sha(expected)==rows[0]['native_sha256']
    baseline=rows[0]['metrics']['presents_hz']['median']
    measured=rows[1]['metrics']['presents_hz']['median']
    report=dict(adopted=False,android_fps_gain_proven=False,rows=rows,
        observed_median_difference_percent=(measured/baseline-1)*100,
        limitations='One short baseline/candidate pair, not repeated or sustained. Same normal-speed 60 Hz simulation, unlocked rendering, FXAA, AO off, resolution and thermal admission limits. Temperatures/clocks are not locked; rates are rendered presents, not display refresh or game speed. No Mali or AO-on claim.')
    (OUT/'comparison.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({row['role']:row['metrics']['presents_hz'] for row in rows},indent=2))
    print('No repeatable Android FPS gain established; candidate remains unadopted.')
if __name__=='__main__':main()
