"""Authenticate the ABBA diagnostic and require exact frames in both modes."""
import hashlib
import json
from pathlib import Path
import re
import sys
import numpy as np
from abba_report import parse
from build_abba import candidate
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/render-bundle-abba-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from compare_ao_captures import frame
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    proofs={}
    for platform in ('windows','android'):
        directory=OUT/platform
        proof=json.loads((directory/'provenance.json').read_text())
        assert proof['diagnostic_only'] and not proof['adopted']
        for path,digest in {**proof['production_unchanged'],**proof['artifacts']}.items(): assert sha(path)==digest,path
        assert sha(directory/'owner.json')==proof['owner_sha256']
        owner=json.loads((directory/'owner.json').read_text())
        for path,digest in {**owner['sources'],**owner['inputs']}.items(): assert sha(path)==digest,path
        parent=ROOT/'build/render-bundles-20260912'/(platform+'-r1')
        assert (directory/'source/lib/gfx/common.cpp').read_text()==candidate((parent/'source/lib/gfx/common.cpp').read_text())
        assert sha(directory/'source/lib/gfx/abba.hpp')==sha(Path(__file__).with_name('abba.hpp'))
        assert proof['compile'].count('-O2')==1 and '-DNDEBUG' in proof['compile']
        assert not any(x in proof['compile'] for x in ('-Ofast','-ffast-math'))
        proofs[platform]=sha(directory/'provenance.json')
    captures=[]
    for ao,reference in ((0,'compact-instance-final-off-baseline'),(1,'compact-instance-ao-baseline')):
        name='render-bundle-abba-'+('ao1' if ao else 'off')
        run=ROOT/'build/board-qa-runs'/name
        baseline=ROOT/'build/board-qa-runs'/reference
        result=json.loads((run/'result.json').read_text())
        old=json.loads((baseline/'result.json').read_text())
        ignored={'exe','name','seconds','capture_frames'}
        assert {k:v for k,v in result['request'].items() if k not in ignored}=={k:v for k,v in old['request'].items() if k not in ignored}
        assert sha(OUT/'windows/release/mp6native.exe') in [v for k,v in result.items() if 'sha256' in k]
        log=(run/'game.log').read_text()
        assert not re.search(r'\[FATAL\]|\[AURORA FATAL|\[MP6-CRASH\]|Validation Error|validation error',log)
        rows=parse(log)
        assert {r['mode'] for r in rows if r['valid'] and r['draws']>10000}=={0,1}
        assert any(r['hits']>10000 for r in rows if r['mode'])
        image_sets=[]
        for directory in (run,baseline):
            text=(directory/'game.log').read_text()
            live=int(re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)',text)[1])
            images={}
            for p in sorted((directory/'frames').glob('*.mfd')):
                tick,pixels=frame(p); assert tick-live not in images
                images[tick-live]=(p,pixels)
            assert len(images)==6 if directory==run else len(images)>=6
            image_sets.append(images)
        pairs=[]
        for age,(path,pixels) in image_sets[0].items():
            ref,other=image_sets[1][age]
            assert np.array_equal(pixels,other),(ao,age)
            pairs.append(dict(age=age,changed_pixels=0,candidate_sha256=sha(path),baseline_sha256=sha(ref)))
        captures.append(dict(ao=ao,pairs=pairs,log_sha256=sha(run/'game.log')))
    result=dict(production_unchanged=True,diagnostic_only=True,adopted=False,android_fps_gain_proven=False,
        platforms=proofs,captures=captures,scope='Twelve exact PC frames while the diagnostic alternates both modes; no desktop FPS inference.')
    (OUT/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: ABBA diagnostic authenticated; 12 exact PC frames; both modes exercised; production unchanged.')
if __name__=='__main__': main()
