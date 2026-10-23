"""Build a private Android Release board benchmark without replacing native artifacts.

Only generated RNG, W01 initialization, and frame-counter objects differ. The
current verified native link is reused, and all output stays under build/.
This tool does not install, launch, or access an Android device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

try:
    from .android_ao_benchmark_source import transform_source, validate_window
except ImportError:  # Direct script invocation from the repository root.
    from android_ao_benchmark_source import transform_source, validate_window

ROOT = Path(__file__).resolve().parents[2]
SOURCE_OBJECTS = {
    'frand': 'game_frand.o',
    'world01': 'rel_world01.o',
    'bridge': 'plat_aurora_bridge_aurora.o',
}


class LinkCaptured(Exception):
    pass


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def single_value(command, flag):
    if command.count(flag) != 1 or command.index(flag) + 1 >= len(command):
        raise RuntimeError('expected one command argument: ' + flag)
    return command[command.index(flag) + 1]


def redirected_compile(command, source, obj, generated, target):
    """Keep the recorded Release flags while moving every compiler output."""
    result = list(command)
    if (result.count('-O2') != 1 or any(re.fullmatch(r'-O(?:0|1|3|s|z|g|fast)', arg) for arg in result)
            or '-fno-strict-aliasing' not in result):
        raise RuntimeError('expected the verified Release compile flags')
    if Path(single_value(result, '-c')).resolve() != source or Path(single_value(result, '-o')).resolve() != obj:
        raise RuntimeError('recorded compile source/object identity mismatch')
    result[result.index('-c') + 1] = str(generated)
    result[result.index('-o') + 1] = str(target)
    if '-MF' in result:
        single_value(result, '-MF')
        result[result.index('-MF') + 1] = str(target.with_suffix('.d'))
    if '-MT' in result:
        single_value(result, '-MT')
        result[result.index('-MT') + 1] = str(target)
    # Generated files retain the original source's quoted-include lookup.
    result += ['-iquote', str(source.parent)]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='current')
    parser.add_argument('--warmup-ticks', type=int, default=600)
    parser.add_argument('--sample-ticks', type=int, default=1200)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple local artifact name required')
    try:
        validate_window(args.warmup_ticks, args.sample_ticks)
    except ValueError as error:
        parser.error(str(error))
    output = ROOT / 'build' / ('ao-android-benchmark-' + args.name)
    if output.exists():
        parser.error('refusing to overwrite an existing benchmark directory: ' + str(output))

    sys.path.insert(0, str(ROOT / 'build'))
    from android_local import build, configure, step_android
    configure()
    step_android.verify_android_native_build_manifest(ROOT, 'release')
    native = (ROOT / 'build/android/aurora/libmp6game.so').resolve()
    native_manifest_path = native.parent / 'native-build-manifest.json'
    native_manifest = json.loads(native_manifest_path.read_text())
    object_root = ROOT / 'build/android/obj'
    staged_root = ROOT / 'packaging/android/app/src/main/jniLibs'
    # Hash all production objects/records and staged native libraries, not only
    # the three overridden objects. A failed build must pass the same check.
    protected = set(path.resolve() for path in object_root.rglob('*') if path.is_file())
    protected.update(path.resolve() for path in staged_root.rglob('*.so') if path.is_file())
    protected.update(path.resolve() for path in native.parent.glob('*.so'))
    protected.add(native_manifest_path)
    inputs = {}
    for kind, filename in SOURCE_OBJECTS.items():
        obj = (object_root / filename).resolve()
        record_path = Path(str(obj) + '.cmd.json')
        command = json.loads(record_path.read_text())['command']
        source = Path(single_value(command, '-c')).resolve(strict=True)
        if not source.is_relative_to(ROOT):
            raise RuntimeError('benchmark source escaped the repository: ' + str(source))
        protected.add(source)
        source_bytes = source.read_bytes()
        text = source_bytes.decode('utf-8').replace('\r\n', '\n')
        generated_text = transform_source(kind, text, args.warmup_ticks, args.sample_ticks)
        inputs[kind] = dict(source=source, obj=obj, record_path=record_path,
                            command=command, source_sha256=hashlib.sha256(source_bytes).hexdigest(),
                            generated_text=generated_text)
    before = {str(path): digest(path) for path in sorted(protected)}
    original_run, original_argv = subprocess.run, sys.argv
    original_link_runner = build.run_link_command
    captured = []

    def capture_link(command, response_path, driver_args=1):
        # Capture before run_link_command writes the production response file.
        # Modern builds pass only @response-file to subprocess.run, so the old
        # subprocess-only probe hook would accidentally execute the real link.
        if ('-shared' not in command or driver_args != 1
                or Path(single_value(command, '-o')).resolve() != native):
            raise RuntimeError('link-only validation attempted an unexpected link')
        captured.extend(command)
        raise LinkCaptured()

    def capture(command, *positional, **kwargs):
        if isinstance(command, (list, tuple)):
            if '-c' in command:
                raise RuntimeError('link-only validation attempted to compile a production object')
            if '-shared' in command and '-o' in command:
                if Path(single_value(command, '-o')).resolve() != native:
                    raise RuntimeError('link-only validation attempted an unexpected native output')
                captured.extend(command)
                raise LinkCaptured()
        return original_run(command, *positional, **kwargs)

    try:
        subprocess.run = capture
        build.run_link_command = capture_link
        sys.argv = ['tools/build.py', '--target', 'aarch64-android', '--windowed',
                    '--configuration', 'release', '--link-only']
        try:
            build.main()
        except LinkCaptured:
            pass
        finally:
            subprocess.run = original_run
            build.run_link_command = original_link_runner
            sys.argv = original_argv
        if not captured:
            raise RuntimeError('production validation failed before the native link')
        link_hash = hashlib.sha256(json.dumps(captured, separators=(',', ':')).encode()).hexdigest()
        if link_hash != native_manifest['link_command_sha256']:
            raise RuntimeError('captured link does not match the verified Release native manifest')

        output.mkdir()
        (output / 'source').mkdir()
        (output / 'obj').mkdir()
        (output / 'lib').mkdir()
        compiled, source_manifest, replacements = [], {}, {}
        for kind, item in inputs.items():
            source, obj = item['source'], item['obj']
            generated = output / 'source' / (kind + source.suffix)
            target = output / 'obj' / obj.name
            generated.write_text(item['generated_text'], encoding='utf-8', newline='\n')
            command = redirected_compile(item['command'], source, obj, generated, target)
            original_run(command, cwd=ROOT, check=True)
            compiled.append(command)
            replacements[obj] = target
            source_manifest[kind] = dict(source=str(source), source_sha256=item['source_sha256'],
                command_record=str(item['record_path']), command_record_sha256=digest(item['record_path']),
                generated_source=str(generated), generated_sha256=digest(generated),
                object=str(target), object_sha256=digest(target))

        link = list(captured)
        for obj, target in replacements.items():
            indexes = [index for index, value in enumerate(link)
                       if Path(value).name == obj.name and Path(value).resolve() == obj]
            if len(indexes) != 1:
                raise RuntimeError('expected exactly one production object in the link: ' + str(obj))
            link[indexes[0]] = str(target)
        benchmark_native = output / 'lib/libmp6game.so'
        link[link.index('-o') + 1] = str(benchmark_native)
        linked = original_link_runner(link, str(output / 'link.rsp'))
        if linked.returncode:
            raise RuntimeError('private benchmark link failed:\n' + (linked.stderr or linked.stdout))
        stripped = output / 'stripped/libmp6game.so'
        stripped.parent.mkdir()
        llvm_strip = Path(compiled[0][0]).parent / 'llvm-strip.exe'
        original_run([str(llvm_strip), '--strip-unneeded', '-o', str(stripped), str(benchmark_native)],
                     cwd=ROOT, check=True)
        provenance = dict(release_asset=False, benchmark='fixed-seed-board',
            gpu_timing_version=2,
            diagnostic='fixed-seed bounded native-board measurement',
            warmup_board_age=args.warmup_ticks, sample_count=args.sample_ticks,
            first_board_age=args.warmup_ticks, last_board_age=args.warmup_ticks + args.sample_ticks - 1,
            initial_seed='0x12345678', board_seed='0xA341316C', source_manifest=source_manifest,
            source_hashes={kind: item['source_sha256'] for kind, item in inputs.items()},
            original_native_sha256=before[str(native)],
            unchanged_production=before, production_manifest_sha256=digest(native_manifest_path),
            original_link_sha256=link_hash, compile=compiled, link=link,
            native_sha256=digest(benchmark_native), stripped_sha256=digest(stripped),
            gpu_scope='On-demand coherent final GPU snapshot: 120 completed frames, separate from CPU ticks; AO distribution covers named timestamped passes, not integrated EFB composite or copy/barrier residuals',
            runtime_requirements=['MP6_TICK_RATE_LOG=1', 'MP6_GPU_TIMINGS=1',
                                  'same isolated data/input/settings, thermal admission and bounded watchdog'])
        # Do not publish successful provenance before the invariant check.
        changed = [path for path, expected in before.items() if digest(path) != expected]
        if changed:
            raise RuntimeError('production files changed: ' + ', '.join(changed))
        (output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        print('Isolated Android Release benchmark (not installed):', stripped)
    finally:
        subprocess.run = original_run
        build.run_link_command = original_link_runner
        sys.argv = original_argv
        changed = [path for path, expected in before.items() if not Path(path).is_file() or digest(path) != expected]
        if changed:
            raise RuntimeError('production preservation check failed: ' + ', '.join(changed))


if __name__ == '__main__':
    main()
