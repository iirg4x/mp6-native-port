"""Shared isolated native-overlay QA builder; production objects are reused."""
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def build_probe(name, instrumenters, jobs=2):
    """Transform only explicit (source-directory-name, filename) matches."""
    os.environ.setdefault('MP6_DISC_ROOT', str(ROOT / 'build/disc-cache/orig/GP6E01'))
    os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT / 'build/disc-cache/split/include'))
    sys.path.insert(0, str(ROOT / 'tools'))
    import build

    output = ROOT / 'build' / name
    output.mkdir(exist_ok=True)
    original_configure, original_collect = build.configure_windows, build.collect_units
    original_argv = sys.argv
    objects = None

    def configure(configuration):
        nonlocal objects
        original_configure(configuration)
        objects = Path(build.OBJ_DIR)
        build.BUILD_DIR, build.OBJ_DIR = str(output), str(output / 'obj')

    def collect(*args, **kwargs):
        units = []
        matched = set()
        for source, flags, name, flavor in original_collect(*args, **kwargs):
            path = Path(source)
            key = (path.parent.name, path.name)
            instrument = instrumenters.get(key)
            obj = objects / name
            if instrument is not None:
                if key in matched:
                    raise RuntimeError(f'QA source matched more than once: {key}')
                matched.add(key)
                changed = instrument(path.read_text())
                source = str(output / (name[:-2] + '_probe.c'))
                Path(source).write_text(changed)
                obj = output / 'obj' / name
            units.append((source, flags, str(obj), flavor))
        missing = set(instrumenters) - matched
        if missing:
            raise RuntimeError(f'QA sources not found (exact case required): {sorted(missing)}')
        return units

    build.configure_windows, build.collect_units = configure, collect
    if jobs not in (1, 2):
        raise ValueError('native QA permits at most two compiler jobs')
    sys.argv = ['tools/build.py', '--configuration', 'release', f'-j{jobs}']
    try:
        return build.main()
    finally:
        build.configure_windows, build.collect_units = original_configure, original_collect
        sys.argv = original_argv
