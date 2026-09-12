"""Check actual API-validated runs, without treating them as FPS evidence."""
import hashlib
import json
from pathlib import Path
import re
import sys
import numpy as np
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT/'build/render-bundles-20260912'
sys.path.insert(0,str(ROOT/'tests/integration'))
from compare_ao_captures import frame
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    proof = json.loads((OUT/'windows-validation/provenance.json').read_text())
    for path,digest in {**proof['artifacts'],**proof['production_unchanged']}.items():
        assert sha(path) == digest,path
    assert proof['validation_only'] and proof['not_for_performance']
    source = (OUT/'windows-validation/source/lib/webgpu/gpu.cpp').read_text()
    assert '"skip_validation"' not in source
    assert '"disable_robustness"' in source # No shader/config change masquerading as this test.
    assert proof['compile'].count('-O2') == 1 and '-DNDEBUG' in proof['compile']
    expected = sha(OUT/'windows-validation/release/mp6native.exe')
    rows = []
    for aa in (2,3,1):
        directory = ROOT/'build/board-qa-runs'/('render-bundles-validation-ao1-aa'+str(aa))
        result = json.loads((directory/'result.json').read_text())
        assert expected in [v for k,v in result.items() if 'sha256' in k]
        assert result['request']['aa'] == aa and result['request']['ao'] == 1
        log = (directory/'game.log').read_text()
        assert not re.search(r'\[FATAL\]|\[AURORA FATAL|\[MP6-CRASH\]|Validation Error|validation error|uncaptured error',log)
        assert 'w01.live' in log
        hits = re.findall(r'\[GX-BUNDLES\].*?hit_draws=(\d+)',log)
        assert hits and int(hits[-1]) > 10000
        paths = sorted((directory/'frames').glob('*.mfd'))
        assert len(paths) == 6
        exact = None
        if aa == 2:
            reference = sorted((ROOT/'build/board-qa-runs/render-bundles-r1-ao1/frames').glob('*.mfd'))
            assert len(reference) == len(paths)
            exact = all(np.array_equal(frame(a)[1],frame(b)[1]) for a,b in zip(paths,reference))
            assert exact
        rows.append(dict(aa=aa,ao=1,frames=6,validation_errors=0,exact_to_release=exact,
                         log_sha256=sha(directory/'game.log'),frame_sha256=[sha(p) for p in paths]))
    (OUT/'api-validation.json').write_text(json.dumps(dict(runs=rows,android_fps_gain_proven=False,
        validation_provenance_sha256=sha(OUT/'windows-validation/provenance.json')),indent=2)+'\n')
    print('PASS: 18 API-validated board captures (FXAA, SSAA 1.5x, MSAA 4x); six FXAA frames exact to Release.')
if __name__ == '__main__': main()
