"""Build the packet writer into private, manifest-bound Release renderers.

Recompile every __gx.h consumer, keeping the current game objects, GPU backend,
optimization flags and unrelated renderer objects. No staging, APK installation,
decomp access, production writes or generated-script execution is performed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/fifo-packets-20260912'
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
from setup.lib import step_aurora, step_android
import apply_patches


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def run(command, **kwargs):
    return subprocess.run([str(a) for a in command], capture_output=True, text=True,
                          check=True, timeout=180, **kwargs)


def inputs_for_link(args):
    return {Path(a) for a in args if not a.startswith('-') and Path(a).is_file()}


def compile_flags(ninja, source):
    needle = re.escape(source.as_posix().replace(':', '$:'))
    blocks = [m.group(1) for m in re.finditer(
        r'^build CMakeFiles/aurora_gx.dir/[^\n]* ' + needle +
        r' [^\n]*\n((?:  [^\n]*\n)+)', ninja, re.MULTILINE)]
    if len(blocks) != 1:
        raise RuntimeError('ambiguous or missing compilation unit: ' + str(source))
    flags = []
    for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
        rows = re.findall(r'^  ' + key + r' = (.*)$', blocks[0], re.MULTILINE)
        if len(rows) != 1:
            raise RuntimeError('missing compiler flags: ' + key)
        flags += shlex.split(rows[0])
    if [f for f in flags if re.fullmatch(r'-O\w+', f)] != ['-O2'] or '-DNDEBUG' not in flags:
        raise RuntimeError('expected the authenticated optimized renderer profile')
    if any(f in flags for f in ('-ffast-math', '-Ofast')):
        raise RuntimeError('quality-changing compiler option')
    return flags


def caller_audit(source):
    consumers, calls = [], []
    token = re.compile(r'\bGX_WRITE_(?:RAS|CP|XF)_REG\b')
    expression = re.compile(r'\b(GX_WRITE_(?:RAS|CP|XF)_REG)\(([^;\n]+)\);')
    for path in sorted((source / 'lib').rglob('*.cpp')):
        text = path.read_text()
        if re.search(r'#include "__gx.h"', text):
            consumers.append(path.relative_to(source).as_posix())
        matches = list(expression.finditer(text))
        if len(matches) != len(token.findall(text)):
            raise RuntimeError('unreviewed multiline or non-call macro use: ' + str(path))
        if matches and path.relative_to(source).as_posix() not in consumers:
            raise RuntimeError('unreviewed indirect macro consumer: ' + str(path))
        for match in matches:
            args = match[2]
            if not re.fullmatch(r'[A-Za-z0-9_>*+|\[\], \-]+', args) or '++' in args or '--' in args:
                raise RuntimeError('nontrivial macro argument needs review: ' + args)
            calls.append(dict(file=path.relative_to(source).as_posix(),
                line=text[:match.start()].count('\n')+1, macro=match[1], arguments=args))
    if not consumers or not calls:
        raise RuntimeError('no register producers found')
    return consumers, calls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=['windows', 'android'])
    args = parser.parse_args()
    windows = args.platform == 'windows'
    source = ROOT / 'build' / ('aurora-release-source' if windows else 'android-aurora-source')
    tree = ROOT / 'build' / ('aurora-release' if windows else 'android-aurora')
    dest = OUT / args.platform
    copy = dest / 'source'
    proof = json.loads((OUT / 'native-tests.json').read_text())
    patch = OUT / 'renderer.patch'
    if sha(patch) != proof['patch_sha256']:
        raise RuntimeError('patch differs from the native-tested candidate')
    for name, digest in proof['source_sha256'].items():
        if sha(source / name) != digest:
            raise RuntimeError('production source changed since native tests: ' + name)
    inputs = {path for top in ('lib', 'include') for path in (source / top).rglob('*') if path.is_file()}
    units, calls = caller_audit(source)
    sections = step_aurora.split_patch_sections(patch.read_text())
    if {section['new'] for section in sections} != {'lib/gx/fifo.hpp', 'lib/dolphin/gx/__gx.h'}:
        raise RuntimeError('unexpected experiment targets')
    modified = {}
    for section in sections:
        if section['old'] != section['new']:
            raise RuntimeError('this experiment must not add, remove or rename source files')
        modified[section['new']] = apply_patches.apply_unified_diff(
            (source / section['old']).read_text(), section['body'], label=section['new'])
    source_hashes = {str(p): sha(p) for p in inputs}
    owner = dict(platform=args.platform, patch_sha256=sha(patch), source_sha256=source_hashes, units=units)
    dest.mkdir(exist_ok=True)
    owner_path = dest / 'owner.json'
    if owner_path.exists():
        if json.loads(owner_path.read_text()) != owner:
            raise RuntimeError('private build belongs to different inputs; do not overwrite')
    else:
        if copy.exists():
            raise RuntimeError('unowned private source directory exists')
        write_json(owner_path, owner)
        for top in ('lib', 'include'):
            shutil.copytree(source / top, copy / top)
        for name, text in modified.items():
            (copy / name).write_text(text)
    for p in inputs:
        name = p.relative_to(source).as_posix()
        if name in modified:
            if (copy / name).read_text() != modified[name]:
                raise RuntimeError('private candidate edited after preparation: ' + name)
        elif sha(copy / name) != source_hashes[str(p)]:
            raise RuntimeError('private source drift: ' + name)
    write_json(dest / 'macro-callers.json', dict(consumers=units, calls=calls,
        scope='Every changed macro use under lib/*.cpp; all arguments are simple non-incrementing expressions.'))

    os.environ['MP6_DISC_ROOT'] = str(ROOT / 'build/disc-cache/orig/GP6E01')
    os.environ['MP6_DECOMP_INC_DATA'] = str(ROOT / 'build/disc-cache/split/include')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT / 'build/release-toolchain/zig-cache'))
    if windows:
        import build
        build.configure_windows('release')
        build._require_aurora_artifact_stamp('windows', build._resolve_aurora_link_items())
        prior = ROOT / 'build/uniform-direct-20260912/windows'
        link = shlex.split((prior / 'baseline/link.rsp').read_text())
        objects = [Path(a) for a in link if a.endswith('.o')]
        prior_hashes = json.loads((prior / 'provenance.json').read_text())['unchanged_inputs']
        if len(objects) != 176 or any(sha(p) != prior_hashes[str(p)] for p in objects):
            raise RuntimeError('fixed-seed game objects changed')
        driver = [build.ZIG, 'c++', '-target', 'x86_64-windows-gnu']
        archiver = [build.ZIG, 'ar']
        suffix = '.obj'
        output = dest / 'release/mp6native.exe'
    else:
        step_android.verify_android_native_build_manifest(ROOT, 'release')
        prior = ROOT / 'build/fifo-nop-20260912/android'
        previous = json.loads((prior / 'provenance.json').read_text())
        link = previous['link']
        native = ROOT / 'build/android/aurora/libmp6game.so'
        link[link.index('-o')+1] = str(native)
        # The production link-item resolver emits forward slashes, even on
        # Windows. Preserve its exact spelling for the manifest command hash.
        link = [(tree / 'libaurora_gx.a').as_posix() if Path(a) == prior / 'libaurora_gx.a' else a for a in link]
        manifest = json.loads((native.parent / 'native-build-manifest.json').read_text())
        encoded = json.dumps(link, separators=(',', ':')).encode()
        if hashlib.sha256(encoded).hexdigest() != manifest['link_command_sha256']:
            raise RuntimeError('recovered baseline link does not match the current native manifest')
        for path, digest in previous['unchanged_production'].items():
            if sha(path) != digest:
                raise RuntimeError('authenticated Android input changed: ' + path)
        ndk = ROOT / 'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin'
        driver = [ndk / 'clang++.exe', '--target=aarch64-none-linux-android28']
        archiver = [ndk / 'llvm-ar.exe']
        suffix = '.o'
        output = dest / 'libmp6game.so'

    inputs |= inputs_for_link(link)
    inputs |= {ROOT / 'build/release/mp6native.exe', ROOT / 'build/android/aurora/libmp6game.so',
               ROOT / 'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'}
    watched = {str(path): sha(path) for path in inputs}
    archive = dest / 'libaurora_gx.a'
    shutil.copy2(tree / archive.name, archive)
    members = run([*archiver, 't', archive]).stdout.splitlines()
    ninja = (tree / 'build.ninja').read_text()
    commands = []
    objects = []
    for name in units:
        flags = [arg.replace(source.as_posix(), copy.as_posix())
                 for arg in compile_flags(ninja, source / name)]
        obj = dest / (Path(name).name + suffix)
        if members.count(obj.name) != 1:
            raise RuntimeError('archive member is not unique: ' + obj.name)
        commands.append([*driver, *flags, '-c', str(copy / name), '-o', str(obj)])
        objects.append(obj)

    def compile_one(item):
        index, command = item
        try:
            result = run(command, cwd=tree, env=env)
        except subprocess.CalledProcessError as error:
            (dest / ('compile-' + str(index) + '.log')).write_text(error.stdout + error.stderr)
            raise
        (dest / ('compile-' + str(index) + '.log')).write_text(result.stdout + result.stderr)
        print(f'{args.platform}: compiled {index+1}/{len(commands)} {units[index]}', flush=True)

    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile_one, enumerate(commands)))
    run([*archiver, 'r', archive, *objects])
    replacements = sum(Path(a) == tree / archive.name for a in link)
    if replacements < 1:
        raise RuntimeError('baseline link does not use the expected renderer archive')
    link = [str(archive) if Path(a) == tree / archive.name else a for a in link]
    output.parent.mkdir(exist_ok=True)
    link[link.index('-o')+1] = str(output)
    response = dest / 'link.rsp'
    link_arguments = link if windows else link[1:]
    response.write_text('\n'.join('"' + arg.replace('\\', '\\\\').replace('"', '\\"') + '"' for arg in link_arguments) + '\n')
    link_driver = [build.ZIG, 'c++'] if windows else [link[0]]
    try:
        result = run([*link_driver, '@' + str(response)], cwd=ROOT, env=env)
    except subprocess.CalledProcessError as error:
        (dest / 'link.log').write_text(error.stdout + error.stderr)
        raise
    (dest / 'link.log').write_text(result.stdout + result.stderr)
    artifacts = {str(output): sha(output), str(archive): sha(archive)}
    if windows:
        build.copy_aurora_runtime_dlls(str(output.parent))
    else:
        stripped = dest / 'stripped/libmp6game.so'
        stripped.parent.mkdir(exist_ok=True)
        run([ndk / 'llvm-strip.exe', '--strip-unneeded', '-o', stripped, output])
        artifacts[str(stripped)] = sha(stripped)
    if any(sha(Path(p)) != digest for p, digest in watched.items()):
        raise RuntimeError('production input changed during the experiment')
    write_json(dest / 'provenance.json', dict(experimental_only=True, adopted=False,
        production_unchanged=watched, native_test_proof_sha256=sha(OUT / 'native-tests.json'),
        patch_sha256=sha(patch), macro_callers_sha256=sha(dest / 'macro-callers.json'),
        candidate_headers={name: sha(copy / name) for name in modified},
        compile=[[str(a) for a in cmd] for cmd in commands], link=link, artifacts=artifacts,
        android_game_fps_gain_proven=False))
    print(f'{args.platform}: private Release renderer linked; production unchanged.', flush=True)


if __name__ == '__main__':
    main()
