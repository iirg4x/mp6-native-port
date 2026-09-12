"""Verify private renderer provenance and exact, settings-matched board frames."""
import hashlib
import json
from pathlib import Path
import re
import sys
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/fifo-packets-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from compare_ao_captures import frame


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    proofs={}
    for platform in ('windows','android'):
        directory=OUT/platform
        proof=json.loads((directory/'provenance.json').read_text())
        assert proof['adopted'] is False and proof['experimental_only'] is True
        assert sha(OUT/'renderer.patch')==proof['patch_sha256']
        assert sha(OUT/'native-tests.json')==proof['native_test_proof_sha256']
        assert sha(directory/'macro-callers.json')==proof['macro_callers_sha256']
        for path,digest in {**proof['production_unchanged'],**proof['artifacts']}.items():
            assert sha(path)==digest, path
        for path,digest in proof['candidate_headers'].items():
            assert sha(directory/'source'/path)==digest, path
        assert len(proof['compile'])==16
        for command in proof['compile']:
            assert command.count('-O2')==1 and '-DNDEBUG' in command
        proofs[platform]=dict(provenance_sha256=sha(directory/'provenance.json'),artifacts=proof['artifacts'])
    rows=[]
    candidate_exe=OUT/'windows/release/mp6native.exe'
    for ao,base in ((0,'compact-instance-final-off-baseline'),(1,'compact-instance-ao-baseline')):
        runs={}
        for role,name in (('baseline',base),('candidate','fifo-packets-board-'+('ao1' if ao else 'off'))):
            run=ROOT/'build/board-qa-runs'/name
            data=json.loads((run/'result.json').read_text())
            request=data['request']
            if role=='baseline': baseline_request=request
            else:
                ignored={'exe','name','seconds','capture_frames'}
                assert {k:v for k,v in request.items() if k not in ignored}=={
                    k:v for k,v in baseline_request.items() if k not in ignored}, 'capture settings differ'
                assert (ROOT/request['exe']).resolve()==candidate_exe.resolve()
                # run_board_qa records the binary used, independently of the
                # current path. Do not credit a stale capture to a rebuilt EXE.
                hashes=[v for k,v in data.items() if 'sha256' in k and isinstance(v,str)]
                assert sha(candidate_exe) in hashes, 'capture executable hash missing or mismatched'
            log=(run/'game.log').read_text()
            assert not any(token in log for token in ('[FATAL]','[AURORA FATAL','[MP6-CRASH]'))
            live=int(re.search(r'\[EVENT\] w01\.live=\S+ num=\d+ tick=(\d+)',log)[1])
            images={}
            for path in sorted((run/'frames').glob('*.mfd')):
                tick,pixels=frame(path)
                assert tick-live not in images
                images[tick-live]=(path,pixels)
            runs[role]=(run,images)
        pairs=[]
        for age,(path,pixels) in runs['candidate'][1].items():
            bp,baseline=runs['baseline'][1][age]
            assert baseline.shape==pixels.shape
            changed=int(np.any(pixels!=baseline,axis=2).sum())
            assert changed==0,(ao,age,changed)
            pairs.append(dict(board_age=age,changed_pixels=changed,baseline_sha256=sha(bp),candidate_sha256=sha(path)))
            if len(pairs)==1: Image.fromarray(pixels).save(OUT/('ao1-verified.png' if ao else 'off-verified.png'))
        assert len(pairs)==6
        rows.append(dict(ao=ao,pairs=pairs,logs={role:sha(run/'game.log') for role,(run,_) in runs.items()}))
    result=dict(adopted=False,production_unchanged=True,platforms=proofs,captures=rows,
        android_game_fps_gain_proven=False,
        limitations='Fixed-seed PC W01 frame equivalence at matching settings; not full-game, Android visual or FPS certification.')
    (OUT/'renderer-verification.json').write_text(json.dumps(result,indent=2))
    print('PASS: 16 rebuilt GX TUs per Release platform; 12 exact PC frame pairs; production unchanged.')


if __name__=='__main__': main()
