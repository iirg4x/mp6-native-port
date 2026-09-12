"""Bind the S22 correctness-only run to its APK, image, logs and thermal limits."""
import hashlib
import json
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT/'build/s22-render-bundles-20260912'
EXPERIMENT = ROOT/'build/render-bundles-20260912'
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path): return json.loads(path.read_text())
def main():
    desktop = read(EXPERIMENT/'r1-verification.json')
    assert desktop['exact_frames_passed'] and not desktop['android_fps_gain_proven']
    records = []
    for role,name in (('baseline','ff-baseline-1'),('candidate','ff-candidate-smoke')):
        run = OUT/name
        request = read(run/'request.json')
        apk = read(OUT/('apk-'+role)/'provenance.json')
        assert request['apk_sha256'] == apk['sha256'] == sha(OUT/('apk-'+role)/'profile.apk')
        expected_native = (ROOT/'build/android/aurora/libmp6game.so' if role == 'baseline' else
                           EXPERIMENT/'android-r1/libmp6game.so')
        assert Path(apk['native_override']).resolve() == expected_native.resolve()
        assert 'MP6_TICK_HZ=0' in request['args'] and 'MP6_ENH_AMBIENT_OCCLUSION=0' in request['args']
        assert 'MP6_FXAA=1' in request['args'] and 'MP6_VSYNC=0' in request['args']
        thermal = [read(p) for p in sorted(run.glob('thermal-window-*.json'))]
        assert len(thermal) == 1
        if role == 'baseline':
            assert thermal[0]['max_allowed_status'] == 0
            assert not thermal[0]['thermal_comparison_valid']
        else:
            assert thermal[0]['max_allowed_status'] == 1
            assert request['thermal_admission']['max_status'] == 1
        log = (run/'logcat.txt').read_text()
        assert '[EVENT] w01.live=' in log
        assert not any(marker in log for marker in ('[FATAL]', '[AURORA FATAL', '[MP6-CRASH]', 'Fatal signal'))
        record = dict(role=role,run=name,request_sha256=sha(run/'request.json'),apk_sha256=apk['sha256'],
            native_sha256=sha(expected_native),log_sha256=sha(run/'logcat.txt'),thermal=thermal,
            workload='Fast Forward, not normal-speed unlocked FPS',fps_eligible=False,
            exclusion=('strict cool baseline window reached thermal status 1' if role=='baseline' else
                       'predeclared correctness-only smoke with elevated thermal admission'))
        if role == 'candidate':
            counts = [dict(zip(('frames','draws','hit_draws','build_draws','miss_draws','rejected_draws'),map(int,m.groups())))
                for m in re.finditer(r'\[GX-BUNDLES\] frames=(\d+) draws=(\d+) hit_draws=(\d+) build_draws=(\d+) miss_draws=(\d+) rejected_draws=(\d+)',log)]
            assert len(counts) > 6
            delta = {k:counts[-1][k]-counts[-6][k] for k in counts[-1]}
            assert delta['frames'] == 600 and delta['draws'] > 0
            delta['reused_draw_percent'] = 100*delta['hit_draws']/delta['draws']
            record['last_600_recorded_frames'] = delta
            images = sorted(run.glob('screen-*.png'))
            assert len(images) == 1
            record['screen'] = dict(path=str(images[0]),sha256=sha(images[0]),
                visually_reviewed=True,scope='W01 welcome dialog and four players; not full-match or pixel-equivalence proof')
        records.append(record)
    result = dict(adopted=False,android_fps_gain_proven=False,eligible_pairs=0,
        reason='The baseline failed the strict thermal window; the candidate was deliberately correctness-only.',
        runs=records,desktop_proof_sha256=sha(EXPERIMENT/'r1-verification.json'),
        api_validation_sha256=sha(EXPERIMENT/'api-validation.json'),
        cache_fixture_sha256=sha(OUT/'native-cache-test.json'),
        distribution_upload=False,submitted_to_google=False)
    (OUT/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(eligible_fps_pairs=0,android_fps_gain_proven=False,
        candidate_reuse=records[-1]['last_600_recorded_frames']),indent=2))
if __name__ == '__main__': main()
