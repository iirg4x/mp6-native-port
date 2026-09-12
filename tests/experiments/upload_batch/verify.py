"""Exact fixed-board image and Release provenance gates; no FPS inference."""
import hashlib
import json
from pathlib import Path
import re
import sys
import numpy as np

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tests/integration'))
from compare_ao_captures import frame


def main():
    rows=[]
    for platform in ('windows','android'):
        proof=json.loads((ROOT/f'build/upload-batch-20260912/{platform}-r1/provenance.json').read_text())
        for path,digest in {**proof['artifacts'],**proof['production_unchanged']}.items():
            assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest,path
        for command in proof['compile']:
            assert '-O2' in command and '-DNDEBUG' in command
            assert not any(x in command for x in ('-Ofast','-ffast-math'))
    for ao,reference in ((0,'compact-instance-final-off-baseline'),(1,'compact-instance-ao-baseline')):
        images={}; requests={}
        for role,name in (('control',reference),('candidate',f'upload-batch-r1-ao{ao}-mode1')):
            run=ROOT/'build/board-qa-runs'/name
            result=json.loads((run/'result.json').read_text()); requests[role]=result['request']
            log=(run/'game.log').read_text()
            assert not any(s in log for s in ('[FATAL]','[AURORA FATAL','[MP6-CRASH]','Validation Error'))
            live=int(re.search(r'\[EVENT\] w01.live=\S+ num=\d+ tick=(\d+)',log)[1])
            images[role]={}
            for path in sorted((run/'frames').glob('*.mfd')):
                tick,pixels=frame(path)
                assert tick-live not in images[role]
                images[role][tick-live]=pixels
        ignored={'exe','name','seconds','capture_frames','frame_upload_mode'}
        assert {k:v for k,v in requests['control'].items() if k not in ignored}=={
            k:v for k,v in requests['candidate'].items() if k not in ignored}
        assert len(images['candidate'])==6
        for age,pixels in images['candidate'].items():
            original=images['control'][age]
            assert pixels.shape==original.shape
            rows.append(dict(ao=ao,age=age,changed_pixels=int(np.count_nonzero(np.any(pixels!=original,axis=2)))))
    passed=all(r['changed_pixels']==0 for r in rows)
    (ROOT/'build/upload-batch-20260912/image-verification.json').write_text(json.dumps(
        dict(passed=passed,pairs=rows,production_unchanged=True,android_fps_evidence=False),indent=2))
    print(json.dumps(dict(passed=passed,pairs=rows),indent=2))
    assert passed


if __name__=='__main__': main()
