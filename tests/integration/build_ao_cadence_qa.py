"""Build a separate board QA image with deferred, natural-alpha frame capture.

Reuse the board fixtures through exact-count, in-memory source substitutions.
Neither the board QA executable nor normal Release is replaced.
"""
from pathlib import Path
import hashlib
import json
import sys

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'build/board-cadence-qa'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transformed_builder():
    from tests.integration.ao_deferred_capture_source import replace_once
    source = (ROOT / 'tests/integration/build_board_qa.py').read_text(encoding='utf-8')
    source = replace_once(source, "OUTPUT = ROOT / 'build/board-qa'",
                          "OUTPUT = ROOT / 'build/board-cadence-qa'")
    source = replace_once(source, "            contents = instrument_fi_pixels(contents)",
                          "            contents = instrument_fi_pixels(contents)\n"
                          "            contents = deferred_interp(contents)")
    source = replace_once(source, "contents = instrument_pixel_dump(Path(source).read_text(encoding='utf-8'))",
                          "contents = deferred_dump(Path(source).read_text(encoding='utf-8'))")
    source = replace_once(source, '    return units\n',
        "    units.append((str(ROOT/'tests/integration/ao_deferred_capture_backend.cpp'),\n"
        "        ['-I'+str(ROOT/'third_party/aurora/lib')],\n"
        "        str(OUTPUT/'obj/qa_deferred_capture.o'), 'aurora'))\n"
        '    return units\n')
    return source


def main():
    sys.path.insert(0, str(ROOT))
    from tests.integration.ao_deferred_capture_source import instrument_dump, instrument_interp
    protected = [ROOT/'build/release/mp6native.exe', ROOT/'build/board-qa/release/mp6native.exe']
    before = {str(path): digest(path) for path in protected if path.exists()}
    namespace = dict(__file__=str(ROOT/'tests/integration/build_board_qa.py'),
                     __name__='__main__', deferred_dump=instrument_dump,
                     deferred_interp=instrument_interp)
    code = transformed_builder()
    result = 1
    try:
        exec(compile(code, namespace['__file__'], 'exec'), namespace)
    except SystemExit as exc:
        result = int(exc.code or 0)
    finally:
        for name, expected in before.items():
            if digest(Path(name)) != expected:
                raise RuntimeError(f'protected executable changed: {name}')
    if result == 0:
        ready_path = OUTPUT/'build-ready.json'
        ready = json.loads(ready_path.read_text())
        ready['deferred_capture'] = {
            'scope': 'QA-only consecutive pre-overlay frames, natural alpha; copy overhead remains',
            'sources': {name: digest(ROOT/name) for name in (
                'tests/integration/build_ao_cadence_qa.py',
                'tests/integration/ao_deferred_capture_source.py',
                'tests/integration/ao_deferred_capture_backend.cpp')},
            'transformed_builder_sha256': hashlib.sha256(code.encode()).hexdigest(),
            'generated_sources': {name: digest(OUTPUT/name) for name in (
                'frame_interp_fi_matrix_probe.c', 'frame_dump_fi_pixel_probe.c')},
            'protected_executables': before,
        }
        ready_path.write_text(json.dumps(ready, indent=2)+'\n')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
