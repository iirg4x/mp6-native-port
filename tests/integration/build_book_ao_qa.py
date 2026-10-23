"""Isolated Release book QA with a real opening-dialogue capture event.

Only the generated opening translation unit receives the event. The player
binary, game source, saves and configuration are never changed. This event
avoids using present counts to guess when the book is visible during FI.
"""
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
os.environ.setdefault('MP6_DISC_ROOT', str(ROOT/'build/disc-cache/orig/GP6E01'))
os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT/'build/disc-cache/split/include'))
sys.path.insert(0, str(ROOT))
from tools import build

OUTPUT = ROOT/'build/book-ao-qa-exe'
original_configure = build.configure_windows
original_collect = build.collect_units
inputs = {}


def configure(configuration):
    original_configure(configuration)
    build.BUILD_DIR = str(OUTPUT)


def collect(*args, **kwargs):
    units = []
    matched = 0
    for source, flags, name, flavor in original_collect(*args, **kwargs):
        if Path(source).as_posix().endswith('/src/REL/bootDll/opening.c'):
            original = Path(source).read_text(encoding='utf-8')
            anchor = '    MessStart(0, OPENING_MESSAGE_BASE, 240);'
            if original.count(anchor) != 1:
                raise RuntimeError('opening dialogue anchor changed; inspect the actual phase')
            # The original BookOpen, 60-tick wait and HuWinExOpen completed.
            # Post on the game thread immediately before the first dialogue.
            text = '#include "mp6_events.h"\n' + original.replace(
                anchor, '    mp6_event_post("qa.book", 0, "dialogue");\n' + anchor)
            target = OUTPUT/'opening_qa.c'
            target.write_text(text, encoding='utf-8')
            inputs.update(source=str(source), source_sha256=hashlib.sha256(original.encode()).hexdigest(),
                          generated_sha256=hashlib.sha256(text.encode()).hexdigest(),
                          event='qa.book=dialogue', release_asset=False)
            flags = [*flags, '-iquote', str(Path(source).parent)]
            source = str(target)
            name = str(OUTPUT/'obj'/name)
            matched += 1
        units.append((source, flags, name, flavor))
    if matched != 1:
        raise RuntimeError(f'expected one opening unit, found {matched}')
    return units


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    (OUTPUT/'obj').mkdir(exist_ok=True)
    build.configure_windows = configure
    build.collect_units = collect
    sys.argv = ['tools/build.py', '--configuration', 'release', '-j4']
    result = build.main()
    if result == 0:
        exe = OUTPUT/'release/mp6native.exe'
        inputs['executable_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
        (OUTPUT/'provenance.json').write_text(json.dumps(inputs, indent=2)+'\n')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
