"""Build Aurora for Android (arm64-v8a) from third_party/aurora.

Configures build/android-aurora (the tree tools/build.py --target
aarch64-android --windowed links) with the NDK toolchain, reusing already
fetched third-party sources (this checkout's, else the primary checkout's;
tools/aurora_deps.py patches the copy; no downloads), builds the Aurora
archives and records them in the Aurora artifact stamp. Needs the r27d NDK
(port/android-sdk or ANDROID_NDK_ROOT).

    python tools/build_android_aurora.py [-j N]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import aurora_deps  # noqa: E402
import build  # noqa: E402
import host_cpus  # noqa: E402
from setup.lib import step_aurora  # noqa: E402

TARGETS = ["aurora_core", "aurora_gx", "aurora_vi", "aurora_pad", "aurora_si", "aurora_card", "aurora_mtx"]


def find_ndk():
    ndk = build._find_ndk_root()
    if not ndk:
        raise SystemExit(f"Android NDK {build.ANDROID_NDK_VERSION} not found under "
                         "port/android-sdk/ndk: set ANDROID_NDK_ROOT")
    return ndk


def build_zlib_ng(cmake, ninja, ndk, tree, jobs):
    """zlib-ng (zlib-compatible API, NEON) for the game library, from the same
    fetched source the desktop Aurora trees build. The phone's system libz is
    plain zlib; archives inflate markedly faster with zlib-ng's NEON paths."""
    source = tree / "_deps" / "zlib-src"
    out = tree / "_deps" / "zlibng-build"
    command = [
        cmake, "-S", source.as_posix(), "-B", out.as_posix(), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={Path(ndk).as_posix()}/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28",
        "-DZLIB_COMPAT=ON", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
        "-DWITH_GTEST=OFF", "-DWITH_NEON=ON", "-DZLIB_ENABLE_TESTS=OFF",
    ]
    if ninja:
        command.append(f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}")
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([cmake, "--build", out.as_posix(), "-j", str(jobs), "--target", "zlib-ng"],
                   cwd=ROOT, check=True)


def main():
    host_cpus.apply_cpu_exclusion()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-j", type=int, default=host_cpus.allowed_cpu_count())
    args = parser.parse_args()
    ndk = find_ndk()
    tree = Path(build.AURORA_BUILD_ANDROID_RMLUI)
    source_deps = Path(build.aurora_deps_cache(android=True))
    cmake = aurora_deps.find_cmake(str(tree), str(source_deps.parent))
    ninja = shutil.which("ninja")
    cache = tree / "CMakeCache.txt"
    if cache.exists():
        text = cache.read_text(encoding="utf-8", errors="replace")
        if not ninja:
            for line in text.splitlines():
                if line.startswith("CMAKE_MAKE_PROGRAM:") and os.path.isfile(line.split("=", 1)[1]):
                    ninja = line.split("=", 1)[1]
        if "CMAKE_HOME_DIRECTORY:INTERNAL=" + Path(build.AURORA_SOURCE).as_posix() not in text:
            cache.unlink()
            shutil.rmtree(tree / "CMakeFiles", ignore_errors=True)
    definitions = aurora_deps.copy_sources(str(source_deps), str(tree / "_deps"))
    section = Path(step_aurora._host_section_header()).as_posix()
    command = [
        cmake, "-S", Path(build.AURORA_SOURCE).as_posix(), "-B", tree.as_posix(), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        f"-DCMAKE_TOOLCHAIN_FILE={Path(ndk).as_posix()}/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28",
        "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
        "-DAURORA_DAWN_PROVIDER=package",
        "-DAURORA_SDL3_PROVIDER=vendor", "-DAURORA_SDL3_LINKAGE=static",
        "-DBUILD_SHARED_LIBS=OFF", "-DAURORA_ENABLE_RMLUI=ON",
        # The savestate carve-out, as for every other Aurora tree.
        f"-DCMAKE_C_FLAGS=-include {section}",
        f"-DCMAKE_CXX_FLAGS=-include {section}",
        "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG -fno-strict-aliasing",
        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG -fno-strict-aliasing",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
    ] + definitions
    if ninja:
        command.append(f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}")
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([cmake, "--build", tree.as_posix(), "-j", str(args.j), "--target", *TARGETS],
                   cwd=ROOT, check=True)
    build_zlib_ng(cmake, ninja, ndk, tree, args.j)
    step_aurora.write_stamp(step_aurora.read_aurora_pin(), [tree.name], build_module=build)
    print("Built Android Aurora:", tree)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
