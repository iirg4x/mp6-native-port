"""Verify private Release inputs, exact PC frames, and command reuse census."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT/'build/render-bundles-20260912'
sys.path.insert(0, str(ROOT/'tests/integration'))
from compare_ao_captures import frame
spec = importlib.util.spec_from_file_location('bundle_build', Path(__file__).with_name('build_candidate.py'))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', default='r1')
    args = parser.parse_args()
    platforms = {}
    for platform in ('windows','android'):
        directory = OUT/(platform+'-'+args.revision)
        proof = json.loads((directory/'provenance.json').read_text())
        assert not proof['adopted'] and proof['experimental_only']
        for path,digest in {**proof['production_unchanged'], **proof['artifacts'], **proof['authored']}.items():
            assert sha(path) == digest, path
        assert sha(directory/'renderer.patch') == proof['patch_sha256']
        source = ROOT/'build'/('aurora-release-source' if platform == 'windows' else 'android-aurora-source')
        assert (directory/'source/lib/gfx/common.cpp').read_text() == builder.candidate((source/'lib/gfx/common.cpp').read_text())
        assert sha(directory/'source/lib/gfx/bundle_cache.hpp') == sha(Path(__file__).with_name('bundle_cache.hpp'))
        command = proof['compile']
        assert command.count('-O2') == 1 and '-DNDEBUG' in command
        assert not any(flag in command for flag in ('-ffast-math', '-Ofast'))
        platforms[platform] = dict(provenance_sha256=sha(directory/'provenance.json'), artifacts=proof['artifacts'])
    rows = []
    for ao,reference in ((0,'compact-instance-final-off-baseline'), (1,'compact-instance-ao-baseline')):
        images = {}; records = {}; counts = []
        candidate = 'render-bundles-'+args.revision+'-'+('ao1' if ao else 'off')
        for role,name in (('baseline',reference),('candidate',candidate)):
            run = ROOT/'build/board-qa-runs'/name
            result = json.loads((run/'result.json').read_text())
            records[role] = result['request']
            if role == 'candidate':
                assert sha(OUT/('windows-'+args.revision)/'release/mp6native.exe') in [v for k,v in result.items() if 'sha256' in k]
            log = (run/'game.log').read_text()
            assert not any(marker in log for marker in ('[FATAL]', '[AURORA FATAL', '[MP6-CRASH]', 'Validation Error'))
            live = int(re.search(r'\[EVENT\] w01\.live=\S+ num=\d+ tick=(\d+)',log)[1])
            images[role] = {}
            for path in sorted((run/'frames').glob('*.mfd')):
                tick,pixels = frame(path)
                assert tick-live not in images[role]
                images[role][tick-live] = (path,pixels)
            if role == 'candidate':
                for match in re.finditer(r'\[GX-BUNDLES\] frames=(\d+) draws=(\d+) hit_draws=(\d+) build_draws=(\d+) miss_draws=(\d+) rejected_draws=(\d+)',log):
                    counts.append(dict(zip(('frames','draws','hit_draws','build_draws','miss_draws','rejected_draws'),map(int,match.groups()))))
        ignored = {'exe','name','seconds','capture_frames'}
        assert {k:v for k,v in records['baseline'].items() if k not in ignored} == {k:v for k,v in records['candidate'].items() if k not in ignored}
        pairs = []
        for age,(path,pixels) in images['candidate'].items():
            ref,baseline = images['baseline'][age]
            assert baseline.shape == pixels.shape
            changed = int(np.count_nonzero(np.any(pixels != baseline,axis=2)))
            pairs.append(dict(age=age,changed_pixels=changed,baseline_sha256=sha(ref),candidate_sha256=sha(path)))
            if len(pairs) == 1: Image.fromarray(pixels).save(OUT/(args.revision+('-ao1.png' if ao else '-off.png')))
        assert len(pairs) == 6
        assert len(counts) >= 6
        delta = {k:counts[-1][k]-counts[-6][k] for k in counts[-1]}
        assert delta['frames'] == 600 and delta['draws'] > 0
        delta['reused_draw_percent'] = 100*delta['hit_draws']/delta['draws']
        rows.append(dict(ao=ao,pairs=pairs,last_600_recorded_frames=delta))
    passed = all(p['changed_pixels'] == 0 for r in rows for p in r['pairs'])
    result = dict(adopted=False, production_unchanged=True, platforms=platforms,
        captures=rows, exact_frames_passed=passed, android_fps_gain_proven=False,
        scope='Twelve fixed-seed PC W01 captures and recorded-command reuse, not Android/FPS/full-game proof.')
    (OUT/(args.revision+'-verification.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(exact_frames_passed=passed, captures=12, reuse=[r['last_600_recorded_frames'] for r in rows]),indent=2))
    if not passed: raise SystemExit('Frame mismatch: candidate not acceptable')

if __name__ == '__main__': main()
