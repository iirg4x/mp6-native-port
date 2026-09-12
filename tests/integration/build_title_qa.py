"""Observe the real title-attract loop in an isolated native test executable.

No overlay route, state fixture, or altered title timing is used. Only log/event
probes and a 600-tick budget after entering file select are added to generated
port-patched sources. Dependency checkouts and normal executables stay intact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f'Expected one title probe anchor: {before!r}')
    return text.replace(before, after, 1)


def boot_probe(text):
    text = ('#include "mp6_events.h"\n#include "mp6_boot.h"\n'
            '#include "mp6_anim_native.h"\n') + text
    text = replace_once(text, 'opening_exec:\n', '''opening_exec:
    {
        static int visits;
        mp6_event_post("title.opening", ++visits, "opening");
    }
''')
    marker = '    for (frame = 0; frame < TITLE_INPUT_WAIT_FRAMES; frame++) {'
    text = replace_once(text, marker, '''    {
        static int visits;
        mp6_event_post("title.ready", ++visits, "ready");
        printf("[TITLE-QA] memory visit=%d heap=%d dvd=%d model=%d anim=%zu\\n",
               visits, HuMemUsedMallocSizeGet(HEAP_HEAP),
               HuMemUsedMallocSizeGet(HEAP_DVD), HuMemUsedMallocSizeGet(HEAP_MODEL),
               mp6_anim_cache_live_count());
        fflush(stdout);
    }
''' + marker)
    marker = '            HuAudFXPlay(TITLE_CONFIRM_SE);'
    return replace_once(text, marker, '''            mp6_event_post("title.start", 1, "accepted");
''' + marker)


def filesel_probe(text):
    text = '#include "mp6_events.h"\n#include "mp6_boot.h"\n' + text
    marker = 'void ObjectSetup(void)\n{'
    return replace_once(text, marker, marker + '''
    mp6_event_post("title.filesel", 1, "entered");
    mp6_max_ticks = (int)mp6_tick_count + 600;
''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--windowed', action='store_true')
    args = parser.parse_args()
    os.environ.setdefault('MP6_DISC_ROOT', 'build/disc-cache/orig/GP6E01')
    os.environ.setdefault('MP6_DECOMP_INC_DATA', 'build/disc-cache/split/include')
    sys.path.insert(0, str(ROOT / 'tools'))
    import build

    output = ROOT / 'build/title-qa'
    output.mkdir(exist_ok=True)
    configure_original = build.configure_windows
    collect_original = build.collect_units
    original_objects = None

    def configure(configuration):
        nonlocal original_objects
        configure_original(configuration)
        original_objects = Path(build.OBJ_DIR)
        build.BUILD_DIR = str(output)
        build.OBJ_DIR = str(output / 'obj')

    def collect(*args, **kwargs):
        units = []
        for source, flags, name, flavor in collect_original(*args, **kwargs):
            obj = original_objects / name
            probe = {'rel_boot.o': boot_probe, 'rel_filesel.o': filesel_probe}.get(name)
            if probe:
                contents = probe(Path(source).read_text(encoding='utf-8'))
                source = str(output / (name[:-2] + '_probe.c'))
                Path(source).write_text(contents, encoding='utf-8')
                obj = output / 'obj' / name
            units.append((source, flags, str(obj), flavor))
        return units

    build.configure_windows = configure
    build.collect_units = collect
    sys.argv = [str(ROOT / 'tools/build.py'), '--configuration', 'release', '-j6']
    if not args.windowed:
        sys.argv.append('--headless')
    result = build.main()
    if result == 0:
        exe = output / ('release/mp6native.exe' if args.windowed else
                        'release-headless/mp6native_headless.exe')
        (exe.parent / 'title-qa-ready.json').write_text(json.dumps({
            'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
            'builder_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        }), encoding='utf-8')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
