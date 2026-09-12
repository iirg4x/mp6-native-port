"""Build an optimized Windows backend without changing dependency checkouts.

Requires the verified, patched Debug Aurora setup. Snapshot its source and
fetched dependencies into build/, then compile RelWithDebInfo and attest the
actual release link inputs. Run tools/build.py --configuration release next.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from setup.lib import common, step_aurora, step_toolchain
import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', type=int, default=4)
    args = parser.parse_args()
    # Match the proven native CMake, not an MSYS CMake earlier on PATH (which
    # interprets C:/ as a relative POSIX path and cannot drive these wrappers).
    cache = Path(build.AURORA_BUILD_RMLUI) / 'CMakeCache.txt'
    cmake = next((line.split('=', 1)[1] for line in cache.read_text().splitlines()
                  if line.startswith('CMAKE_COMMAND:INTERNAL=')), shutil.which('cmake'))
    if not cmake:
        parser.error('CMake is required')
    build._VERIFIED_ZIG_TREE_IDENTITY = step_toolchain.verified_required_zig_tree()
    problems = step_aurora.verify_link_inputs('windows', build_module=build)
    problems += step_aurora._verify_patch_sources()
    if problems:
        parser.error('Debug source/artifact prerequisites failed: ' + '; '.join(problems))
    upstream = Path(build.AURORA_ROOT)
    cached_deps = Path(build.AURORA_DEPS_RMLUI)
    source = ROOT / 'build' / 'aurora-release-source'
    destination = ROOT / 'build' / 'aurora-release'
    # Copy sources only. Never reuse Debug objects or a CMake cache with paths
    # into the dependency checkout. Resource-compiler scratch files stay local.
    ignore = shutil.ignore_patterns('.git', 'build', 'build-*', '.cache', '.idea')
    shutil.copytree(upstream, source, ignore=ignore, dirs_exist_ok=True)
    for patch in sorted((ROOT / 'compat/aurora/windows').glob('*.patch')):
        if not step_aurora._apply_patch(build.apply_patches, str(patch), [str(source)]):
            parser.error(f'Release patch has no target: {patch}')
    definitions = []
    for dep in sorted(cached_deps.glob('*-src')):
        local = destination / '_deps' / dep.name
        shutil.copytree(dep, local, ignore=shutil.ignore_patterns('.git'), dirs_exist_ok=True)
        definitions.append(f'-DFETCHCONTENT_SOURCE_DIR_{dep.name[:-4].upper()}={local.as_posix()}')
    original_toolchain = common.TOOLCHAIN_DIR
    original_aurora = common.AURORA_DIR
    try:
        common.TOOLCHAIN_DIR = str(ROOT / 'build' / 'release-toolchain')
        wrappers = step_aurora._write_wrapper_scripts(build.ZIG)
        common.AURORA_DIR = str(source)
        command = step_aurora._configure_args(cmake, str(destination), wrappers, True)
    finally:
        common.TOOLCHAIN_DIR = original_toolchain
        common.AURORA_DIR = original_aurora
    command = [arg.replace('-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_BUILD_TYPE=RelWithDebInfo')
               for arg in command]
    command += definitions + [
        '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
        '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g -gcodeview -DNDEBUG -fno-strict-aliasing -Wno-error=date-time',
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g -gcodeview -DNDEBUG -fno-strict-aliasing -Wno-error=date-time',
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([cmake, '--build', str(destination), '-j', str(args.j), '--target',
                    *step_aurora.BUILD_TARGETS], cwd=ROOT, check=True)
    build.configure_windows('release')
    problems = step_aurora._verify_carveout(build)
    if problems:
        parser.error('; '.join(problems))
    step_aurora.write_stamp(step_aurora.read_aurora_pin(), ['aurora-release'],
                           build_module=build, path=build.AURORA_ARTIFACT_STAMP)
    print('Verified optimized backend:', destination)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
