"""Bind the ready-memo implementation to its real private builds and PC frames."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/ready-pipeline-memo-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from compare_ao_captures import frame
spec=importlib.util.spec_from_file_location('memo_build',Path(__file__).with_name('build_renderer.py'))
builder=importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    proofs={}
    for platform in ('windows','android'):
        directory=OUT/platform
        proof=json.loads((directory/'provenance.json').read_text())
        assert not proof['adopted'] and proof['experimental_only']
        for path,digest in {**proof['production_unchanged'],**proof['artifacts']}.items():
            assert sha(path)==digest,path
        assert sha(directory/'renderer.patch')==proof['patch_sha256']
        assert sha(OUT/'native-tests.json')==proof['native_test_proof_sha256']
        source=ROOT/'build'/('aurora-release-source' if platform=='windows' else 'android-aurora-source')
        name='lib/gfx/pipeline_cache.cpp'
        assert (directory/'source'/name).read_text()==builder.candidate((source/name).read_text())
        command=proof['compile']
        assert command.count('-O2')==1 and '-DNDEBUG' in command
        assert not any(flag in command for flag in ('-ffast-math','-Ofast'))
        proofs[platform]=dict(provenance_sha256=sha(directory/'provenance.json'),artifacts=proof['artifacts'])
    rows=[]
    for ao,reference in ((0,'compact-instance-final-off-baseline'),(1,'compact-instance-ao-baseline')):
        images={}; records={}
        for role,name in (('baseline',reference),('candidate','ready-memo-board-'+('ao1' if ao else 'off'))):
            run=ROOT/'build/board-qa-runs'/name
            result=json.loads((run/'result.json').read_text())
            records[role]=result['request']
            if role=='candidate':
                expected=sha(OUT/'windows/release/mp6native.exe')
                assert expected in [v for k,v in result.items() if 'sha256' in k]
            log=(run/'game.log').read_text()
            assert not any(marker in log for marker in ('[FATAL]','[AURORA FATAL','[MP6-CRASH]'))
            live=int(re.search(r'\[EVENT\] w01\.live=\S+ num=\d+ tick=(\d+)',log)[1])
            images[role]={}
            for path in sorted((run/'frames').glob('*.mfd')):
                tick,pixels=frame(path)
                assert tick-live not in images[role]
                images[role][tick-live]=(path,pixels)
        ignored={'exe','name','seconds','capture_frames'}
        assert {k:v for k,v in records['baseline'].items() if k not in ignored}=={
            k:v for k,v in records['candidate'].items() if k not in ignored}
        pairs=[]
        for age,(path,pixels) in images['candidate'].items():
            ref,baseline=images['baseline'][age]
            assert baseline.shape==pixels.shape and np.array_equal(baseline,pixels),(ao,age)
            pairs.append(dict(age=age,changed_pixels=0,baseline_sha256=sha(ref),candidate_sha256=sha(path)))
            if len(pairs)==1: Image.fromarray(pixels).save(OUT/('ao1-verified.png' if ao else 'off-verified.png'))
        assert len(pairs)==6
        rows.append(dict(ao=ao,pairs=pairs))
    result=dict(adopted=False,production_unchanged=True,platforms=proofs,captures=rows,
        android_fps_gain_proven=False,
        scope='Twelve exact fixed-seed PC W01 frames, not Android/FPS/full-game/restart GPU certification.')
    (OUT/'renderer-verification.json').write_text(json.dumps(result,indent=2))
    print('PASS: private optimized renderers; 12 exact PC frame pairs; production unchanged.')

if __name__=='__main__': main()
