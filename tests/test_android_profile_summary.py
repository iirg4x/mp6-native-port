"""Prevent startup or partial warmup windows becoming mobile FPS evidence."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parent/'integration'))
import summarize_android_profile as summary


class AndroidProfileSummary(unittest.TestCase):
    def test_thermal_evidence_uses_exact_resume_and_rejects_over_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            run=Path(tmp)
            (run/'resume-1.json').write_text(json.dumps(dict(time=10)))
            (run/'resume-2.json').write_text(json.dumps(dict(time=50)))
            window=dict(samples=[dict(time=11,status=0),dict(time=31,status=1)],
                        max_allowed_status=0,thermal_comparison_valid=True,reason='bounded window completed')
            (run/'thermal-window-1.json').write_text(json.dumps(window))
            (run/'thermal-window-2.json').write_text(json.dumps({**window,'samples':[dict(time=51,status=0)]}))
            result,evidence=summary.thermal_evidence(run,10)
            self.assertFalse(result['valid']) # Recompute; never trust an inconsistent saved boolean.
            self.assertEqual(result['statuses'],[0,1])
            self.assertEqual(evidence['thermal_window'],'thermal-window-1.json')
            self.assertTrue(summary.thermal_evidence(run,50)[0]['valid'])
            (run/'thermal-window-3.json').write_text(json.dumps(window))
            self.assertFalse(summary.thermal_evidence(run,10)[0]['valid'])
            self.assertEqual(summary.thermal_evidence(run,None)[0],dict(available=False))

    def summarize(self, log, resume=None, request=None, marker=None, later_resume=None):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            run=root/'build/session/run'
            run.mkdir(parents=True)
            (run/'logcat.txt').write_bytes(log.encode())
            if request is not None:
                (run/'request.json').write_text(json.dumps(request))
            args=['summary','run','--session','session']
            if resume is not None:
                (run/'resume-1.json').write_text(json.dumps({'last_reported_tick':resume}))
                if marker is None:
                    args.append('--latest-resume')
            if marker is not None:
                args += ['--resume-marker', marker]
            if later_resume is not None:
                (run/'resume-2.json').write_text(json.dumps({'last_reported_tick':later_resume}))
            with patch.object(summary,'ROOT',root),patch.object(sys,'argv',args),contextlib.redirect_stdout(io.StringIO()):
                summary.main()
            return json.loads((run/('summary-resume.json' if resume is not None or marker is not None else 'summary.json')).read_text())

    def test_only_whole_warmed_windows_count(self):
        log='[EVENT] w01.live=board tick=100\n'
        for tick,count,rate in ((650,250,300),(750,250,250),(1000,250,190),(1250,250,192)):
            log+=f'[MP6-TICKRATE] ticks={count} rate={rate} tick={tick}\n'
            log+=f'[MP6-PRESENTRATE] rate={rate}\n[MP6-GPU] span=3.2\n'
        result=self.summarize(log)
        self.assertEqual(result['presents_hz']['n'],2)
        self.assertEqual(result['presents_hz']['median'],191)
        self.assertTrue(result['measurement_window']['full_tick_windows_only'])
        self.assertTrue(result['measurement_window']['valid'])

    def test_resume_warmup_and_unknown_window_are_not_admitted(self):
        log='[EVENT] w01.live=board tick=100\n'
        log+='[MP6-TICKRATE] ticks=200 rate=300 tick=1400\n[MP6-PRESENTRATE] rate=300\n'
        log+='[MP6-TICKRATE] rate=500 tick=1600\n[MP6-PRESENTRATE] rate=500\n'
        result=self.summarize(log,resume=1000)
        self.assertFalse(result['measurement_window']['valid'])
        self.assertNotIn('presents_hz',result)

    def test_ablation_is_labeled_and_requires_native_confirmation(self):
        log='[EVENT] w01.live=board tick=100\n'
        log+='[MP6-TICKRATE] ticks=200 rate=300 tick=1400\n'
        log+='[MP6-PRESENTRATE] rate=300\n[MP6-GPU] span=3.2\n'
        request={'diagnostic':'AO composite ablation'}
        result=self.summarize(log,request=request)
        self.assertFalse(result['measurement_window']['valid'])
        self.assertFalse(result['shipping_performance_evidence'])
        confirmed=self.summarize('[MP6-COMPOSITE-PROBE] skip=1 local-diagnostic-only\n'+log,request=request)
        self.assertTrue(confirmed['measurement_window']['valid'])
        self.assertEqual(confirmed['diagnostic'],request['diagnostic'])

    def test_explicit_marker_does_not_select_later_visual_resume(self):
        log='[EVENT] w01.live=board tick=100\n'
        log+='[MP6-TICKRATE] ticks=200 rate=190 tick=1500\n'
        log+='[MP6-PRESENTRATE] rate=190\n[MP6-GPU] span=3.2\n'
        result=self.summarize(log,resume=1000,marker='resume-1.json',later_resume=2000)
        self.assertTrue(result['measurement_window']['valid'])
        self.assertEqual(result['presents_hz']['median'],190)
        self.assertEqual(result['input_evidence']['resume_marker'],'resume-1.json')
        self.assertEqual(result['input_evidence']['logcat_sha256'],
                         hashlib.sha256(log.encode()).hexdigest())
        self.assertFalse(self.summarize(log,resume=1000,later_resume=2000)['measurement_window']['valid'])

    def test_marker_cannot_escape_run(self):
        with self.assertRaises(SystemExit),contextlib.redirect_stderr(io.StringIO()):
            self.summarize('',marker='../resume-1.json')

    def test_missing_marker_is_not_silently_replaced(self):
        with self.assertRaises(FileNotFoundError):
            self.summarize('',resume=1000,marker='resume-3.json')


if __name__=='__main__':
    unittest.main()
