"""Name the functions in an Android MP6_SAMPLE_PROFILE report.

The phone build prints "[MP6-SAMPLE] <self|game> <count> <library> <offset>"
lines to logcat (src/host/host_android.c). This resolves the libmp6game.so
offsets with the NDK's llvm-symbolizer against the unstripped library the
APK was built from, and prints per-function totals:

    python tools/android_sample_report.py logcat.txt [--lib build/android/aurora/libmp6game.so]

"self" is where the game thread was; "game" charges time spent in other
libraries (libc, Dawn, the driver) to the first libmp6game.so caller.
"""
import argparse
from collections import Counter, defaultdict
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

LINE = re.compile(r'\[MP6-SAMPLE\] (self|game) (\d+) (\S+) 0x([0-9a-f]+)')
GAME_LIB = 'libmp6game.so'


def symbolizer():
    ndk = build._find_ndk_root()
    if not ndk:
        raise SystemExit('Android NDK not found (see tools/build.py)')
    host = 'windows-x86_64' if sys.platform == 'win32' else 'linux-x86_64'
    suffix = '.exe' if sys.platform == 'win32' else ''
    return str(Path(ndk) / 'toolchains/llvm/prebuilt' / host / 'bin' / ('llvm-symbolizer' + suffix))


def function_names(lib, offsets):
    """offset -> function name, one llvm-symbolizer run for all of them."""
    if not offsets:
        return {}
    ordered = sorted(offsets)
    out = subprocess.run([symbolizer(), '--obj', str(lib), '--functions=linkage', '--no-inlines',
                          '--output-style=GNU'],
                         input='\n'.join(hex(o) for o in ordered) + '\n',
                         capture_output=True, text=True, check=True).stdout
    # GNU style prints two lines per address: the function, then file:line.
    lines = out.splitlines()
    return {offset: lines[2 * i] if 2 * i < len(lines) else '??' for i, offset in enumerate(ordered)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logcat')
    parser.add_argument('--lib', default=str(ROOT / 'build/android/aurora' / GAME_LIB))
    parser.add_argument('--top', type=int, default=25)
    args = parser.parse_args()
    rows = defaultdict(list)
    header = ''
    hitches = []
    for line in Path(args.logcat).read_text(encoding='utf-8', errors='replace').splitlines():
        if '[MP6-SAMPLE] ticks' in line:
            header = line[line.index('[MP6-SAMPLE]'):]
        if '[MP6-SAMPLE] hitch tick' in line:
            hitches.append(line[line.index('[MP6-SAMPLE]'):])
        m = LINE.search(line)
        if m:
            rows[m.group(1)].append((int(m.group(2)), m.group(3), int(m.group(4), 16)))
    if not rows:
        raise SystemExit('no [MP6-SAMPLE] lines (run with MP6_SAMPLE_PROFILE=<first>-<last>)')
    offsets = {o for kind in rows.values() for _, lib, o in kind if lib == GAME_LIB}
    names = function_names(args.lib, offsets)
    print(header)
    for hitch in hitches:
        print(hitch)
    for kind in ('self', 'game'):
        totals = Counter()
        for count, lib, offset in rows[kind]:
            totals[names.get(offset, '??') if lib == GAME_LIB else f'[{lib}]'] += count
        total = sum(totals.values())
        print(f'{kind}: {total} samples')
        for name, count in totals.most_common(args.top):
            print(f'  {count:6d} {100.0 * count / max(total, 1):5.1f}%  {name}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
