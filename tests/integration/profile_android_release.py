"""Capture and symbolize an owned Release-native MP6 run, without experimental code."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from android_device_profile import adb, shell, mp6_is_foreground, ROOT, PACKAGE

NDK = ROOT / 'build/android-sdk/ndk/27.3.13750724'
SERIAL = 'RZCTB00SF3W'
REMOTE = '/data/local/tmp/mp6-s22-simpleperf-20260911'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def report(run):
    sys.path.insert(0, str(NDK / 'simpleperf'))
    from simpleperf_report_lib import ReportLib
    proof = json.loads((run / 'cpu-provenance.json').read_text())
    native = Path(proof['unstripped_elf'])
    assert sha(native) == proof['native_sha256']
    assert sha(run / 'perf.data') == proof['perf_sha256']
    reader = ReportLib()
    reader.SetRecordFile(str(run / 'perf.data'))
    reader.ShowIpForUnknownSymbol()
    samples, addresses, paths = [], set(), set()
    while sample := reader.GetNextSample():
        leaf = reader.GetSymbolOfCurrentSample()
        chain = reader.GetCallChainOfCurrentSample()
        frames = []
        for item in [leaf] + [chain.entries[i].symbol for i in range(chain.nr)]:
            if item.dso_name.endswith('/libmp6game.so'):
                paths.add(item.dso_name)
                key = hex(item.vaddr_in_file)
                addresses.add(key)
            else:
                key = item.symbol_name + ' [' + Path(item.dso_name).name + ']'
            frames.append(key)
        samples.append((sample.period, sample.tid, sample.thread_comm, sample.cpu, frames))
    assert len(paths) == 1 and samples
    recorded = reader.GetBuildIdForPath(paths.pop()).removeprefix('0x').lower()
    reader.Close()
    llvm = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    notes = subprocess.check_output([str(llvm / 'llvm-readelf.exe'), '-n', str(native)], text=True)
    actual = re.search(r'Build ID: ([0-9a-f]+)', notes)[1]
    assert recorded.startswith(actual) and not recorded[len(actual):].strip('0')
    ordered = sorted(addresses)
    output = subprocess.run([str(llvm / 'llvm-addr2line.exe'), '-f', '-C', '-e', str(native)],
                            input='\n'.join(ordered) + '\n', capture_output=True, text=True, check=True)
    lines = output.stdout.splitlines()
    assert len(lines) == 2 * len(ordered)
    names = {address: lines[i * 2] for i, address in enumerate(ordered)}
    own, inclusive, threads, stacks = Counter(), Counter(), Counter(), Counter()
    per_thread, per_core = {}, {}
    for weight, tid, comm, cpu, frames in samples:
        resolved = tuple(names.get(key, key) for key in frames)
        own[resolved[0]] += weight
        for name in set(resolved):
            inclusive[name] += weight
        threads[(tid, comm)] += weight
        per_core.setdefault((tid, comm), Counter())[cpu] += weight
        stacks[resolved[:12]] += weight
        target = per_thread.setdefault((tid, comm), Counter())
        for name in set(resolved):
            target[name] += weight
    total = sum(threads.values())
    def rows(counter, denominator, limit):
        return [dict(symbol=name, percent=100 * weight / denominator)
                for name, weight in counter.most_common(limit)]
    result = dict(samples=len(samples), native_build_id=actual, fps_evidence=False,
        scope='Active app user-space CPU cycles. Inclusive percentages overlap; not wall time.',
        self=rows(own, total, 50), inclusive=rows(inclusive, total, 80),
        stacks=[dict(frames=list(names), percent=100 * weight / total)
                for names, weight in stacks.most_common(60)],
        threads=[dict(tid=tid, name=comm, percent=100 * weight / total,
                      inclusive=rows(per_thread[(tid, comm)], weight, 40),
                      sampled_cycles_by_cpu=rows(per_core[(tid, comm)], weight, 16))
                 for (tid, comm), weight in threads.most_common()])
    (run / 'cpu-report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(samples=len(samples), self=result['self'][:12],
        threads=[{**t, 'inclusive': t['inclusive'][:12]} for t in result['threads'][:2]]), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('capture', 'report'))
    parser.add_argument('--session', required=True)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    assert all(re.fullmatch('[a-z0-9-]+', s) for s in (args.session, args.name))
    session = ROOT / 'build' / args.session
    run = session / args.name
    if args.action == 'report':
        report(run)
        return
    request = json.loads((run / 'request.json').read_text())
    wrapper = json.loads((session / 'apk-baseline/provenance.json').read_text())
    staged = ROOT / 'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'
    native = ROOT / 'build/android/aurora/libmp6game.so'
    assert Path(wrapper['native_override']).resolve() == staged.resolve()
    assert sha(staged) == 'da3a5d72f8944b6c1d665d66f70bae50681518f40200b7b47824008dd1bc7a48'
    assert sha(native) == 'd3f539a81e1929ac7c1354f9d4f25460d484f5c936599d75f4bc950ae6baab70'
    assert shell(SERIAL, 'getprop', 'ro.product.model') == 'SM-S906E'
    assert request['serial'] == SERIAL and shell(SERIAL, 'pidof', PACKAGE) == request['pid']
    package = shell(SERIAL, 'pm', 'path', PACKAGE).removeprefix('package:')
    assert package.startswith('/data/app/') and '\n' not in package
    assert shell(SERIAL, 'sha256sum', package).split()[0] == wrapper['sha256'] == request['apk_sha256']
    assert not (run / 'perf.data').exists() and mp6_is_foreground(SERIAL)
    assert adb(SERIAL, 'shell', 'test -e ' + REMOTE, check=False).returncode != 0
    tool = NDK / 'simpleperf/bin/android/arm64/simpleperf'
    expected = sha(tool)
    adb(SERIAL, 'push', str(tool), REMOTE)
    try:
        assert shell(SERIAL, 'sha256sum', REMOTE).split()[0] == expected
        shell(SERIAL, 'chmod', '700', REMOTE)
        subprocess.run([sys.executable, str(ROOT / 'tests/integration/capture_android_cpu_profile.py'),
                        '--serial', SERIAL, '--model', 'SM-S906E', '--session', args.session,
                        '--name', args.name], cwd=ROOT, check=True)
        proof = dict(apk_sha256=wrapper['sha256'], native_sha256=sha(native),
                     staged_sha256=sha(staged), perf_sha256=sha(run / 'perf.data'),
                     tool_sha256=expected, unstripped_elf=str(native), fps_evidence=False,
                     experimental_native_code=False, profiling_changes_timing=True)
        (run / 'cpu-provenance.json').write_text(json.dumps(proof, indent=2) + '\n')
    finally:
        assert shell(SERIAL, 'sha256sum', REMOTE).split()[0] == expected
        shell(SERIAL, 'rm', REMOTE)
    report(run)


if __name__ == '__main__':
    main()
