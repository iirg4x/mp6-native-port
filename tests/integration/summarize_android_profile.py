"""Summarize warmed board timing logs and optional app-only simpleperf data."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/tablet-performance-20260910'
NDK = ROOT/'build/android-sdk/ndk/27.3.13750724'


def thermal_evidence(run, start_time):
    """Attach only the measurement belonging to this launch/resume, not later captures."""
    if start_time is None:
        return dict(available=False), {}
    later = [json.loads(path.read_text()).get('time') for path in run.glob('resume-*.json')]
    end_time = min((stamp for stamp in later if stamp is not None and stamp > start_time), default=float('inf'))
    matches = []
    for path in run.glob('thermal-window-*.json'):
        data = path.read_bytes()
        window = json.loads(data)
        samples = window.get('samples', [])
        if samples and start_time <= samples[0]['time'] < end_time:
            matches.append((path, data, window))
    if not matches:
        return dict(available=False), {}
    if len(matches) != 1:
        return dict(available=True, valid=False, reason='multiple thermal windows for one resume'), {}
    path, data, window = matches[0]
    statuses = [sample['status'] for sample in window['samples']]
    limit = window.get('max_allowed_status', 1) # Legacy harness stopped at status 2.
    valid = (limit in (0, 1) and all(type(status) is int and 0 <= status <= limit for status in statuses)
             and window.get('thermal_comparison_valid', True) is True
             and window.get('reason') == 'bounded window completed')
    return (dict(available=True, valid=valid, statuses=statuses, max_allowed_status=limit,
                 reason=window.get('reason'), scope='Thermal limits only; not proof of equal GPU/CPU clocks'),
            dict(thermal_window=path.name, thermal_sha256=hashlib.sha256(data).hexdigest()))


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    parser.add_argument('--symbols', action='store_true')
    resume_choice = parser.add_mutually_exclusive_group()
    resume_choice.add_argument('--latest-resume', action='store_true', help='Use only the latest resumed board, after another 300 ticks')
    resume_choice.add_argument('--resume-marker', help='Exact resume-N.json in this run; keeps later screenshot resumes out of the comparison')
    parser.add_argument('--session', default='tablet-performance-20260910')
    args = parser.parse_args()
    assert re.fullmatch(r'[A-Za-z0-9_-]+', args.name)
    assert re.fullmatch(r'[A-Za-z0-9_-]+', args.session)
    OUT = ROOT/'build'/args.session
    run = OUT/args.name
    log_bytes = (run/'logcat.txt').read_bytes()
    text = log_bytes.decode(errors='replace')
    input_evidence = {'logcat_sha256': hashlib.sha256(log_bytes).hexdigest()}
    event = re.search(r'\[EVENT\] w01.live=.*?tick=(\d+)', text)
    first = int(event[1])+600 if event else 10**9
    resumed = args.latest_resume or args.resume_marker is not None
    start_time = None
    if resumed:
        if args.resume_marker is not None:
            if not re.fullmatch(r'resume-[0-9]+\.json', args.resume_marker):
                parser.error('--resume-marker must name a resume-N.json in this run')
            resume_path = run/args.resume_marker
        else:
            resumes = sorted(run.glob('resume-*.json'))
            if not resumes:
                raise RuntimeError('no recorded resume; cannot infer a resumed measurement window')
            resume_path = resumes[-1]
        resume_bytes = resume_path.read_bytes()
        resume = json.loads(resume_bytes)
        start_time = resume.get('time')
        input_evidence['resume_marker'] = resume_path.name
        input_evidence['resume_sha256'] = hashlib.sha256(resume_bytes).hexdigest()
        first = max(first, resume['last_reported_tick']+300)
    tick = 0
    window_start = 0
    series = defaultdict(list)
    for line in text.splitlines():
        if '[MP6-TICKRATE]' in line:
            match = re.search(r'tick=(\d+)', line)
            count = re.search(r'\bticks=(\d+)', line)
            if match:
                tick = int(match[1])
                window_start = tick-int(count[1]) if count else 0
        # A five-second report can END after warmup while still containing
        # loading/camera-transition frames. Admit only whole warmed windows.
        if window_start < first:
            continue
        for key, pattern in (
            ('tick_hz', r'\[MP6-TICKRATE\].*rate=([0-9.]+)'),
            ('presents_hz', r'\[MP6-PRESENTRATE\].*rate=([0-9.]+)'),
            ('gpu_span_ms', r'\[MP6-GPU\] span=([0-9.]+)')):
            m = re.search(pattern, line)
            if m:
                series[key].append(float(m[1]))
        m = re.search(r'\[MP6-GPU\] (.+)=([0-9.]+) ms$', line)
        if m:
            series['gpu_'+m[1]].append(float(m[2]))
        m = re.search(r'phase-avg-ms\(game=([\d.]+) endframe=([\d.]+) seal=([\d.]+) vipost=([\d.]+)', line)
        if m:
            for key, value in zip(('game_ms', 'submit_ms', 'seal_ms', 'post_ms'), m.groups()):
                series[key].append(float(value))
    result = {key: dict(n=len(v), mean=round(statistics.mean(v), 3), median=round(statistics.median(v), 3),
                       p10=round(sorted(v)[min(len(v)-1, int(len(v)*.1))], 3),
                       p90=round(sorted(v)[min(len(v)-1, int(len(v)*.9))], 3))
              for key, v in series.items() if v and any(v)}
    result['buffer_queue_errors'] = text.count('err=NO_BUFFER_AVAILABLE')
    result['measurement_window'] = dict(first_tick=first if event else None,
        full_tick_windows_only=True,
        board_ready=event is not None, resumed=resumed, latest_resume=args.latest_resume,
        valid=bool(event and series['presents_hz'] and series['gpu_span_ms']),
        scope='Asynchronous diagnostic windows, not necessarily independent frames or thermally matched runs')
    request_path = run/'request.json'
    if request_path.exists():
        request_bytes = request_path.read_bytes()
        input_evidence['request_sha256'] = hashlib.sha256(request_bytes).hexdigest()
        request = json.loads(request_bytes)
        if not resumed:
            start_time = request.get('start_time')
        if 'diagnostic' in request:
            result['diagnostic'] = request['diagnostic']
            result['shipping_performance_evidence'] = False
            marker = '[MP6-COMPOSITE-PROBE] skip=1 local-diagnostic-only'
            result['diagnostic_confirmed'] = marker in text
            result['measurement_window']['valid'] &= result['diagnostic_confirmed']
    thermal, thermal_hashes = thermal_evidence(run, start_time)
    result['measurement_window']['thermal'] = thermal
    if thermal['available']:
        result['measurement_window']['valid'] &= thermal['valid']
    input_evidence.update(thermal_hashes)
    result['input_evidence'] = input_evidence
    (run/('summary-resume.json' if resumed else 'summary.json')).write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    if args.symbols:
        sys.path.insert(0, str(NDK/'simpleperf'))
        from simpleperf_report_lib import ReportLib
        lib = ReportLib()
        lib.SetRecordFile(str(run/'perf.data'))
        paths = set()
        while lib.GetNextSample():
            symbol = lib.GetSymbolOfCurrentSample()
            if symbol.dso_name.endswith('/libmp6game.so'):
                paths.add(symbol.dso_name)
        assert len(paths) == 1
        path = paths.pop()
        build_id = lib.GetBuildIdForPath(path).lower().removeprefix('0x').rstrip('0')
        native = ROOT/'build/android/aurora/libmp6game.so'
        elf = subprocess.check_output([str(NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'),
                                       '-n', str(native)], text=True)
        assert re.search(r'Build ID: ([0-9a-f]+)', elf)[1].rstrip('0') == build_id
        symbols = OUT/'symbols'
        target = symbols/path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            shutil.copy2(native, target)
        (symbols/'build_id_list').write_bytes((lib.GetBuildIdForPath(path)+'='+target.relative_to(symbols).as_posix()+'\n').encode())
        command = [sys.executable, str(NDK/'simpleperf/report.py'), '-i', str(run/'perf.data'),
                   '--symfs', str(symbols), '--sort', 'comm,dso,symbol', '--percent-limit', '0.7']
        for label, extra in (('self', []), ('children', ['--children'])):
            report = subprocess.check_output(command+extra, text=True)
            (run/('cpu-'+label+'.txt')).write_text(report)
            print(report[:10000])


if __name__ == '__main__':
    main()
