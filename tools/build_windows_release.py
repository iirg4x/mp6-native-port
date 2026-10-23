"""Build the optimized Windows Aurora backend from third_party/aurora.

Configures build/aurora-release straight from the vendored Aurora source
(no copy, no patch step), with third-party sources copied from an existing
fetch (tools/aurora_deps.py, no downloads), compiles RelWithDebInfo and
attests the actual release link inputs. Run tools/build.py --configuration
release next.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from setup.lib import common, step_aurora, step_toolchain
import aurora_deps
import build
import host_cpus


def main():
    host_cpus.apply_cpu_exclusion()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', type=int, default=min(4, host_cpus.allowed_cpu_count()))
    args = parser.parse_args()
    source_deps = Path(build.aurora_deps_cache())
    cmake = aurora_deps.find_cmake(str(source_deps.parent), build.AURORA_BUILD_RMLUI)
    build._VERIFIED_ZIG_TREE_IDENTITY = step_toolchain.verified_required_zig_tree()
    destination = ROOT / 'build' / 'aurora-release'
    # A tree configured from the former build/aurora-release-source copy
    # cannot be re-pointed at the vendored source; start its cache over.
    cache = destination / 'CMakeCache.txt'
    home = 'CMAKE_HOME_DIRECTORY:INTERNAL=' + Path(build.AURORA_SOURCE).as_posix()
    if cache.exists() and home not in cache.read_text(encoding='utf-8', errors='replace'):
        cache.unlink()
        shutil.rmtree(destination / 'CMakeFiles', ignore_errors=True)
    # Third-party sources only; never reuse another tree's objects or CMake
    # cache. Resource-compiler scratch files stay local to this tree.
    definitions = aurora_deps.copy_sources(str(source_deps), str(destination / '_deps'))
    original_toolchain = common.TOOLCHAIN_DIR
    original_aurora = common.AURORA_DIR
    try:
        common.TOOLCHAIN_DIR = str(ROOT / 'build' / 'release-toolchain')
        wrappers = step_aurora._write_wrapper_scripts(build.ZIG)
        common.AURORA_DIR = build.AURORA_SOURCE
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
