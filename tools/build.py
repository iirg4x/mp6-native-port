#!/usr/bin/env python3
"""
build.py -- the build driver for mp6native.exe.

There is no CMake step for the game itself: CMAKE_C_COMPILER as a "zig;cc"
list does not survive CMake's compiler-detection bootstrap (it mis-splits
the value). This plain Python/subprocess driver compiles every .c file to
a .o via `zig cc`, in parallel, incrementally (skips files whose .o is
newer than both the source and this script), then links everything into
build/mp6native.exe with a fixed low image base and ASLR disabled (see
platform/os/arena.c's header comment for why).

Usage:
    python tools/build.py            # build
    python tools/build.py --clean    # remove build/ first
    python tools/build.py --link-only
"""
import argparse
import concurrent.futures
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apply_patches  # noqa: E402 -- surgical patch queue, see its own docstring

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../mp6-native
PORT_ROOT = os.path.dirname(NATIVE_ROOT)                                    # .../port
DECOMP = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos", "marioparty6"))
ZIG = os.path.join(PORT_ROOT, "toolchain", "zig-x86_64-windows-0.16.0", "zig.exe")
BUILD_DIR = os.path.join(NATIVE_ROOT, "build")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
PATCHED_SRC_DIR = os.path.join(BUILD_DIR, "patched-src")

TARGET = "x86_64-windows-gnu"
IMAGE_BASE = "0x10000000"  # see platform/os/arena.c: keeps code/static-data pointers <4GB

# ---------------------------------------------------------------------------
# the `--target aarch64-android` row --
# the design's own target-row table, first row beyond Windows. Game
# TUs compile with NDK clang (r27d, port/android-sdk/ndk/<ver>; NOT
# zig-android -- the design's own cross-toolchain-ABI argument) into a
# SEPARATE build tree (build/android/) so the Windows build's objects,
# generated headers and incremental state are bit-for-bit untouched by an
# android build and vice versa (the MSL override headers genuinely differ
# per target -- absolute paths into each toolchain's real libc -- so they
# can never share a directory). Output: build/android/libmp6game.so (ALL
# game/REL TUs + platform/os + platform/host with host_android.c -- the
# headless TU set; -shared -fPIC, 16KB max-page-size per the design's own risk analysis) +
# build/android/mp6launcher (the probe-shaped loader exe, see
# platform/android/mp6launcher.c). Headless-only by design: aurora/SDL for
# Android is the android row.
ANDROID_API_LEVEL = 28  # matches the probe row used during Android bring-up
ANDROID_TRIPLE = f"aarch64-linux-android{ANDROID_API_LEVEL}"
ANDROID_BUILD_DIR = os.path.join(BUILD_DIR, "android")
ANDROID_OBJ_DIR = os.path.join(ANDROID_BUILD_DIR, "obj")
ANDROID_MSL_OVERRIDE = os.path.join(ANDROID_BUILD_DIR, "msl_override")
ANDROID_PATCHED_INCLUDE = os.path.join(ANDROID_BUILD_DIR, "patched_include")
# The on-device smoke layout (everything
# under /data/local/tmp/mp6. These bake the DEVICE-side fallback paths into
# the .so the same way MP6_DVD_FILES_ROOT below bakes this repo's absolute
# Windows paths into the exe; at runtime host_android.c's
# mp6_host_disc_root (driven by the launcher's MP6_HOST_BASE) normally
# resolves the same tree anyway, so these are the same belt-and-suspenders
# fallback the Windows build keeps.
ANDROID_DEVICE_BASE = "/data/local/tmp/mp6"
ANDROID_DVD_FILES_ROOT = ANDROID_DEVICE_BASE + "/GP6E01/files"
ANDROID_DVD_FST_PATH = ANDROID_DEVICE_BASE + "/GP6E01/sys/fst.bin"


def _find_ndk_root():
    """ANDROID_NDK_ROOT env override, else the newest ndk under
    port/android-sdk/ndk/ (the probe kit's own autodetection rule,
    port/android-probe/build.bat)."""
    env = os.environ.get("ANDROID_NDK_ROOT")
    if env and os.path.isdir(env):
        return env
    ndk_base = os.path.join(PORT_ROOT, "android-sdk", "ndk")
    if os.path.isdir(ndk_base):
        vers = sorted(d for d in os.listdir(ndk_base)
                      if os.path.isdir(os.path.join(ndk_base, d)))
        if vers:
            return os.path.join(ndk_base, vers[-1])
    return None


def _ndk_clang(ndk_root):
    return os.path.join(ndk_root, "toolchains", "llvm", "prebuilt",
                        "windows-x86_64", "bin", "clang.exe")


def _find_ndk_libc_dir(name):
    """The android analogue of _find_zig_libc_dir (below): where the REAL
    system header lives for the MSL-override shadow files. bionic's libc
    headers are in the NDK sysroot; float.h/stdarg.h/stddef.h are compiler
    headers from clang's own resource dir (lib/clang/<ver>/include)."""
    ndk = _find_ndk_root()
    if not ndk:
        return None
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64")
    candidates = [os.path.join(prebuilt, "sysroot", "usr", "include")]
    clang_base = os.path.join(prebuilt, "lib", "clang")
    if os.path.isdir(clang_base):
        for ver in sorted(os.listdir(clang_base)):
            candidates.append(os.path.join(clang_base, ver, "include"))
    for d in candidates:
        if os.path.exists(os.path.join(d, name)):
            return os.path.join(d, name)
    return None

DECOMP_INCLUDE = os.path.join(DECOMP, "include")
SHIM_INCLUDE = os.path.join(NATIVE_ROOT, "shim", "include")  # moved up here: AURORA_FLAGS (below) needs it too
# platform/host/host.h -- the OS seam interface. On BOTH flag sets (like
# SHIM_INCLUDE) since seam consumers span both flavors: common-flavor TUs
# (arena/process_native/card/dvd/shims_manual/msm_bridge) AND aurora-flavor
# ones (aurora_bridge.c's tick throttle, main_native.c's crash-install call).
HOST_INCLUDE = os.path.join(NATIVE_ROOT, "platform", "host")

# The real, extracted GameCube disc tree -- platform/dvd/dvd_files.c reads
# orig/GP6E01/sys/fst.bin (the real File String Table) and serves real
# bytes straight out of orig/GP6E01/files/ at runtime (NOT baked into the
# binary) -- so the RUNNING exe needs these two absolute, forward-slash
# paths (avoids C-string backslash-escaping entirely), baked in as -D's the
# same way DECOMP_INCLUDE etc. are already absolute build-time paths.
MP6_DVD_FILES_ROOT = os.path.join(DECOMP, "orig", "GP6E01", "files").replace("\\", "/")
MP6_DVD_FST_PATH = os.path.join(DECOMP, "orig", "GP6E01", "sys", "fst.bin").replace("\\", "/")

# The launcher's bottom-right version line (partyboard shows its git
# describe there). Resolved once per build-driver run; "dev" when git is
# unavailable.
_MP6_PORT_VERSION = None
def mp6_port_version():
    global _MP6_PORT_VERSION
    if _MP6_PORT_VERSION is None:
        try:
            _MP6_PORT_VERSION = subprocess.run(
                ["git", "-C", NATIVE_ROOT, "rev-parse", "--short", "HEAD"],
                capture_output=True, text=True, timeout=10).stdout.strip() or "dev"
        except Exception:
            _MP6_PORT_VERSION = "dev"
    return _MP6_PORT_VERSION

# game/decode.c's HuDecodeZlib needs REAL zlib -- see platform/null/
# shims_manual.c's own comment for why a fake inflate() (never actually
# decompressing anything) is not an option once real disc data is in play.
# Reuses the exact zlib-ng build
# Aurora's own CMake already fetches+builds (external_refs/repos/aurora/
# build/_deps/zlib-build/) for BOTH build modes now, not just aurora's --
# zlib-ng is plain C with no Aurora/C++ dependency of its own, so linking
# its already-built DLL into --headless too is far simpler than hand-
# building zlib-ng's own SIMD-dispatch machinery from source a second time
# just for this one build mode. (AURORA_DEPS is defined below this point in
# the file currently, so these two are resolved lazily inside main()/the
# link step instead of as flat module-level constants like the DVD paths
# above -- see MP6_ZLIB_LIB_ITEM/MP6_ZLIB_DLL usage further down.)

# ---------------------------------------------------------------------------
# Aurora (external_refs/repos/aurora), built standalone with the project's
# own zig toolchain into aurora/build/ (a CMake+Ninja tree; NOT reused here
# as a build system -- this driver only reads its already-built artifacts:
# the `simple` example's own `build.ninja` link line was inspected directly
# to get the exact archive list/order and INCLUDES/DEFINES below, since
# that's the one already proven to open a real window and render).
AURORA_ROOT = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos", "aurora"))
AURORA_INCLUDE = os.path.join(AURORA_ROOT, "include")
AURORA_BUILD = os.path.join(AURORA_ROOT, "build")
AURORA_DEPS = os.path.join(AURORA_BUILD, "_deps")

# ---------------------------------------------------------------------------
# The RmlUi-enabled aurora build tree. The ripped partyboard launcher UI
# (platform/gx/ui/) runs on aurora's own RmlUi module (lib/rmlui/*), which
# AURORA_ENABLE_RMLUI compiles INTO aurora_core/aurora_gx as a PUBLIC define
# -- i.e. enabling it changes those archives' content. To keep the
# canonical aurora/build tree byte-untouched (and canonical mp6-native
# builds unaffected), the module is enabled in this SEPARATE tree,
# configured with the same recipe as aurora/build plus
# -DAURORA_ENABLE_RMLUI=ON (same source checkout and pin -- the module is
# simply not enabled in the plain tree):
#
#   cmake -S external_refs/repos/aurora -B .../aurora/build-rmlui -G Ninja
#     -DCMAKE_BUILD_TYPE=Debug
#     -DCMAKE_C_COMPILER/<etc>=port/toolchain/zig-cc-wrappers/...
#     -DCMAKE_EXE_LINKER_FLAGS=-L<wrappers>/stub-libs
#     -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=package
#     -DAURORA_ENABLE_RMLUI=ON
#     -DCMAKE_CXX_FLAGS="-include <NATIVE_ROOT>/shim/include/mp6_host_section.h"
#     -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
#   (then apply platform/gx/aurora-patches/0001-abseil-*.patch to
#    build-rmlui/_deps/abseil-cpp-src)
#   cmake --build .../build-rmlui --target aurora_core aurora_gx aurora_main
#     aurora_vi aurora_pad aurora_si aurora_card aurora_mtx
#
# THE LAST TWO FLAGS ARE THE SAVESTATE CARVE-OUT and
# are NOT optional if savestates are to work in the windowed build. Aurora,
# RmlUi, abseil et al are consumed as STATIC ARCHIVES from this out-of-band
# tree, so their C++ globals -- the SDL_Window*, the WebGPU device/queue/
# surface, two sqlite3 handles, a live render-worker std::thread, the gamepad
# map, the RmlUi context -- are merged into mp6native.exe's ordinary writable
# sections and would otherwise be captured and restored like game state,
# installing the CAPTURING process's driver handles into a running process.
# HOST_STATE_SECTION_SOURCES cannot reach them (it only covers TUs build.py
# itself compiles); the tree's own CXX flags are the only seam that can.
#
# CMAKE_CXX_FLAGS only, deliberately NOT CMAKE_C_FLAGS: the pragma turns C
# tentative definitions into strong per-TU ones, which breaks the link with
# duplicate symbols in the C dependencies (the same trap that bit the port's
# own TUs -- see host_section_flags()). C++ has no tentative definitions, and
# every aurora global that matters is C++.
#
# DISABLE_PRECOMPILE_HEADERS is required because ~190 TUs here (all of RmlUi)
# compile with a PCH, and the pragma's effect does not survive into a
# PCH-using TU -- verified empirically: with PCH on, aurora's own non-PCH TUs
# were carved correctly and a windowed rewind ran its whole post-restore span,
# but RmlUi's own statics were still restored and the process died at exit in
# Rml::PluginRegistry::NotifyShutdown walking a std::vector full of the
# capturing process's heap pointers. With PCH off, that crash is gone.
#
# The WINDOWED link + its runtime DLLs consume THIS tree; --headless keeps
# consuming aurora/build (its zlib import lib/DLL) byte-for-byte as before.
AURORA_BUILD_RMLUI = os.path.join(AURORA_ROOT, "build-rmlui")
AURORA_DEPS_RMLUI = os.path.join(AURORA_BUILD_RMLUI, "_deps")

# See the zlib comment further up -- resolved here (not as a flat constant
# next to MP6_DVD_FILES_ROOT) simply because AURORA_DEPS has to exist
# first. Used by BOTH build modes' link step / DLL-copy step below.
MP6_ZLIB_LIB_ITEM = os.path.join(AURORA_DEPS, "zlib-build", "libzlib.dll.a")
MP6_ZLIB_DLL = os.path.join(AURORA_DEPS, "zlib-build", "libzlib1.dll")
AURORA_STUB_LIBS = os.path.join(PORT_ROOT, "toolchain", "zig-cc-wrappers", "stub-libs")  # comsuppw stub

# Mirrors examples/CMakeFiles/simple.dir/simple.c.obj's own `INCLUDES =` line
# in aurora/build/build.ninja. fmt/sdl3/xxhash/imgui/dawn are all transitive
# PUBLIC/INTERFACE include dirs of the aurora_* targets, propagated to any
# consumer by CMake regardless of whether that consumer's OWN code touches
# them directly -- kept here for the same reason (our 2 consuming files
# currently only need aurora/*.h + dolphin/*.h, but this stays a faithful
# mirror of the proven-working example rather than a trimmed guess).
AURORA_COMPILE_INCLUDES = [
    ("-I", AURORA_INCLUDE),
    ("-I", os.path.join(AURORA_DEPS, "fmt-src", "include")),
    ("-I", os.path.join(AURORA_DEPS, "sdl3_prebuilt-src", "include")),
    ("-I", os.path.join(AURORA_DEPS, "xxhash-src", "cmake_unofficial", "..")),
    ("-I", os.path.join(AURORA_DEPS, "imgui-src")),
    ("-isystem", os.path.join(AURORA_DEPS, "dawn_prebuilt-src", "include")),
]
# Mirrors the same compile rule's `DEFINES =` line exactly -- the
# already-proven set, not a trimmed-down guess.
AURORA_DEFINES = [
    "-DAURORA", "-DAURORA_ENABLE_GX",
    "-DIMGUI_ENABLE_FREETYPE", '-DIMGUI_USER_CONFIG="aurora/imgui_config.h"',
    "-DTARGET_PC", "-DWEBGPU_DAWN",
]
# platform/main_native.c and platform/gx/aurora_bridge.c ONLY -- see
# aurora_bridge.c's own header comment for why these two files deliberately
# do NOT also get COMMON_FLAGS's decomp -I's / -include dolphin_compat.h
# (both header trees have e.g. `dolphin/gx/GXGeometry.h` at the identical
# relative path; mixing both -I roots in one TU would make -I ORDER, not
# authorial intent, decide which tree's copy `#include <dolphin/...>`
# resolves to, for every such header in that one file).
AURORA_FLAGS = [
    "-target", TARGET,
    # See COMMON_FLAGS's own comment on -g/-gcodeview -- kept consistent
    # between both flag sets so an Aurora-only TU (platform/gx/aurora_bridge.c,
    # the non-headless platform/main_native.c, etc.) resolves symbols just as
    # well as a COMMON_FLAGS one.
    "-g", "-gcodeview",
    "-std=gnu11",
    # mp6_boot.h/mp6_shim_log.h only (both plain-C, no decomp dependency at
    # all -- verified) -- safe to add unconditionally since nothing under
    # shim/include/ is named like anything in AURORA_INCLUDE's own tree.
    "-I", SHIM_INCLUDE,
    # host.h (plain C, stdint/stddef only -- deliberately depends on
    # NEITHER dolphin.h flavor, see its own header comment) for
    # aurora_bridge.c's throttle + main_native.c's crash-install call.
    "-I", HOST_INCLUDE,
] + [x for pair in AURORA_COMPILE_INCLUDES for x in pair] + AURORA_DEFINES

# Exact archive list + order from aurora/build/build.ninja's own link line
# for `examples/simple.exe` (`grep -n "simple.exe:" build.ninja`, the
# `LINK_LIBRARIES =` value) -- static-archive link order matters here
# (abseil's own internal deps in particular), so this mirrors that proven
# line verbatim rather than a hand-picked subset. Entries are either a
# path relative to AURORA_BUILD, or a raw `-lxxx` linker flag passed
# through as-is (an already-installed Windows import library, resolved by
# zig's bundled mingw-w64 sysroot).
AURORA_LINK_ITEMS = [
    # aurora::pad (+ its own aurora::si dependency) is NOT part of
    # examples/simple.exe's link line at all (that example never calls any
    # PAD function) -- added here since PAD is in scope for this port.
    # Built directly into the SAME already-configured aurora/build tree via
    # `cmake --build aurora/build --target aurora_pad aurora_si` (PowerShell,
    # not Git Bash -- zig's cache-directory resolution fails under Git
    # Bash's environment). Listed first, before aurora_core, since both
    # depend on it (PUBLIC target_link_libraries in cmake/aurora_{pad,si}.cmake).
    "libaurora_pad.a", "libaurora_si.a",
    # aurora::card (memory-card slot A): full SDK-compatible CARD backend
    # (GCI-folder default) -- built via `cmake --build build --target
    # aurora_card` (PowerShell) after aurora patch 0008 (mingw shlobj.h).
    # Depends PUBLIC on aurora::core, so listed before it like pad/si.
    # Configured at runtime by platform/os/card_native.c's mp6_CARDInit
    # interposer.
    "libaurora_card.a",
    # aurora::mtx: a standalone leaf target (cmake/aurora_mtx.cmake has no
    # target_link_libraries at all), so link-order relative to the others
    # doesn't matter; built the same way, `cmake --build aurora/build
    # --target aurora_mtx` (PowerShell). Without it, the game's entire
    # matrix/vector math silently no-ops -- see platform/gx/aurora_bridge.c's
    # own header comment for the full story.
    "libaurora_mtx.a",
    "libaurora_core.a", "libaurora_gx.a", "libaurora_main.a", "libaurora_vi.a",
    "_deps/png-build/libpng16d.dll.a",
    "libaurora_core.a",  # CMake emits aurora_core twice; kept to match exactly
    # The RmlUi module archives -- aurora_core's lib/rmlui/* objects
    # reference the SDL platform backend, which references RmlUi core,
    # which references freetype (already below). Order matters for static
    # resolution: backends before core, both after the aurora archives,
    # both before libfreetyped.a. Resolved against AURORA_BUILD_RMLUI like
    # every other entry here (see _resolve_aurora_link_items).
    "extern/librmlui_backends.a",
    "librmlui.a",
    "_deps/fmt-build/libfmtd.a",
    "_deps/xxhash-build/libxxhash.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cord.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_info.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cord_internal.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_functions.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_handle.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_cord_state.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc32c.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_internal.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_cpu_detect.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_str_format_internal.a",
    "_deps/abseil-cpp-build/absl/container/libabsl_raw_hash_set.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_hash.a",
    "_deps/abseil-cpp-build/absl/types/libabsl_bad_optional_access.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_city.a",
    "_deps/abseil-cpp-build/absl/types/libabsl_bad_variant_access.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_low_level_hash.a",
    "_deps/abseil-cpp-build/absl/container/libabsl_hashtablez_sampler.a",
    "_deps/abseil-cpp-build/absl/profiling/libabsl_exponential_biased.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_synchronization.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_stacktrace.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_graphcycles_internal.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_kernel_timeout_internal.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_time.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_civil_time.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_time_zone.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_symbolize.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_strings.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_strings_internal.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_string_view.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_throw_delegate.a",
    "_deps/abseil-cpp-build/absl/numeric/libabsl_int128.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_debugging_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_malloc_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_base.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_raw_logging_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_log_severity.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_spinlock_wait.a",
    "-ladvapi32",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_internal.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_rust.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_decode_rust_punycode.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_utf8_for_code_point.a",
    "-ldbghelp",
    "extern/libsqlite3.a",
    "_deps/tracy-build/libTracyClient.a",
    "-lws2_32",
    "-ldbghelp",
    "-lsecur32",
    "_deps/zstd-build/lib/libzstd.a",
    "-lwbemuuid",
    "-lcomsuppw",
    "-lntdll",
    "-lDXGI",
    "extern/libimgui.a",
    "extern/libimgui_backends.a",
    "extern/libimgui.a",
    "extern/libimgui_backends.a",
    "_deps/freetype-build/libfreetyped.a",
    "_deps/zlib-build/libzlib.dll.a",
    "_deps/sdl3_prebuilt-src/lib/SDL3.lib",
    "_deps/dawn_prebuilt-src/lib/webgpu_dawn.lib",
    "-lkernel32", "-luser32", "-lgdi32", "-lwinspool", "-lshell32",
    "-lole32", "-loleaut32", "-luuid", "-lcomdlg32", "-ladvapi32",
    "-lpsapi",  # the crash/RSS dbghelp+psapi consumers -- platform/host/host_win32.c
]

# Runtime DLLs the linked exe needs sitting next to it (examples/CMakeLists.txt
# never calls aurora_copy_runtime_dlls() for the `simple` target either --
# an upstream oversight -- so this driver does the equivalent copy itself,
# there being no CMake step to invoke here).
AURORA_RUNTIME_DLLS = [
    # From the RMLUI tree, matching the archives the windowed exe now
    # links (same pinned prebuilts/fetches as aurora/build's copies).
    os.path.join(AURORA_DEPS_RMLUI, "dawn_prebuilt-src", "bin", "webgpu_dawn.dll"),
    os.path.join(AURORA_DEPS_RMLUI, "dawn_prebuilt-src", "bin", "dxcompiler.dll"),
    os.path.join(AURORA_DEPS_RMLUI, "dawn_prebuilt-src", "bin", "dxil.dll"),
    os.path.join(AURORA_DEPS_RMLUI, "sdl3_prebuilt-src", "bin", "SDL3.dll"),
    os.path.join(AURORA_DEPS_RMLUI, "png-build", "libpng16d.dll"),
    os.path.join(AURORA_DEPS_RMLUI, "zlib-build", "libzlib1.dll"),
    # nod runtime for the launcher's disc-image
    # import (content_import.cpp links the import lib; the DLL is the
    # official prebuilt package fetched by tools/fetch_nod.py). NOD_WIN_DLL
    # is defined above -- inlined here to keep this list a plain constant.
    os.path.join(PORT_ROOT, "toolchain", "nod", "windows-x86_64", "bin", "nod.dll"),
]
# ---------------------------------------------------------------------------
# aurora for Android -- the same
# consumption model as Windows (aurora is built in its OWN CMake tree; this
# driver only reads the already-built artifacts), pointed at the
# build-android/ sibling of the Windows build/ tree:
#
#   cmake -S external_refs/repos/aurora -B .../aurora/build-android -G Ninja
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
#     -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
#     -DAURORA_DAWN_PROVIDER=package        (prebuilt dawn-android-aarch64 --
#                                            the pin's own provider table has
#                                            an android row, no from-source
#                                            Dawn build needed)
#     -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=shared
#     -DBUILD_SHARED_LIBS=OFF -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
#   cmake --build .../build-android --target
#     aurora_core aurora_gx aurora_vi aurora_pad aurora_si aurora_card
#     aurora_mtx SDL3-shared
#
# SDL3 is STATIC inside libmp6game.so (AURORA_SDL3_LINKAGE=static) because
# aurora's own android path requires it: lib/window.cpp's SurfaceLock calls
# SDL-INTERNAL Android_Lock/UnlockActivityMutex, which no shared libSDL3.so
# exports (hidden symbols; verified with llvm-nm -D) -- static linking into
# the same .so is the only shape that resolves them, and it is exactly the
# single-.so shape the MP4 port ships. The JNI consequence: SDL3's
# RegisterNatives-based JNI_OnLoad lives inside the LOW-loaded game image,
# so the APK bootstrap (platform/android/mp6shell.c, libmain.so) chain-calls
# it after android_dlopen_ext -- see that file's header for the full story.
# Dawn/aurora/fmt/etc are static archives exactly like the Windows exe.
AURORA_BUILD_ANDROID = os.path.join(AURORA_ROOT, "build-android")
AURORA_DEPS_ANDROID = os.path.join(AURORA_BUILD_ANDROID, "_deps")
ANDROID_AURORA_OUT_DIR = os.path.join(ANDROID_BUILD_DIR, "aurora")  # libmp6game.so + libmain.so

# ---------------------------------------------------------------------------
# the RmlUi-ENABLED android aurora tree -- the
# exact same tree-split made earlier for Windows (AURORA_BUILD_RMLUI beside
# AURORA_BUILD): AURORA_ENABLE_RMLUI is a PUBLIC define that changes the
# aurora_core/aurora_gx archive CONTENT, so the android windowed row now
# consumes this sibling tree wholesale (mixing archives across trees would
# be an ODR hazard) while build-android stays byte-untouched for future
# re-runs. Same recipe + the one flag:
#
#   cmake -S external_refs/repos/aurora -B .../aurora/build-android-rmlui -G Ninja
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
#     -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
#     -DAURORA_DAWN_PROVIDER=package
#     -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static
#     -DBUILD_SHARED_LIBS=OFF -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
#     -DAURORA_ENABLE_RMLUI=ON
#   cmake --build .../build-android-rmlui --target
#     aurora_core aurora_gx aurora_vi aurora_pad aurora_si aurora_card aurora_mtx
#   (rmlui/rmlui_backends/freetype build as aurora_core dependencies; the
#    ninja target names differ from the CMake ones, so they are not listed.)
AURORA_BUILD_ANDROID_RMLUI = os.path.join(AURORA_ROOT, "build-android-rmlui")
AURORA_DEPS_ANDROID_RMLUI = os.path.join(AURORA_BUILD_ANDROID_RMLUI, "_deps")

# nod (encounter/nod v2.0.0-alpha.10 -- the exact version aurora's own
# dependency table pins for AURORA_ENABLE_DVD; dual-licensed MIT OR
# Apache-2.0) backs the launcher's disc-image import
# (platform/content/content_import.cpp). Machine-local toolchain artifacts
# under port/toolchain/nod (the port/toolchain convention -- zig/rust live
# the same way), populated by `python tools/fetch_nod.py`: the official
# prebuilt Windows package (nod.dll + import lib -- an MSVC DLL is
# ABI-neutral C to consume, exactly like the SDL3.lib/webgpu_dawn.lib
# prebuilts already on the Windows link line) and a cargo cross-build of
# the nod-ffi staticlib for android (no android prebuilt exists upstream).
NOD_DIR = os.path.join(PORT_ROOT, "toolchain", "nod")
NOD_INCLUDE = os.path.join(NOD_DIR, "include")
NOD_WIN_LIB = os.path.join(NOD_DIR, "windows-x86_64", "lib", "nod.lib")
NOD_WIN_DLL = os.path.join(NOD_DIR, "windows-x86_64", "bin", "nod.dll")
NOD_ANDROID_LIB = os.path.join(NOD_DIR, "android-aarch64", "libnod.a")
NOD_RECIPE_HINT = "run `python tools/fetch_nod.py`"


def android_aurora_flags():
    """The android row's AURORA_FLAGS -- same deliberate mirror
    discipline as android_common_flags() vs COMMON_FLAGS: identical include
    ORDER and AURORA_DEFINES, with only the target differences (NDK triple,
    -fPIC, plain -g DWARF) plus the include-dir differences that fall
    directly out of the android tree's own providers (verified against
    build-android/build.ninja's simple.c compile rule, the same
    proven-line-mirroring technique the Windows AURORA_COMPILE_INCLUDES
    used): SDL3 is vendor-built from source there (sdl-src/include +
    sdl-build/include-revision, which replace the Windows
    sdl3_prebuilt-src/include), and Dawn is the prebuilt android package
    (dawn_prebuilt-src/include, same dir name as Windows).

    The _deps roots now come from the RmlUi-enabled android tree
    (AURORA_DEPS_ANDROID_RMLUI) so every include stays tree-consistent with
    the archives the windowed link consumes -- the same wholesale switch
    _resolve_aurora_link_items made for Windows. Same third-party
    pins, so header content is identical; only the tree location moves."""
    return [
        "-target", ANDROID_TRIPLE,
        "-g",
        "-fPIC",
        "-std=gnu11",
        "-I", SHIM_INCLUDE,
        "-I", HOST_INCLUDE,
        "-I", AURORA_INCLUDE,
        "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "fmt-src", "include"),
        "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "sdl-build", "include-revision"),
        "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "sdl-src", "include"),
        "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "xxhash-src", "cmake_unofficial", ".."),
        "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "imgui-src"),
        "-isystem", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "dawn_prebuilt-src", "include"),
    ] + AURORA_DEFINES


# Exact archive list + order from build-android/build.ninja's own link
# line for examples/simple (the same verbatim-mirror rule AURORA_LINK_ITEMS
# follows for Windows -- the android tree's RelWithDebInfo names drop the
# Windows Debug 'd' suffixes), with exactly these deliberate deltas:
#   - libaurora_main.a EXCLUDED: lib/main.cpp's wrapper would export an
#     SDL_main from the GAME .so; on android the SDL_main lives in the
#     libmain.so bootstrap shell (platform/android/mp6shell.c) instead, and
#     the game .so exports aurora_main directly (main_native.c's renamed
#     main) for the shell to dlsym.
#   - aurora_pad/si/card/mtx PREPENDED, same reasoning+order as the Windows
#     list (they are not part of simple's own link closure).
#   - the sysroot's absolute libz.so path becomes plain -lz (bionic's public
#     libz -- also what the headless .so links for the game's zlib use).
ANDROID_AURORA_LINK_ITEMS = [
    "libaurora_pad.a", "libaurora_si.a",
    "libaurora_card.a",
    "libaurora_mtx.a",
    "libaurora_core.a", "libaurora_gx.a", "libaurora_vi.a",
    "_deps/png-build/libpng16.a",
    "-lm",
    "libaurora_core.a",  # CMake emits aurora_core twice; kept to match exactly
    # the RmlUi module archives, same position
    # and ordering rationale as the Windows AURORA_LINK_ITEMS entries
    # (backends before core, after the aurora archives, before freetype).
    "extern/librmlui_backends.a",
    "librmlui.a",
    "_deps/fmt-build/libfmt.a",
    "_deps/xxhash-build/libxxhash.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cord.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_info.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cord_internal.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_functions.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_cordz_handle.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_cord_state.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc32c.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_internal.a",
    "_deps/abseil-cpp-build/absl/crc/libabsl_crc_cpu_detect.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_str_format_internal.a",
    "_deps/abseil-cpp-build/absl/container/libabsl_raw_hash_set.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_hash.a",
    "_deps/abseil-cpp-build/absl/types/libabsl_bad_optional_access.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_city.a",
    "_deps/abseil-cpp-build/absl/types/libabsl_bad_variant_access.a",
    "_deps/abseil-cpp-build/absl/hash/libabsl_low_level_hash.a",
    "_deps/abseil-cpp-build/absl/container/libabsl_hashtablez_sampler.a",
    "_deps/abseil-cpp-build/absl/profiling/libabsl_exponential_biased.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_synchronization.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_stacktrace.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_graphcycles_internal.a",
    "_deps/abseil-cpp-build/absl/synchronization/libabsl_kernel_timeout_internal.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_time.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_civil_time.a",
    "_deps/abseil-cpp-build/absl/time/libabsl_time_zone.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_symbolize.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_strings.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_strings_internal.a",
    "_deps/abseil-cpp-build/absl/strings/libabsl_string_view.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_throw_delegate.a",
    "_deps/abseil-cpp-build/absl/numeric/libabsl_int128.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_debugging_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_malloc_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_base.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_raw_logging_internal.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_log_severity.a",
    "_deps/abseil-cpp-build/absl/base/libabsl_spinlock_wait.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_internal.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_rust.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_decode_rust_punycode.a",
    "_deps/abseil-cpp-build/absl/debugging/libabsl_utf8_for_code_point.a",
    "extern/libsqlite3.a",
    "_deps/tracy-build/libTracyClient.a",
    "_deps/zstd-build/lib/libzstd.a",
    "extern/libimgui.a",
    "extern/libimgui_backends.a",
    "extern/libimgui.a",
    "extern/libimgui_backends.a",
    "_deps/freetype-build/libfreetype.a",
    "-lz",
    "_deps/sdl-build/libSDL3.a",
    "-lm", "-lOpenSLES", "-lGLESv1_CM", "-lGLESv2",
    "_deps/dawn_prebuilt-src/lib/libwebgpu_dawn.a",
    "-ldl", "-llog", "-landroid", "-pthread", "-latomic", "-lm",
]


def _resolve_android_aurora_link_items():
    """Same resolution rule as _resolve_aurora_link_items(), against the
    android tree, against the RmlUi-enabled android tree
    (AURORA_BUILD_ANDROID_RMLUI) -- the wholesale-tree switch made for
    Windows, for the same PUBLIC-define/ODR reason; build-android stays
    reserved for future byte-compare re-runs."""
    out = []
    for item in ANDROID_AURORA_LINK_ITEMS:
        if item.startswith("-"):
            out.append(item)
        else:
            out.append(os.path.normpath(os.path.join(AURORA_BUILD_ANDROID_RMLUI, item)).replace("\\", "/"))
    return out

# Pre-generated .inc data blobs (font bitmaps, decode tables, splash-screen
# packed sprites, ...) #include-d directly by several game/*.c and
# REL/bootDll/data.c files. These are the decomp's OWN build-tool output
# (asset conversion, not hand-written source) and already exist checked
# into the repo -- the real bytes are sitting right there, so there's no
# need to fake anything.
DECOMP_INC_DATA = os.path.join(DECOMP, "build", "GP6E01", "include")
PATCHED_INCLUDE = os.path.join(BUILD_DIR, "patched_include")
MSL_OVERRIDE = os.path.join(BUILD_DIR, "msl_override")

# ---------------------------------------------------------------------------
# MSL (Metrowerks Standard Library) override.
#
# include/ (the SAME directory as dolphin/, game/, msm/) ALSO vendors tiny
# MWCC-era stand-ins for ctype.h/float.h/math.h/stdarg.h/stddef.h/stdio.h/
# stdlib.h/string.h -- e.g. include/stdio.h is 12 lines (puts/printf/sprintf/
# vprintf/vsprintf only: no stdout/stderr/fprintf/fopen/fflush/...) and
# include/math.h's sqrtf/sqrt/fabs call __frsqrte/__fabs, MWCC-only PPC
# compiler builtins with no clang equivalent. docs/ARCHITECTURE.md already
# calls for MSL to be "replaced by the host's native libc", so these need to
# resolve to zig's bundled real libc headers instead -- but since they live
# in the SAME directory as headers we genuinely need (dolphin.h, ovl_table.h,
# game/*.h all resolve relative to this same include/ root), we can't just
# drop that -I entry, and neither plain -I ordering nor #include_next can
# selectively skip one file in a directory while keeping the rest (verified
# empirically): whichever of {our override dir, decomp's include/} is
# searched first always wins outright for a given
# filename, for every file in that directory, not just the ones we'd want
# to override. The fix: generate tiny shadow files with an ABSOLUTE #include
# straight to zig's bundled header, sidestepping search-order entirely.
MSL_OVERRIDE_NAMES = ["ctype.h", "float.h", "math.h", "stdarg.h", "stddef.h", "stdio.h", "stdlib.h", "string.h"]
ZIG_ROOT = os.path.dirname(ZIG)


def _find_zig_libc_dir(name):
    # any-windows-any covers most of libc; a couple of headers are only
    # under the arch+abi-specific dir, so fall back to it.
    candidates = [
        os.path.join(ZIG_ROOT, "lib", "libc", "include", "any-windows-any"),
        os.path.join(ZIG_ROOT, "lib", "libc", "include", "x86_64-windows-gnu"),
    ]
    for d in candidates:
        if os.path.exists(os.path.join(d, name)):
            return os.path.join(d, name)
    return None


def patch_msl_override(dst_dir=None, find_real=None, libc_desc="zig's bundled libc"):
    # Parameterized for the android target row (dst_dir=
    # ANDROID_MSL_OVERRIDE, find_real=_find_ndk_libc_dir) -- the shadow
    # files' CONTENT is an absolute path into the toolchain's real libc, so
    # each target needs its own generated set in its own build tree. The
    # no-arg call is byte-for-byte the original Windows behavior.
    if dst_dir is None:
        dst_dir = MSL_OVERRIDE
    if find_real is None:
        find_real = _find_zig_libc_dir
    os.makedirs(dst_dir, exist_ok=True)
    for name in MSL_OVERRIDE_NAMES:
        real = find_real(name)
        dst = os.path.join(dst_dir, name)
        if not real:
            print(f"[WARN] patch_msl_override: couldn't find the real {name} ({libc_desc}), "
                  f"leaving decomp's MSL stub active")
            continue
        real_posix = real.replace("\\", "/")
        content = (
            f"/* MP6 native port: forces the REAL system {name} ({libc_desc}) instead "
            f"of the decomp's own tiny vendored MSL-era include/{name} stub. See "
            f"tools/build.py's patch_msl_override(). */\n"
            f'#include "{real_posix}"\n'
        )
        _write_if_changed(dst, content)

    # include/humath.h -- itself living in include/ (top-level), same as
    # the real include/math.h it does `#include "math.h"` on -- resolves
    # that quote-include to the real (unpatched, __frsqrte/__fabs-using)
    # decomp math.h no matter what's on -I, via the same "includer's own
    # directory first" rule that makes game/msm.h unshadowable (see
    # dolphin_compat.h's old MSMSE comment, now moved to
    # shim/include/selmenu_compat.h). humath.h itself is small enough to
    # just special-case directly instead of adding another generic
    # mechanism for a single file.
    humath_real = find_real("math.h")
    if humath_real:
        src = os.path.join(DECOMP_INCLUDE, "humath.h")
        dst = os.path.join(dst_dir, "humath.h")
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            real_text = f.read()
        real_posix = humath_real.replace("\\", "/")
        patched, n = re.subn(r'#include\s+"math\.h"', f'#include "{real_posix}"', real_text, count=1)
        if n != 1:
            print(f"[WARN] patch_msl_override: expected exactly 1 '#include \"math.h\"' in "
                  f"include/humath.h, found {n} -- decomp header may have changed upstream")
        header_note = (
            "/* MP6 native port: shadow of include/humath.h -- itself living in "
            "include/ (top-level), same as the real include/math.h it does "
            '`#include "math.h"` on -- resolves that quote-include to the real '
            "(unpatched, __frsqrte/__fabs-using) decomp math.h no matter what's on "
            "-I, via the same \"includer's own directory first\" rule that makes "
            "game/msm.h unshadowable (see shim/include/selmenu_compat.h). Only that "
            "one line is substituted (regex, same technique as patch_headers()'s "
            "AT_ADDRESS fix) -- everything else is byte-identical to the real file, "
            "regenerated fresh every build so it can't silently drift out of sync "
            "(a full-file hand-transcription here once dropped 2 of humath.h's "
            "macros by copy-paste mistake, silently link-breaking 3 unrelated "
            "files). */\n"
        )
        _write_if_changed(dst, header_note + patched)
    else:
        print("[WARN] patch_msl_override: couldn't find the real math.h for the humath.h shadow")

# ---------------------------------------------------------------------------
# AT_ADDRESS(xyz) header patch. include/dolphin/os.h defines
#     #ifdef __MWERKS__
#     #define AT_ADDRESS(xyz) : (xyz)
#     #else
#     #define AT_ADDRESS
#     #endif
# with NO `#ifndef AT_ADDRESS` guard, so a force-included compat header can
# never win: whichever definition is lexically active AT THE USE SITE wins,
# and os.h's own (non-MWERKS) branch always redefines it to an OBJECT-like
# empty macro right before using it. An object-like macro never consumes a
# following `(...)` (that's only a function-like-macro behavior), so
# `u32 __OSBusClock AT_ADDRESS(OS_BASE_CACHED | 0x00F8);` expands to
# `u32 __OSBusClock (OS_BASE_CACHED | 0x00F8);` -- which clang parses as an
# (invalid) function declarator, not a variable declaration. This isn't
# fixable from a force-included header (empirically verified: a
# function-like pre-definition is simply overwritten by os.h's own
# object-like one before the first use). The 5 affected declarations (3 in
# os.h, 2 in OSExec.h) are mechanically patched into a build-generated
# shadow copy that's searched BEFORE the real decomp include dir, so
# `#include <dolphin/os.h>` resolves to the patched copy -- the real file
# under external_refs/repos/marioparty6 is never touched, and the patch is
# regenerated fresh from it on every build (can't silently go stale).
AT_ADDRESS_PATCH_FILES = ["dolphin/os.h", "dolphin/os/OSExec.h"]
_AT_ADDRESS_RE = re.compile(r"^(.*\S)[ \t]+AT_ADDRESS\([^()]*\)([ \t]*;.*)$", re.MULTILINE)

# include/game/msm.h and the UNRELATED top-level include/msm.h both guard
# themselves with `#ifndef MSM_H` / `#define MSM_H` -- apparently by
# coincidence, not design. Tried decoupling them (rename one's guard so
# both get fully processed regardless of order) and that made things
# WORSE: the two headers turn out to declare ~30 of the SAME functions
# (msmSeGetIndexPtr, msmMusSetMasterVolume, ...) with subtly different,
# incompatible parameter/return types (game/msm.h's own game/msm_data.h
# has its OWN independent `MSM_SE` struct, distinct from top-level msm.h's
# `MSMSE`) -- decoupling surfaces a pile of "conflicting types for ..."
# errors that the original, never-both-visible-at-once guard collision
# was accidentally (or deliberately) hiding. So: LEAVE the shared guard
# alone, letting whichever header a given TU reaches first keep winning
# (matching what real compilation always did) -- and patch over the only
# 2 gaps that leaves in the slice's actual call sites (both verified via
# `grep -rl` to have exactly one user each, so the fix is precisely
# targeted, not a blanket alias): MSMSE (src/REL/selmenuDll/selmenu.c
# needs the top-level name; only game/msm.h's MSM_SE is ever visible to
# it) and MSM_STREAMNO_NONE (src/game/audio.c needs it; only defined in
# the top-level header, never reached from audio.c's side of the guard
# race). See shim/include/dolphin_compat.h.
GUARD_RENAME_PATCH_FILES = []

# Two structs decomp's own headers size
# SMALLER than external_refs/repos/aurora's real, always-compiled-with-
# TARGET_PC ABI (Aurora's own build always defines TARGET_PC -- see
# AURORA_DEFINES above -- so its compiled libaurora_gx.a/libaurora_pad.a
# genuinely expect the LARGER size below, regardless of what THIS build
# defines). Both were found by diffing decomp's include/dolphin/gx/
# GXStruct.h and include/dolphin/pad.h against Aurora's own copies
# byte-for-byte:
#   - GXTlutObj: decomp's TARGET_PC branch is `u32 dummy[4]` (16 bytes);
#     Aurora's real ABI is `u32 dummy[10]` (40 bytes). game/hsfdraw.c and
#     game/sprput.c both allocate a GXTlutObj on the stack and pass its
#     address to GXInitTlutObj/GXLoadTlut (confirmed real, non-speculative
#     call sites via grep) -- linking those straight to Aurora's real,
#     40-byte-writing GXInitTlutObj with only a 16-byte decomp allocation
#     backing it is a genuine stack-buffer overflow, not a theoretical one.
#   - PADStatus: decomp's copy has NO `#ifdef TARGET_PC` branch for this
#     struct at all (always the 10-field, ~12-byte-with-padding shape);
#     Aurora's real ABI adds a trailing `u32 extButton` under TARGET_PC
#     (16 bytes total). game/pad.c's PADRead(&status) call passes a
#     decomp-sized PADStatus straight to Aurora's real PADRead, which may
#     write that trailing field -- a 4-byte overflow past decomp's
#     allocation.
# Fixed the SAME way as AT_ADDRESS above (a build-generated shadow copy,
# regenerated fresh from the real header every build) rather than a hand-
# written override in shim/include/dolphin_compat.h, since these are pure
# facts about the header's OWN declared shape, not per-call-site adapters.
# Bumping the two struct sizes is harmless for the --headless build too
# (nothing there ever writes past decomp's own original bounds); all three
# patches are applied unconditionally in both build modes for the same
# reason -- one shadow-header code path instead of two.
#
# A third entry here (GXGeometry.h's GXSetArray) was added only after
# hitting a real compile error: decomp's OWN call sites (game/hsfdraw.c,
# game/hsfanim.c, game/sprput.c, game/printfunc.c -- confirmed via grep,
# every one of them) use the REAL-hardware 3-argument shape
# (GXSetArray(attr, data, stride), no explicit size -- real hardware
# addresses memory directly, no GPU buffer to size) -- NOT the 4-argument
# `#ifdef TARGET_PC` branch this build's -DTARGET_PC would otherwise
# select (decomp's own header still declares a TARGET_PC branch, it's just
# never actually how MP6's own code calls it, unlike GXVert.h/GXBump.h/
# GXTransform.h's TARGET_PC branches, which DO match real MP6 call sites).
# Fixed by making the shadow copy declare the unconditional 3-arg shape
# regardless of TARGET_PC -- matching real usage -- rather than editing
# the decomp's call sites (which would violate "decomp is read-only") or
# leaving the ambient -DTARGET_PC flip this one declaration under it.
# The remaining arity gap against Aurora's real (5-arg) GXSetArray is
# closed via a rename, not a linker trick -- see shim/include/
# dolphin_compat.h's own section-3 comment and platform/gx/
# aurora_bridge.c's mp6_GXSetArray3 for why (short version: this
# toolchain's zig cc/c++ rejects -Wl,--wrap outright, and a link-order
# trick was tested and does NOT work either -- lld-link hard-errors with
# "duplicate symbol" once the archive member providing Aurora's real
# GXSetArray gets pulled in for other symbols regardless).
HEADER_CONTENT_PATCHES = [
    (
        "dolphin/gx/GXStruct.h",
        re.compile(
            r"(typedef struct _GXTlutObj \{\s*#ifdef TARGET_PC\s*)u32 dummy\[4\];",
            re.MULTILINE,
        ),
        r"\1u32 dummy[10];",
        "GXTlutObj TARGET_PC size 4 -> 10 u32s (16 -> 40 bytes, matching Aurora's real ABI)",
    ),
    (
        "dolphin/pad.h",
        re.compile(r"(\n\s*s8 err;\s*\n)\} PADStatus;"),
        r"\1#ifdef TARGET_PC\n  u32 extButton;\n#endif\n} PADStatus;",
        "PADStatus: added trailing TARGET_PC-gated u32 extButton (matching Aurora's real ABI)",
    ),
    (
        "dolphin/gx/GXGeometry.h",
        re.compile(
            r"#ifdef TARGET_PC\s*\n"
            r"void GXSetArray\(GXAttr attr, const void\* data, u32 size, u8 stride\);\s*\n"
            r"#else\s*\n"
            r"void GXSetArray\(GXAttr attr, void\* data, u8 stride\);\s*\n"
            r"#endif",
        ),
        "void GXSetArray(GXAttr attr, void* data, u8 stride);",
        "GXSetArray: unconditional real-hardware 3-arg shape (matching decomp's actual call "
        "sites) regardless of TARGET_PC -- see platform/gx/aurora_bridge.c for the 5-arg adapter",
    ),
]

# android-ONLY additions to the shadow-
# header set (applied on top of HEADER_CONTENT_PATCHES, into the android
# tree's own patched_include -- the Windows tree never sees these).
#
# dolphin/types.h's BOOL: the decomp's own header says
#     #if defined(TARGET_PC) && !defined(_WIN32)   -> typedef bool BOOL (1 byte)
#     #else                                        -> typedef int  BOOL (4 bytes)
# On Windows (_WIN32) BOOL has therefore always been the 4-byte int -- which
# is also exactly what MWCC/PPC BOOL was on real hardware, i.e. what every
# decomp struct layout, on-disc overlay and save layout was authored
# against (platform/os/save_endian.c's _Static_asserts pin some of them).
# NDK clang defines neither _WIN32 nor anything else in that condition, so
# an unpatched android build would silently flip every BOOL to a 1-byte
# C99 bool -- a whole-image struct-layout divergence from both real
# hardware and the Windows anchor. Forcing the #else branch keeps BOOL the
# 4-byte int everywhere, matching Windows bit-for-bit.
ANDROID_HEADER_CONTENT_PATCHES = [
    (
        "dolphin/types.h",
        re.compile(r"#if defined\(TARGET_PC\) && !defined\(_WIN32\)"),
        "#if 0 /* MP6 native port : forced OFF -- BOOL must stay the 4-byte int of "
        "real hardware and the Windows anchor; see tools/build.py's "
        "ANDROID_HEADER_CONTENT_PATCHES */",
        "BOOL: 4-byte int on android too (decomp's TARGET_PC&&!_WIN32 branch would make it "
        "1-byte C99 bool, diverging every BOOL-bearing struct layout from GC/Windows)",
    ),
    # s64/u64 as literal `long long`: on LP64 (android aarch64) stdint's
    # int64_t/uint64_t are plain `long`, a DIFFERENT type (same width) from
    # the `long long` include/musyx/musyx.h's own MUSY_TARGET_PC branch
    # typedefs for the same u64 name -- C's typedef-redefinition rule then
    # errors in every TU that sees both (all of game/audio.h's ~40
    # includers + msm_bridge/shims). On Windows LLP64 the two spellings
    # were the SAME type, which is the only reason this never bit there
    # (same class as dolphin_compat.h's MUSY_TARGET note). Width/layout
    # unchanged -- this is a C type-identity fix, not an ABI change.
    (
        "dolphin/types.h",
        re.compile(r"typedef int64_t s64;"),
        "typedef signed long long s64; /* 'long long' spelling, see build.py */",
        "s64: long-long spelling so musyx.h's own s64/u64 typedefs stay compatible on LP64",
    ),
    (
        "dolphin/types.h",
        re.compile(r"typedef uint64_t u64;"),
        "typedef unsigned long long u64; /* 'long long' spelling, see build.py */",
        "u64: long-long spelling so musyx.h's own s64/u64 typedefs stay compatible on LP64",
    ),
]


def patch_abi_struct_headers(dst_root=None, extra_patches=()):
    # Parameterized like patch_msl_override -- the android target row
    # writes into its own tree AND appends ANDROID_HEADER_CONTENT_PATCHES
    # (dolphin/types.h, see that table's own comment). The no-arg call is
    # byte-for-byte the original Windows behavior (every Windows entry has
    # a unique rel, so the per-file grouping below degenerates to the old
    # one-patch-one-write flow with an identical header note).
    if dst_root is None:
        dst_root = PATCHED_INCLUDE
    # Group patches by target header: multiple entries may patch the SAME
    # file (the android dolphin/types.h set does) -- each file is read
    # once, every one of its patterns applied in table order, written once.
    # A naive per-entry read-patch-write loop would silently drop all but
    # the last entry for a shared rel (each entry re-reading the ORIGINAL).
    by_rel = {}
    order = []
    for rel, pattern, replacement, desc in list(HEADER_CONTENT_PATCHES) + list(extra_patches):
        if rel not in by_rel:
            by_rel[rel] = []
            order.append(rel)
        by_rel[rel].append((pattern, replacement, desc))
    for rel in order:
        src = os.path.join(DECOMP_INCLUDE, rel.replace("/", os.sep))
        dst = os.path.join(dst_root, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        descs = []
        for pattern, replacement, desc in by_rel[rel]:
            text, n = pattern.subn(replacement, text)
            if n != 1:
                print(f"[WARN] patch_abi_struct_headers: expected exactly 1 match ({desc}) in {rel}, found {n} "
                      f"-- decomp header may have changed upstream of the pinned commit")
            descs.append(desc)
        header_note = (
            "/* MP6 native port: build-generated shadow copy of this header with a content fix "
            "applied (see tools/build.py's HEADER_CONTENT_PATCHES for "
            f"why): {'; '.join(descs)}. Regenerated from the real "
            "external_refs/repos/marioparty6/include/" + rel + " on every build -- do not "
            "hand-edit. */\n"
        )
        _write_if_changed(dst, header_note + text)


def apply_decomp_override_headers(dst_root=None):
    """Decomp-overrides shield, header half (see resolve_source() for the
    C-file half and the full story): copies every header materialized under
    patches/decomp-overrides/include/ into the build-generated patched-
    include tree, which is already searched BEFORE the decomp's include/ --
    so a foreign lane's broken in-flight header WIP in the SHARED decomp
    checkout can't break this port's builds, without touching that WIP.
    Runs for both the Windows and android rows."""
    if dst_root is None:
        dst_root = PATCHED_INCLUDE
    src_root = os.path.join(NATIVE_ROOT, "patches", "decomp-overrides", "include")
    if not os.path.isdir(src_root):
        return
    for root, _dirs, files in os.walk(src_root):
        rel = os.path.relpath(root, src_root)
        for fn in files:
            src = os.path.join(root, fn)
            dst_dir = os.path.join(dst_root, rel) if rel != "." else dst_root
            os.makedirs(dst_dir, exist_ok=True)
            with open(src, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
            _write_if_changed(os.path.join(dst_dir, fn), content)


def patch_headers(dst_root=None):
    # Parameterized like patch_abi_struct_headers; the no-arg call is
    # byte-for-byte the original Windows behavior.
    if dst_root is None:
        dst_root = PATCHED_INCLUDE
    for rel in AT_ADDRESS_PATCH_FILES:
        src = os.path.join(DECOMP_INCLUDE, rel.replace("/", os.sep))
        dst = os.path.join(dst_root, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        patched, n = _AT_ADDRESS_RE.subn(r"\1\2", text)
        if n == 0:
            print(f"[WARN] patch_headers: expected >=1 AT_ADDRESS(...) declaration in {rel}, found 0 "
                  f"-- decomp header may have changed upstream of the pinned commit")
        header_note = (
            "/* MP6 native port: build-generated shadow copy of this header with its "
            "AT_ADDRESS(...) absolute-address declarations stripped (see tools/build.py's "
            "patch_headers()/AT_ADDRESS_PATCH_FILES for why). Regenerated from the real "
            "external_refs/repos/marioparty6/include/" + rel + " on every build -- do not "
            "hand-edit. */\n"
        )
        _write_if_changed(dst, header_note + patched)

    for rel, old_guard, new_guard in GUARD_RENAME_PATCH_FILES:
        src = os.path.join(DECOMP_INCLUDE, rel.replace("/", os.sep))
        dst = os.path.join(dst_root, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        patched, n = re.subn(r"\b" + re.escape(old_guard) + r"\b", new_guard, text)
        if n < 2:
            print(f"[WARN] patch_headers: expected >=2 occurrences of guard {old_guard} in {rel}, found {n}")
        header_note = (
            f"/* MP6 native port: build-generated shadow copy of this header with its include "
            f"guard renamed {old_guard} -> {new_guard} (see tools/build.py's "
            f"GUARD_RENAME_PATCH_FILES for why -- it collides with an unrelated header's guard "
            f"name otherwise). Regenerated from the real "
            f"external_refs/repos/marioparty6/include/{rel} on every build -- do not hand-edit. */\n"
        )
        _write_if_changed(dst, header_note + patched)


COMMON_FLAGS = [
    "-target", TARGET,
    # The link already emits build/mp6native.pdb unconditionally (Aurora's
    # own prebuilt .a archives carry CodeView debug info from their own
    # CMake build), but this port's own decomp/platform TUs need their own
    # debug-info flag too -- otherwise every crash address landing inside
    # this port's own code (as opposed to an Aurora-side frame) resolves to
    # a flat "<no symbol>" from both the crash handler's (platform/host/
    # host_win32.c) in-process SymFromAddr AND an ad hoc standalone dbghelp
    # script, even though the crash is firmly inside the game's own
    # 0x10000000+ module range. `-gcodeview` (not plain `-g`'s default
    # DWARF) is clang's explicit "emit MSVC/PDB-compatible debug info"
    # flag, supported regardless of the windows-gnu vs windows-msvc target
    # environment (it controls only the debug-info FORMAT, not the
    # CRT/ABI) -- exactly what dbghelp/PDB tooling needs. `-g` alone is
    # required too (it's what actually turns debug-info emission on;
    # `-gcodeview` only selects the format).
    "-g", "-gcodeview",
    "-funsigned-char",
    "-fcommon",
    # zig cc injects its own runtime panics for signed-overflow UB by
    # default (unlike a plain clang/gcc -O0, which just wraps silently) --
    # game/frand.c's Park-Miller RNG (`m = seed - (k * 0x1F31D)`, the
    # classic 16807/127773/2836 minimal-standard-RNG constants) relies on
    # int32 wraparound as NORMAL operation, same as every other compiler
    # this decompiled code has ever actually been built with. -fwrapv
    # makes signed overflow well-defined two's-complement wraparound
    # instead of UB, matching real hardware, and turns off the trap.
    "-fwrapv",
    # Defined globally, for every decomp
    # TU, not just the GX headers. Flips include/dolphin/gx/{GXVert,GXBump,
    # GXGeometry,GXTransform}.h and pad.h/vi.h/types.h onto their "PC" `#ifdef
    # TARGET_PC` branches (real `extern` prototypes instead of raw
    # GXFIFO_ADDR/write-gather-pipe MMIO pokes for the whole GXVert.h vertex-
    # submission family, in particular) -- verified byte-for-byte against
    # external_refs/repos/aurora's OWN copy of every such header (both are
    # independent forks of the same original PC-compat header set) before
    # committing to this: the TARGET_PC branch's declarations match Aurora's
    # exactly wherever both trees declare the same name, which is what lets
    # the game's real GX/VI/PAD calls link straight against Aurora's
    # compiled implementations. The few places they do NOT match (GXSetArray
    # gains a trailing `bool le` parameter; GXTlutObj/PADStatus are smaller
    # in decomp's copy than in Aurora's real, always-TARGET_PC-compiled ABI)
    # are reconciled without editing the decomp: GXSetArray via a
    # preprocessor rename to a uniquely-named bridge function (see
    # shim/include/dolphin_compat.h's own section-3 comment and
    # platform/gx/aurora_bridge.c's mp6_GXSetArray3 for why a linker
    # --wrap -- tried first -- doesn't work on this toolchain), the two
    # structs via the SAME build-generated shadow-header technique
    # patch_headers() already used for AT_ADDRESS (see
    # HEADER_CONTENT_PATCHES below).
    "-DTARGET_PC",
    # A standing, always-compiled diagnostic -- distinct from and
    # complementary to the RSS watchdog (platform/null/shims_manual.c's
    # mp6_rss_watchdog_check, always on, aborts past a cap): this one never
    # aborts, just periodically logs a per-HEAP breakdown (HEAP_HEAP/
    # HEAP_SOUND/HEAP_MODEL/HEAP_DVD), the "which heap is actually growing"
    # signal a leak hunt needs that the watchdog's own single RSS number
    # can't provide (see docs/TESTING.md's leak gate).
    "-DMP6_LEAKHUNT_DEBUG",
    "-w",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=int-conversion",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=return-type",
    "-I", MSL_OVERRIDE,
    "-I", PATCHED_INCLUDE,
    "-I", DECOMP_INCLUDE,
    "-I", DECOMP_INC_DATA,
    "-I", SHIM_INCLUDE,
    "-I", HOST_INCLUDE,  # platform/host/host.h (see HOST_INCLUDE's own comment)
    "-include", "dolphin_compat.h",
    # Only platform/dvd/dvd_files.c actually uses these, but every
    # other TU ignores an unused -D harmlessly -- simpler than adding a
    # per-file flags mechanism to PLATFORM_SOURCES_COMMON for 2 defines.
    f'-DMP6_DVD_FILES_ROOT="{MP6_DVD_FILES_ROOT}"',
    f'-DMP6_DVD_FST_PATH="{MP6_DVD_FST_PATH}"',
]


def android_common_flags():
    """The android row's COMMON_FLAGS -- a deliberate flag-for-flag
    mirror of COMMON_FLAGS above (same semantics flags, same -Wno-error
    set, same include order, same force-include; every entry's rationale
    lives with the Windows copy) with exactly these target differences:
      - `-target aarch64-linux-android28` (NDK clang; API level = the
        probe row's) instead of the zig windows-gnu triple;
      - `-fPIC` (the game image is a SHARED library placed low at runtime
        by android_dlopen_ext, not an exe linked at a fixed image base);
      - plain `-g` DWARF (no `-gcodeview`: that is PDB/dbghelp tooling,
        meaningless for ELF -- ndk-stack/llvm-symbolizer read DWARF);
      - the android build tree's own msl_override/patched_include dirs
        (generated content differs per target -- absolute libc paths and
        the ANDROID_HEADER_CONTENT_PATCHES);
      - the on-device MP6_DVD_* fallback paths (see ANDROID_DEVICE_BASE).
    Kept as its own list builder rather than a transform of COMMON_FLAGS so
    the Windows list stays a single untouched literal (a hard rule:
    Windows builds bit-for-bit unaffected)."""
    return [
        "-target", ANDROID_TRIPLE,
        "-g",
        "-fPIC",
        "-funsigned-char",
        "-fcommon",
        "-fwrapv",
        "-DTARGET_PC",
        "-DMP6_LEAKHUNT_DEBUG",
        "-w",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=int-conversion",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=return-type",
        "-I", ANDROID_MSL_OVERRIDE,
        "-I", ANDROID_PATCHED_INCLUDE,
        "-I", DECOMP_INCLUDE,
        "-I", DECOMP_INC_DATA,
        "-I", SHIM_INCLUDE,
        "-I", HOST_INCLUDE,
        "-include", "dolphin_compat.h",
        f'-DMP6_DVD_FILES_ROOT="{ANDROID_DVD_FILES_ROOT}"',
        f'-DMP6_DVD_FST_PATH="{ANDROID_DVD_FST_PATH}"',
        # src/game/hsfmotion.c (decomp, read-only) marks 2 functions
        # `__declspec(weak)`. The Windows toolchain accepts that spelling
        # natively (clang enables declspec for Windows triples); for ELF
        # this macro maps it to the GNU attribute -- the exact rewrite
        # MinGW's own _mingw.h historically did -- preserving the same
        # weak-definition semantics (nothing in this link overrides the
        # two symbols; verified by grep, so this is spelling, not
        # resolution). Android-only: the Windows command line is untouched.
        "-D__declspec(x)=__attribute__((x))",
    ]

# (relative-to-DECOMP source path, extra per-file -D flags)
GAME_SKIP_LIST = {
    "src/game/kerent.c":  "100% MWCC PPC inline asm (REL<->DOL kernel jump table); "
                            "meaningless for a monolithic native link, fake `void NAME(void)` "
                            "prototypes would conflict with real signatures elsewhere.",
    "src/game/jmp.c":     "gcsetjmp/gclongjmp as raw MWCC PPC asm; superseded (see process.c "
                            "entry below -- both replaced together by platform/os/process_native.c).",
    "src/game/malloc.c":  "7 functions use unguarded `asm { mflr retaddr }`; replaced by "
                            "platform/os/malloc_direct.c (__builtin_return_address(0)).",
    "src/game/process.c": "HuPrcCall's scheduling loop needs "
                            "gcsetjmp(&processjmpbuf) to 'return again', later, with a new "
                            "value, from arbitrary points after HuPrcCall's own subsequent "
                            "calls have reused that exact stack region -- undefined behavior "
                            "per C11 7.13.2.1p3, and empirically broken on this toolchain in "
                            "every form tried (real setjmp/longjmp, __builtin_setjmp/longjmp, "
                            "a manual stack-snapshot-and-restore around the pair). Real "
                            "hardware's gcsetjmp/gclongjmp are raw PPC asm with no such "
                            "constraint (they just poke sp/lr registers). Replaced in full by "
                            "platform/os/process_native.c, which reimplements HuPrcCall's "
                            "exact observable behavior (verified line-by-line) using ordinary, "
                            "same-stack C control flow -- 'dispatch a process and wait for it "
                            "to yield' becomes a plain function call/return, so nothing needs "
                            "processjmpbuf's broken resume trick at all. This also removes the "
                            "only caller of gcsetjmp/gclongjmp, so jmp.c's replacement "
                            "(formerly platform/os/jmp_native.c + hostjmp.c) is superseded too.",
}

# Resolves a decomp-relative source path (e.g.
# "src/game/decode.c") to build/patched-src/<same path> if apply_patches.py
# produced one (i.e. patches/decomp/<same path>.patch exists), else to the
# real, untouched decomp file. Checked fresh per build (not cached) so a
# patch added/removed between runs is picked up without a --clean.
def resolve_source(rel):
    # Decomp-overrides shield. The decomp checkout is SHARED across
    # lanes and consumed read-only here; when a foreign lane leaves broken
    # in-flight WIP in its own working tree (e.g. a source file modified to
    # include an untracked, incomplete generated header), this port must
    # neither fail nor touch that WIP. Files materialized
    # under patches/decomp-overrides/<rel> (pristine `git show HEAD:<rel>`
    # snapshots) take priority over the decomp working tree; remove the
    # override once the decomp lane lands or reverts its WIP.
    override = os.path.join(NATIVE_ROOT, "patches", "decomp-overrides", rel.replace("/", os.sep))
    if os.path.exists(override):
        return override
    patched = os.path.join(PATCHED_SRC_DIR, rel.replace("/", os.sep))
    if os.path.exists(patched):
        return patched
    return os.path.join(DECOMP, rel.replace("/", os.sep))


def game_sources():
    """Returns (decomp_rel_path, extra_flags) tuples -- rel path, NOT
    abs_path, so collect_units() can both special-case main.c by name and
    resolve_source() the same rel to whichever of {original, patched-src}
    should actually be compiled."""
    game_dir = os.path.join(DECOMP, "src", "game")
    out = []
    for f in sorted(os.listdir(game_dir)):
        if not f.endswith(".c"):
            continue
        rel = f"src/game/{f}"
        if rel in GAME_SKIP_LIST:
            continue
        out.append((rel, []))
    return out

# Each REL module is its own MWCC-era link unit on real hardware, so
# bootDll/selmenuDll/fileseldll each freely reuse the SAME per-module
# helper-function names (_prolog/_epilog/_ctors/_dtors -- the REL lifecycle
# entry points -- and ObjectSetup, a conventional "set up this module's
# own objects" helper each one's _prolog calls). Statically linking all
# three into one exe makes those genuine duplicate-symbol clashes; -D
# renames each one per-file (verified for ObjectSetup: called only from
# its own file's _prolog in all 3, same as boot.c's other renamed names).
REL_SOURCES = [
    ("src/REL/bootDll/boot.c", ["-D_prolog=bootDll_prolog", "-D_epilog=bootDll_epilog",
                                 "-D_ctors=bootDll_ctors", "-D_dtors=bootDll_dtors",
                                 "-DObjectSetup=bootDll_ObjectSetup"]),
    ("src/REL/bootDll/data.c", []),
    ("src/REL/bootDll/opening.c", []),
    ("src/REL/selmenuDll/selmenu.c", ["-D_prolog=selmenuDll_prolog", "-D_epilog=selmenuDll_epilog",
                                       "-D_ctors=selmenuDll_ctors", "-D_dtors=selmenuDll_dtors",
                                       "-DObjectSetup=selmenuDll_ObjectSetup",
                                       # see shim/include/selmenu_compat.h for why this is
                                       # per-file rather than in the global dolphin_compat.h
                                       "-include", "selmenu_compat.h"]),
    ("src/REL/fileseldll/filename.c", []),
    ("src/REL/fileseldll/filesel.c", ["-D_prolog=fileselDll_prolog", "-D_epilog=fileselDll_epilog",
                                        "-D_ctors=fileselDll_ctors", "-D_dtors=fileselDll_dtors",
                                        "-DObjectSetup=fileselDll_ObjectSetup"]),
    ("src/REL/fileseldll/saveload.c", []),
    # mdseldll (mode select -- the overlay file-select's own no-card flow
    # proceeds to unconditionally, ovl_table.h index 93). The decomp's
    # configure.py marks mdsel.c Matching and the built REL is
    # byte-identical to the disc original.
    # src/REL/mdseldll/runtime.c is deliberately NOT listed -- it's a
    # 1-line #include of src/Runtime.PPCEABI.H/runtime.c (the MWCC/PPC REL
    # runtime: division helpers etc.), which this port replaces with the
    # native toolchain's own runtime by construction -- exactly like the
    # other 3 RELs, whose REL-runtime objects aren't compiled here either.
    ("src/REL/mdseldll/mdsel.c", ["-D_prolog=mdselDll_prolog", "-D_epilog=mdselDll_epilog",
                                    "-D_ctors=mdselDll_ctors", "-D_dtors=mdselDll_dtors",
                                    "-DObjectSetup=mdselDll_ObjectSetup",
                                    # Same duplicate-symbol story as _prolog above, one layer
                                    # deeper: every REL was its own link unit on hardware, so
                                    # the address-derived fn_1_XXXX/lbl_1_* names freely repeat
                                    # across RELs. mdsel.c's set intersects fileseldll's
                                    # (rel_filesel.o) in exactly these 5 (measured from the two
                                    # objects' COFF symbol tables, not guessed; all other
                                    # cross-.o dupes are linker-deduped .refptr COMDATs). All 5
                                    # are REL-internal -- nothing outside mdsel.c links against
                                    # them -- so a TU-local rename reproduces the original
                                    # per-module namespace exactly. The newcomer renames;
                                    # fileseldll's objects stay untouched.
                                    "-Dfn_1_3CC0=mdsel_fn_1_3CC0",
                                    "-Dfn_1_6E54=mdsel_fn_1_6E54",
                                    "-Dfn_1_C8C=mdsel_fn_1_C8C",
                                    "-Dlbl_1_data_0=mdsel_lbl_1_data_0",
                                    "-Dlbl_1_data_154=mdsel_lbl_1_data_154"]),
    # mdpartydll (party-mode setup --
    # mode select's own "Party Mode" confirm proceeds to this overlay,
    # ovl_table.h line 92 = index 91). The decomp's configure.py marks all
    # 3 mdpartydll objects (mdparty.c, stage.c, runtime.c) Matching and the
    # built REL is byte-identical to the disc original (461380 bytes).
    # src/REL/mdpartydll/runtime.c is deliberately NOT listed here -- same
    # reasoning as mdseldll's runtime.c above (a 1-line #include of the
    # MWCC/PPC REL runtime, replaced by the native toolchain's own runtime).
    # fn_1_*/lbl_1_* collision -D renames: measured the same way as
    # mdsel.c's own -- by attempting the link with none, reading the
    # linker's duplicate-symbol errors against every earlier fn_1_-namespace
    # REL object (mdsel/filesel), and renaming only the newcomer side. All
    # 6 are REL-internal (defined and referenced only inside mdparty.c
    # itself); mdsel.c's and filesel.c's own objects stay untouched.
    ("src/REL/mdpartydll/mdparty.c", ["-D_prolog=mdpartyDll_prolog", "-D_epilog=mdpartyDll_epilog",
                                        "-D_ctors=mdpartyDll_ctors", "-D_dtors=mdpartyDll_dtors",
                                        "-DObjectSetup=mdpartyDll_ObjectSetup",
                                        "-Dfn_1_0=mdparty_fn_1_0",
                                        "-Dfn_1_1B4=mdparty_fn_1_1B4",
                                        "-Dlbl_1_data_4=mdparty_lbl_1_data_4",
                                        "-Dfn_1_36C4=mdparty_fn_1_36C4",
                                        "-Dlbl_1_data_1C=mdparty_lbl_1_data_1C",
                                        "-Dlbl_1_data_0=mdparty_lbl_1_data_0"]),
    ("src/REL/mdpartydll/stage.c", []),
]

MAIN_C_FLAGS = ["-Dmain=GameMain"]

# Compiled with COMMON_FLAGS (decomp -I's + -include dolphin_compat.h), same
# as every game/REL source, in BOTH build modes.
# Savestate carve-out (shim/include/mp6_host_section.h).
# These TUs' file-scope statics are owned by something other than the game
# thread -- the SDL audio callback thread's mixer, the content-import worker
# threads, the SDL/Dawn handle holders -- or, in savestate.c's own case, must
# refer to the RUNNING process rather than the captured one. Force-including
# the pragma header redirects their statics into a dedicated PE section that
# savestate.c then excludes from both capture and restore BY NAME, so the
# carve-out cannot rot when someone adds a new static to one of these files.
#
# Deliberately NOT listed: platform/os/process_native.c and
# platform/host/coro_arena.c. Both are port code, but both hold load-bearing
# GAME state (the HuPrc scheduler table, and the coroutine wrappers it points
# at) that a savestate must restore -- the split here is host-owned vs
# game-owned, never port vs decomp.
HOST_STATE_SECTION_SOURCES = {
    "platform/os/savestate.c",
    "platform/audio/msm_bridge.c",
    "platform/audio/audio_out_sdl.c",
    "platform/gx/aurora_bridge.c",
    "platform/content/content_import.cpp",
    # The DVD layer's whole statics set is host-owned
    # -- the FST blob and its string pool are CRT-heap pointers, g_entryPaths
    # is a heap array of heap strings, and g_resolvedFilesRoot/_FstPath are
    # THIS machine's content paths (restoring those would override the running
    # launcher's content root with the capturing run's). The
    # g_fstLoadAttempted/g_fstOk latch is what makes this unrecoverable
    # without the carve-out: restored as "already loaded", it permanently
    # blocks fst_load_once() from re-deriving the blob, so the dangling
    # pointer can never self-heal. The one thing that stops restoring is
    # g_openReal[]'s open-file identity -- which is empty at every capture
    # point in today's build, asserted at capture time in savestate.c.
    "platform/dvd/dvd_files.c",
    # A GetProcAddress result from winmm.dll plus path caches. No game
    # state (audited: timer-resolution fn ptr/flag and resolved host paths).
    "platform/host/host_win32.c",
    # Pure host UI state -- RmlUi document stacks, toasts, picked-path
    # std::string heap pointers, connected-gamepad lists. content_setup.cpp
    # was a plain omission: its near-identically-named sibling
    # content_import.cpp was already listed.
    "platform/gx/ui/ui.cpp",
    "platform/gx/ui/content_setup.cpp",
    "platform/gx/ui/launcher_core.cpp",
    # Review finding (savestate-x1): the list originally covered only 4 of the
    # ~22 aurora-only C++ TUs. The rest hold heap-backed namespace-scope
    # std::strings (R"RML" document sources), per-TU log objects, and mutable
    # UI state -- all host-owned, none of it game state. A restore that
    # reinstalls a capturing process's std::string data pointers into these
    # TUs' statics recreates the exact static-destructor crash class the
    # aurora-archive carve-out fixed, via the port's own files instead.
    # framescope.c additionally holds env-latched arming state and
    # dumped-pointer dedupe tables that must describe the RUNNING process.
    "platform/gx/framescope.c",
    "platform/gx/ui/event.cpp",
    "platform/gx/ui/component.cpp",
    "platform/gx/ui/document.cpp",
    "platform/gx/ui/button.cpp",
    "platform/gx/ui/select_button.cpp",
    "platform/gx/ui/bool_button.cpp",
    "platform/gx/ui/number_button.cpp",
    "platform/gx/ui/string_button.cpp",
    "platform/gx/ui/pane.cpp",
    "platform/gx/ui/tab_bar.cpp",
    "platform/gx/ui/window.cpp",
    "platform/gx/ui/modal.cpp",
    "platform/gx/ui/input.cpp",
    "platform/gx/ui/overlay.cpp",
    "platform/gx/ui/menu_bar.cpp",
    "platform/gx/ui/graphics_tuner.cpp",
    "platform/gx/ui/prelaunch.cpp",
    "platform/gx/ui/settings.cpp",
    # Freecam (shim/include/mp6_freecam.h): the enable flag + fly pose belong
    # to the RUNNING process's UI session -- a restored state must neither
    # re-enable freecam nor teleport the camera the user is flying.
    "platform/hsf/mp6_freecam.c",
    # Unlocked FPS MODEL interpolation (shim/include/mp6_fi_model.h): the
    # per-tick transform/camera snapshot buffers are host-owned RENDER state
    # of the RUNNING process, not captured game state -- a restore must not
    # reinstate a pre-restore snapshot (it would interpolate across the state
    # discontinuity). Same carve-out shape as frame_interp.c right below.
    # Compiled in BOTH modes (COMMON_FLAGS), so the section pragma is asserted
    # against both builds; harmless in headless (no interpolation there).
    "platform/hsf/mp6_fi_model.c",
    # MP6_SHADOW_DUMP (shim/include/mp6_shadow_dump.h): same shape as
    # framescope.c right above -- an env-latched arming flag and a dump
    # counter that describe the RUNNING process's debug session, not game
    # state.
    "platform/gx/shadow_dump.c",
    # Unlocked FPS presentation layer (shim/include/mp6_unlocked_fps.h): the
    # idle-window pacing statics are monotonic timestamps taken from the RUNNING
    # process's timer plus diagnostic counters -- host state, not captured game
    # state. Restoring a capturing process's timestamps into the loading one
    # would hand the first post-restore window a nonsense alpha and spacing.
    # (Historically this carve-out also protected the retained-stream path's
    # realloc'd buffers, whose restored pointer VALUES corrupted the heap; that
    # path is deleted, the carve-out stays for the reason above.) Aurora-only
    # TU (PLATFORM_AURORA_ONLY); the headless build never compiles it, so this
    # entry is asserted only against the windowed build.
    "platform/gx/frame_interp.c",
}


def verify_host_section_sources():
    """Fail the build if a carve-out TU lost its section include.

    Why an in-TU include instead of a per-TU -include flag: a force-include
    lands BEFORE the TU's own headers, so the pragma also captured the decomp
    headers' C tentative definitions (dolphin/os.h's `u32 __OSBusClock;` and
    friends) -- common symbols the linker merges; giving them a section turns
    each into a strong definition and the link fails with duplicate symbols.

    The carve-out is a safety property, not an optimization: a TU that
    silently stops being carved out gets its SDL/Dawn handles, live
    std::thread objects, or foreign heap pointers restored over a running
    process. That failure shows up as heap corruption far from the cause, so
    it must be caught at build time, not by a mystery crash later."""
    missing = []
    for rel in sorted(HOST_STATE_SECTION_SOURCES):
        p = os.path.join(NATIVE_ROOT, rel.replace("/", os.sep))
        if not os.path.exists(p):
            missing.append(f"{rel} (file not found)")
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        # The include must exist AND sit at preprocessor top level. A raw
        # substring test is NOT enough: this assert once passed while the
        # include sat inside the #else of an #ifdef _WIN32 block in
        # aurora_bridge.c (and inside #ifdef __ANDROID__ in content_setup.cpp),
        # so on Windows -- the only savestate platform -- those TUs silently
        # compiled UNCARVED and a cross-session restore clobbered their live
        # SDL/throttle state. Track #if/#ifdef/#ifndef nesting depth and
        # require the include at depth 0, where no platform condition can
        # preprocess it away.
        found_at_top = False
        found_nested = False
        depth = 0
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("#if"):  # #if, #ifdef, #ifndef
                depth += 1
            elif s.startswith("#endif"):
                depth = max(0, depth - 1)
            elif '#include "mp6_host_section.h"' in s and not s.startswith("//"):
                if depth == 0:
                    found_at_top = True
                else:
                    found_nested = True
        if not found_at_top:
            if found_nested:
                missing.append(f"{rel} (include is NESTED inside a preprocessor conditional -- "
                               f"it must sit at top level or a platform branch silently uncarves the TU)")
            else:
                missing.append(f"{rel} (no #include \"mp6_host_section.h\")")
    if missing:
        print("\n[BUILD] FATAL: savestate carve-out is incomplete. Each TU listed in")
        print("        HOST_STATE_SECTION_SOURCES holds file-scope statics owned by a")
        print("        non-game thread or an OS/driver handle -- not the deterministic")
        print("        game simulation a savestate captures. Without the section include,")
        print("        a restore overwrites this TU's live SDL/thread/heap state with the")
        print("        CAPTURING process's own: corruption, not a clean load. Affected:")
        for m in missing:
            print(f"          - {m}")
        print("        Fix: add `#include \"mp6_host_section.h\"` AFTER that file's own")
        print("        includes, at preprocessor top level (never inside an #if/#ifdef")
        print("        branch, or a platform build silently skips the carve-out) -- or,")
        print("        if this file genuinely holds no host-owned statics, remove it from")
        print("        HOST_STATE_SECTION_SOURCES instead.")
        raise SystemExit(1)


PLATFORM_SOURCES_COMMON = [
    "platform/os/arena.c",
    "platform/os/process_native.c",  # replaces jmp_native.c + hostjmp.c +
                                       # prc_trace.c (game/jmp.c AND game/process.c
                                       # skipped together, see GAME_SKIP_LIST) --
                                       # gcsetjmp/gclongjmp(&processjmpbuf) had no
                                       # working native equivalent; see its own
                                       # header comment for the full investigation.
    "platform/os/malloc_direct.c",
    "platform/os/savestate.c",  # cross-session savestate capture/restore
                                  # In BOTH build modes on
                                  # purpose: the headless build is where the
                                  # capture/restore regression gate runs, since
                                  # it is the mode with a byte-identical log.
    "platform/os/dll_bridge.c",
    "platform/os/board_stub.c",  # Honest no-op placeholder for src/board/board.c's
                                   # mbSaveInit/mbSavePartyInit -- board.c itself is a separate,
                                   # not-yet-integrated recovery lane; see the file's own header.
    "platform/os/log.c",
    "platform/os/card_native.c",  # memory-card slot A: CARDInit(void)->aurora
                                    # CARDInit(game,maker) interposer + saves/
                                    # base path; honest no-op under
                                    # MP6_HEADLESS_BUILD (see its header)
    "platform/host/host_win32.c",  # the win32 host backend -- the OS seam
                                     # (time/memory/paths/coro/mutex/thread/
                                     # rss/crash). Links in BOTH modes:
                                     # kernel32/dbghelp/psapi are already on
                                     # both link lines.
    "platform/host/coro_arena.c",  # the DEFAULT coro backend -- minicoro
                                     # (MCO_USE_ASM) over a low-4GB arena stack
                                     # pool. Self-guarded #ifndef MP6_CORO_FIBERS,
                                     # so it compiles to nothing (and host_win32.c's
                                     # fiber backend takes over) under the
                                     # --coro-fibers A/B lever. Links in BOTH
                                     # modes, like process_native.c.
    "platform/os/save_endian.c",  # field-wise BE marshal for the persisted
                                    # GW_COMMON/GW_SYSTEM/GW_PLAYER save-box
                                    # structs -- called only from the patched
                                    # game/saveload.c struct<->saveBuf boundaries
                                    # (see shim/include/mp6_save_endian.h)
    "platform/dvd/dvd_files.c",  # real FST + host-file serving, see dll_bridge.c's own header
    "platform/hsf/hsf_load_native.c",  # real HSF (3D scene) deserializer
    "platform/hsf/mp6_freecam.c",  # freecam camera override (shim/include/mp6_freecam.h):
                                     # needs game/hu3d.h (COMMON_FLAGS' decomp -I's) like its
                                     # neighbors; BOTH modes because its one caller is the
                                     # shared Hu3DExec patch hook -- a permanently-false
                                     # branch in headless (nothing there can enable it).
    "platform/hsf/mp6_fi_model.c",  # Unlocked FPS MODEL-level interpolation (shim/include/
                                      # mp6_fi_model.h): snapshot Hu3DData[]/Hu3DCamera[] per tick,
                                      # re-run Hu3DExec on the interpolated pose in the idle window.
                                      # Needs game/hu3d.h (COMMON_FLAGS' decomp -I's) like mp6_freecam.c;
                                      # BOTH modes so the replay-pass flag it defines resolves for the
                                      # shared hsfman.c guard patch -- the replay caller (frame_interp.c)
                                      # is aurora-only, so the re-run path is dead code in headless.
    "platform/hsf/mp6_widescreen_extrude.c",  # The shared backdrop-extrude helpers:
                                                 # shared backdrop-extrude helper, hoisted out of
                                                 # mdpartydll/mdparty.c's own file-local static so every
                                                 # widescreen-extruded REL calls one definition. Needs
                                                 # game/hu3d.h (COMMON_FLAGS' decomp -I's), same as
                                                 # hsf_load_native.c above; links into BOTH modes since
                                                 # its REL callers are shared between them.
    "platform/hsf/mp6_shadow_quality.c",  # Mods-page Shadow Quality (shim/include/
                                             # mp6_shadow_quality.h): mp6_shadow_quality_scale(), the
                                             # Hu3DShadowMultiCreate/Hu3DShadowMultiSizeSet origin-site
                                             # helper. Needs game/memory.h (HEAPID/HuMemMaxMemorySizeGet),
                                             # same COMMON_FLAGS access as its platform/hsf/ neighbors;
                                             # links into BOTH modes (its one caller is the shared
                                             # hsfman.c.patch origin site), internally #ifdef
                                             # MP6_HEADLESS_BUILD like shims_manual.c's widescreen stubs.
    "platform/gx/gxarray_registry.c",  # GXSetArray real-size registry.
                                         # Dependency-free (no decomp/Aurora headers), so either flavor's
                                         # flags compile it fine; lives in COMMON so it's linked into BOTH
                                         # modes -- hsf_load_native.c (both modes) is the writer,
                                         # aurora_bridge.c (aurora only) is the one real reader.
    "platform/gx/shadow_dump.c",  # MP6_SHADOW_DUMP debug lever (shim/include/
                                     # mp6_shadow_dump.h): GPU->CPU readback + PNG dump of the
                                     # resolved Hu3DShadow copy texture (aurora-patches/0016).
                                     # Needs game/hu3d.h (Hu3DShadow), same COMMON_FLAGS access as
                                     # its platform/gx/framescope.c and platform/hsf/ neighbors;
                                     # links into BOTH modes, internally #ifdef MP6_HEADLESS_BUILD
                                     # (headless has no renderer/GPU -- standing no-op there).
    # platform/null/shims_generated{,_aurora}.c is NOT in this list -- see
    # collect_units() below, it's the one PLATFORM_SOURCES_COMMON entry
    # whose SOURCE FILE (not just its compile flags) differs per mode. See
    # tools/gen_shims.py's OUT_FILE_AURORA comment for why a real content
    # difference (not weak symbols, not #ifdef) is what this split needs.
    "platform/null/shims_manual.c",  # VIInit/VIWaitForRetrace/VIGetRetraceCount/
                                       # VIGetNextField/GXInit inside are now
                                       # `#ifdef MP6_HEADLESS_BUILD`-guarded; harmless
                                       # to always compile this file in both modes.
    # Real streamed-music playback -- see platform/audio/msm_bridge.c's own
    # header comment for the full design. All three compile with COMMON_FLAGS
    # in BOTH build modes
    # (dspadpcm.c/wav_writer.c are plain dependency-free C that happens to
    # tolerate the decomp -I's/-include harmlessly; msm_bridge.c genuinely
    # needs them for the real dolphin.h/msm.h types its taken-over msm*/AI*
    # symbols must match exactly). The live-playback SDL3 backend
    # (platform/audio/audio_out_sdl.c) is Aurora-only -- see
    # PLATFORM_AURORA_ONLY below, same split as aurora_bridge.c's own.
    "platform/audio/dspadpcm.c",
    "platform/audio/wav_writer.c",
    "platform/audio/msm_bridge.c",
]

# Compiled with AURORA_FLAGS instead of
# COMMON_FLAGS (Aurora's own -I's, no decomp -I/-include at all -- see
# AURORA_FLAGS's own comment for why), and ONLY for the non-headless build.
# platform/main_native.c is the one file compiled in BOTH modes but with
# DIFFERENT flags per mode (its own #ifdef MP6_HEADLESS_BUILD chooses which
# half of the file is even reachable) -- see PLATFORM_AURORA_ONLY below for
# the file that exists in the aurora build alone.
MAIN_NATIVE = "platform/main_native.c"
PLATFORM_AURORA_ONLY = [
    "platform/gx/aurora_bridge.c", "platform/gx/framescope.c",
    "platform/gx/frame_interp.c",  # Unlocked FPS presentation layer
                                     # (shim/include/mp6_unlocked_fps.h): the frame-boundary
                                     # hooks + the idle-window pacing that drives the
                                     # model-level replay (platform/hsf/mp6_fi_model.c).
                                     # Aurora headers only (AURORA_FLAGS), same split as
                                     # aurora_bridge.c -- headless untouched by construction.
    "platform/gx/freecam_input.c",  # freecam host-input collector (SDL keyboard/
                                      # mouse/touch/gamepad -> per-tick camera deltas;
                                      # shim/include/mp6_freecam.h's windowed-only half)
    # Real-time SDL3 playback for the msm stream mixer -- Aurora-only (needs
    # Aurora's own AURORA_FLAGS for SDL3's real headers/include path;
    # --headless has no live device to feed at all, see
    # platform/audio/msm_bridge.c's msmSysRegularProc for its own
    # from-the-tick-pump verification path instead).
    "platform/audio/audio_out_sdl.c",
    # The pre-boot launcher menu: partyboard's own RmlUi launcher
    # implementation, ripped at explicit user direction
    # (docs/PARTYBOARD_PROVENANCE.md) into platform/gx/ui/ and adapted to
    # our settings content. launcher_core.cpp is OURS (the config model /
    # mode decision / HSF wordmark decode behind the same six-function C
    # seam main_native.c always consumed); the rest are the ripped
    # framework files. All C++
    # (compile_one() swaps the -std flag and adds the RmlUi include/define
    # set for .cpp sources); aurora-only by construction, and excluded from
    # every android unit set (collect_units skips .cpp entries on android
    # rows).
    "platform/gx/ui/launcher_core.cpp",
    "platform/gx/ui/ui.cpp",
    "platform/gx/ui/event.cpp",
    "platform/gx/ui/component.cpp",
    "platform/gx/ui/document.cpp",
    "platform/gx/ui/button.cpp",
    "platform/gx/ui/select_button.cpp",
    "platform/gx/ui/bool_button.cpp",
    "platform/gx/ui/number_button.cpp",
    "platform/gx/ui/string_button.cpp",
    "platform/gx/ui/pane.cpp",
    "platform/gx/ui/tab_bar.cpp",
    "platform/gx/ui/window.cpp",
    "platform/gx/ui/modal.cpp",
    "platform/gx/ui/input.cpp",
    "platform/gx/ui/overlay.cpp",
    "platform/gx/ui/menu_bar.cpp",
    "platform/gx/ui/graphics_tuner.cpp",
    "platform/gx/ui/prelaunch.cpp",
    "platform/gx/ui/settings.cpp",
    # first-run content onboarding -- the
    # ContentSetup dialog (ripped-framework idiom, ours) + the nod-backed
    # import engine it drives. Aurora-flavor .cpp like the rest of ui/;
    # compiled on BOTH the Windows windowed row and the android --windowed
    # row (collect_units no longer skips .cpp on android -- an earlier
    # "windows-windowed only" rule this lane retires).
    "platform/gx/ui/content_setup.cpp",
    "platform/content/content_import.cpp",
]


def mode_suffix_for_stamp(args):
    """Distinct stamp TU per build mode so headless/windowed (and the fiber
    A/B lever) never share a cached stamp object."""
    return ("_headless" if args.headless else "_aurora") + ("_corofib" if getattr(args, "coro_fibers", False) else "")


def _ar_has_section(archive_path, section_name, max_objects=40):
    """True if any of the first `max_objects` COFF members of a .a archive
    contains a section named `section_name` (8 bytes max, e.g. .mp6hbss)."""
    want = section_name.encode()[:8].ljust(8, b"\0")
    try:
        with open(archive_path, "rb") as f:
            if f.read(8) != b"!<arch>\n":
                return False
            checked = 0
            while checked < max_objects:
                hdr = f.read(60)
                if len(hdr) < 60:
                    return False
                try:
                    size = int(hdr[48:58].decode().strip())
                except ValueError:
                    return False
                body_start = f.tell()
                name = hdr[0:16].decode(errors="replace").strip()
                if not name.startswith("/") and size > 20:  # skip symbol/string tables
                    data = f.read(min(size, 1 << 20))
                    if len(data) >= 20:
                        nsec = int.from_bytes(data[2:4], "little")
                        optsz = int.from_bytes(data[16:18], "little")
                        off = 20 + optsz
                        for i in range(min(nsec, 96)):
                            e = off + i * 40
                            if e + 8 <= len(data) and data[e:e + 8] == want:
                                return True
                    checked += 1
                f.seek(body_start + size + (size & 1))
    except OSError:
        return False
    return False


def verify_aurora_carveout():
    """Fail the WINDOWED build if the aurora tree was rebuilt without the
    savestate carve-out flags (a review finding).

    The port TUs' carve-out is asserted by verify_host_section_sources(),
    but the aurora/RmlUi/sqlite archives come from an out-of-band CMake tree
    whose required flags (-include mp6_host_section.h on C AND CXX, PCH off
    -- see the recipe comment above AURORA_BUILD_RMLUI) existed only as
    documentation. A tree regenerated without them links cleanly and every
    other gate passes (headless never links aurora), while a windowed
    restore reinstalls the capturing process's SDL/WebGPU/sqlite handles
    over a live render loop. So check the actual invariant at its source:
    the archives' own objects must carry the carve-out sections.
      - libaurora_gx.a  proves the CXX flag reached aurora's own TUs
      - librmlui.a      proves PCH is off (PCH-compiled TUs drop the pragma)
      - libsqlite3.a    proves the C flag reached the C dependencies
    """
    checks = [
        (os.path.join(AURORA_BUILD_RMLUI, "libaurora_gx.a"), ".mp6hbss",
         "aurora's own C++ TUs (CMAKE_CXX_FLAGS -include missing?)"),
        (os.path.join(AURORA_BUILD_RMLUI, "librmlui.a"), ".mp6hbss",
         "RmlUi's TUs (CMAKE_DISABLE_PRECOMPILE_HEADERS=ON missing? PCH drops the pragma)"),
        (os.path.join(AURORA_BUILD_RMLUI, "extern", "libsqlite3.a"), ".mp6hdat",
         "the C dependencies (CMAKE_C_FLAGS -include missing?)"),
    ]
    bad = []
    for path, section, hint in checks:
        if not os.path.exists(path):
            bad.append(f"{os.path.basename(path)}: archive not found at {path}")
        elif not _ar_has_section(path, section):
            bad.append(f"{os.path.basename(path)}: no {section} section in its objects -- {hint}")
    if bad:
        print("\n[BUILD] FATAL: the aurora build-rmlui tree was built WITHOUT the savestate")
        print("        carve-out. A windowed savestate restore against this link would")
        print("        reinstall a dead process's SDL/WebGPU/sqlite handles over a live one.")
        for b in bad:
            print(f"          - {b}")
        print("        Reconfigure the tree with the flags in the AURORA_BUILD_RMLUI recipe")
        print("        comment (tools/build.py) and rebuild the aurora targets.")
        raise SystemExit(1)


def collect_units(headless, coro_fibers=False, android=False):
    """Returns a list of (abs_src_path, extra_flags, obj_name, flavor) --
    `flavor` picks the BASE flag set in compile_one(): "common" ->
    COMMON_FLAGS (decomp -I's, -include dolphin_compat.h -- game/REL
    sources and most of platform/), "aurora" -> AURORA_FLAGS (Aurora's own
    -I's only -- platform/main_native.c in the default build, and
    platform/gx/aurora_bridge.c, see that file's own header comment).

    android=True is the headless TU set with exactly ONE swap --
    platform/host/host_win32.c -> platform/host/host_android.c (the host-seam
    seam's second backend; everything else in the set is portable C, the
    the design's own 'already-portable' census + coro_arena.c). Callers pass
    headless=True with it (the android row is headless-only on Android)."""
    verify_host_section_sources()  # savestate carve-out completeness
    units = []
    for rel, flags in game_sources():
        abs_path = resolve_source(rel)
        f = list(flags)
        if rel.replace("\\", "/") == "src/game/main.c":
            f = f + MAIN_C_FLAGS
        obj_name = "game_" + os.path.basename(abs_path).replace(".c", ".o")
        units.append((abs_path, f, obj_name, "common"))
    for rel, flags in REL_SOURCES:
        abs_path = resolve_source(rel)
        obj_name = "rel_" + os.path.basename(abs_path).replace(".c", ".o")
        units.append((abs_path, flags, obj_name, "common"))

    # Platform sources: mode-suffixed obj names (NOT shared with the other
    # mode's build) since their CONTENT depends on which flags/macros this
    # invocation passes -- unlike game/REL sources, which compile to
    # byte-identical objects in both modes (TARGET_PC, the patched headers,
    # and dolphin_compat.h are all unconditional now) and so keep sharing
    # plain "plat_"-prefixed names safely. Using distinct names for the
    # mode-dependent files is what lets needs_rebuild()'s plain mtime check
    # stay correct even when switching between `--headless` and the default
    # build back and forth without a --clean in between.
    # The coro backend is a compile-time choice. Default = the arena-backed
    # minicoro backend (platform/host/coro_arena.c); --coro-fibers = the
    # Win32 fiber backend (host_win32.c), an A/B lever. The define reaches
    # every PLATFORM_SOURCES_COMMON/shims TU via headless_flags (only host_win32.c
    # and coro_arena.c actually read it; the rest ignore it harmlessly), and
    # the "_corofib" obj/exe suffix keeps the two backends' objects from
    # ever aliasing when switching without a --clean. The DEFAULT build's
    # object names are unchanged (coro_suffix=""), so it is the minicoro
    # backend that ships.
    coro_suffix = "_corofib" if coro_fibers else ""
    coro_flags = ["-DMP6_CORO_FIBERS"] if coro_fibers else []
    mode_suffix = ("_headless" if headless else "_aurora") + coro_suffix
    headless_flags = (["-DMP6_HEADLESS_BUILD"] if headless else []) + coro_flags
    for rel in PLATFORM_SOURCES_COMMON:
        if android and rel == "platform/host/host_win32.c":
            rel = "platform/host/host_android.c"  # the one backend swap (see docstring)
        abs_path = os.path.join(NATIVE_ROOT, rel.replace("/", os.sep))
        base = os.path.basename(abs_path).replace(".c", "")
        units.append((abs_path, headless_flags, f"plat_{base}{mode_suffix}.o", "common"))

    # shims_generated.c (--headless) vs shims_generated_aurora.c (default) --
    # see tools/gen_shims.py's OUT_FILE_AURORA comment: a real source-file
    # difference, not just a flag, because the aurora variant must not even
    # DEFINE the ~100+ symbols Aurora's own linked libraries provide.
    shims_rel = "platform/null/shims_generated.c" if headless else "platform/null/shims_generated_aurora.c"
    shims_abs = os.path.join(NATIVE_ROOT, shims_rel.replace("/", os.sep))
    units.append((shims_abs, headless_flags, f"plat_shims_generated{mode_suffix}.o", "common"))

    main_abs = os.path.join(NATIVE_ROOT, MAIN_NATIVE.replace("/", os.sep))
    if headless:
        units.append((main_abs, headless_flags, f"plat_main_native{mode_suffix}.o", "common"))
    else:
        units.append((main_abs, [], f"plat_main_native{mode_suffix}.o", "aurora"))

    if not headless:
        for rel in PLATFORM_AURORA_ONLY:
            # the .cpp launcher/onboarding TUs
            # now compile on the android --windowed row too (an earlier
            # "windows-windowed only" exclusion is retired) -- as the
            # "aurora_cpp_rmlui" flavor: NDK clang++ with the same RmlUi
            # include/define set the Windows .cpp branch adds, RTTI ON
            # (ui.cpp's dynamic_cast needs it -- unlike the touch-overlay
            # TU's exception/RTTI-free "aurora_cpp" flavor, which stays
            # byte-untouched).
            flavor = "aurora"
            if rel.endswith(".cpp") and android:
                flavor = "aurora_cpp_rmlui"
            # The import engine reads nod's C FFI header; nod include
            # dir is per-unit so no other TU sees it (tools/fetch_nod.py
            # populates port/toolchain/nod).
            flags = ["-I", NOD_INCLUDE] if rel.endswith("content_import.cpp") else []
            abs_path = os.path.join(NATIVE_ROOT, rel.replace("/", os.sep))
            # splitext, not .replace(".c",""), so a .cpp entry gets a clean
            # obj name too; identical result for every .c entry.
            base = os.path.splitext(os.path.basename(abs_path))[0]
            units.append((abs_path, flags, f"plat_{base}{mode_suffix}.o", flavor))

    return units


def _newest_mtime_under(dirpath):
    newest = 0.0
    for root, _dirs, files in os.walk(dirpath):
        for f in files:
            try:
                newest = max(newest, os.path.getmtime(os.path.join(root, f)))
            except OSError:
                pass
    return newest


def needs_rebuild(src, obj):
    if not os.path.exists(obj):
        return True
    obj_mtime = os.path.getmtime(obj)
    self_mtime = os.path.getmtime(os.path.abspath(__file__))
    # No real dependency graph -- approximate "did any shared
    # compat header change" by checking the whole shim/include/ + patched
    # header tree's newest mtime too, not just this one source file's.
    newest_shared = max(_newest_mtime_under(SHIM_INCLUDE), _newest_mtime_under(PATCHED_INCLUDE),
                         _newest_mtime_under(MSL_OVERRIDE))
    return (os.path.getmtime(src) > obj_mtime
            or self_mtime > obj_mtime
            or newest_shared > obj_mtime)


def _write_if_changed(path, content):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            if f.read() == content:
                return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


# Set (only) by build_android() before compilation -- {"clang": <NDK
# clang path>, "flags": android_common_flags()}. None = the Windows flow,
# untouched. compile_one/needs_rebuild read module globals, so the android
# build redirects OBJ_DIR/MSL_OVERRIDE/PATCHED_INCLUDE the same way (see
# build_android's own comment); a single build.py invocation only ever
# builds one target.
_ANDROID_CC = None


def compile_one(unit):
    src, flags, obj_name, flavor = unit
    obj = os.path.join(OBJ_DIR, obj_name)
    if not needs_rebuild(src, obj):
        return (src, obj, True, "", True)  # skipped=True
    if _ANDROID_CC is not None:
        # android row. Headless: every unit is "common" flavor by
        # construction. The android --windowed row adds the aurora-flavor TUs
        # (aurora_bridge/framescope/audio_out_sdl/main_native), compiled
        # against android_aurora_flags() exactly like the Windows split.
        # "aurora_cpp": the touch-overlay
        # TU (platform/android/touch_pad.cpp) draws through aurora's ImGui
        # pass, whose API is C++ -- same aurora include set with the C
        # -std swapped for aurora's own C++20 (its CMAKE_CXX_STANDARD),
        # compiled by NDK clang++ so the C++ driver defaults apply.
        # Exceptions/RTTI off: nothing here uses them and the game image's
        # other TUs are all plain C. This flavor exists only on the
        # android --windowed row; the Windows flow never constructs it.
        if flavor == "aurora_cpp":
            cxx = _ANDROID_CC["clang"].replace("clang.exe", "clang++.exe")
            cxxflags = ["-std=c++20" if f == "-std=gnu11" else f
                        for f in _ANDROID_CC["aurora_flags"]]
            cmd = [cxx] + cxxflags + ["-fno-exceptions", "-fno-rtti"] + flags + ["-c", src, "-o", obj]
        elif flavor == "aurora_cpp_rmlui":
            # the launcher/onboarding .cpp TUs
            # on android -- NDK clang++ with the exact RmlUi include/define
            # set the Windows .cpp branch (below) adds, resolved against
            # the RmlUi-enabled android tree. Differences from the Windows
            # list, both principled: no zlib-build -I (bionic's NDK sysroot
            # provides the real <zlib.h> -- the same -lz the link uses) and
            # no -fno-exceptions/-fno-rtti (ui.cpp's dynamic_cast needs
            # RTTI; aurora's own android C++ compiles with both on).
            cxx = _ANDROID_CC["clang"].replace("clang.exe", "clang++.exe")
            cxxflags = ["-std=c++20" if f == "-std=gnu11" else f
                        for f in _ANDROID_CC["aurora_flags"]]
            cxxflags = cxxflags + [f'-DMP6_PORT_VERSION="{mp6_port_version()}"',
                                   "-DAURORA_ENABLE_RMLUI",
                                   "-DRMLUI_STATIC_LIB",
                                   "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "rmlui-src", "Include"),
                                   "-I", os.path.join(AURORA_DEPS_ANDROID_RMLUI, "abseil-cpp-src"),
                                   "-I", AURORA_ROOT]
            cmd = [cxx] + cxxflags + flags + ["-c", src, "-o", obj]
        else:
            base_flags = _ANDROID_CC["aurora_flags"] if flavor == "aurora" else _ANDROID_CC["flags"]
            cmd = [_ANDROID_CC["clang"]] + base_flags + flags + ["-c", src, "-o", obj]
    else:
        base_flags = AURORA_FLAGS if flavor == "aurora" else COMMON_FLAGS
        zig_lang = "cc"
        if src.endswith(".cpp"):
            # Every .cpp TU is part of the ripped RmlUi launcher UI
            # (platform/gx/ui/). Same AURORA_FLAGS
            # base with the C -std swapped for C++20 (the ripped framework
            # and aurora's RmlUi glue both use ranges/concepts/span),
            # compiled via `zig c++` (plain `zig cc` does not add the
            # libc++ include paths). The final link is already `zig c++`.
            base_flags = [f for f in base_flags if f != "-std=gnu11"] + ["-std=gnu++20"]
            # The launcher decodes the game's own title wordmark at runtime
            # (title.bin entries are zlib-compressed) -- zlib-ng's compat
            # header from the RMLUI dep tree (same pinned fetch as
            # aurora/build's). These additions are scoped to the .cpp
            # branch so AURORA_FLAGS stays byte-identical for the C TUs:
            #   - -DAURORA_ENABLE_RMLUI + -DRMLUI_STATIC_LIB mirror the
            #     DEFINES line of build-rmlui/build.ninja's own aurora_core
            #     compile rule (the proven-line-mirroring discipline this
            #     file already uses; RMLUI_STATIC_LIB is load-bearing on
            #     MinGW -- without it RmlUi headers declare dllimport);
            #   - rmlui-src Include/ for <RmlUi/Core.h>;
            #   - abseil-cpp-src for ui.cpp's absl::flat_hash_set;
            #   - AURORA_ROOT itself for the ripped files' "lib/window.hpp"
            #     aurora-internal includes.
            base_flags = base_flags + ["-I", os.path.join(AURORA_DEPS_RMLUI, "zlib-build"),
                                       f'-DMP6_PORT_VERSION="{mp6_port_version()}"',
                                       "-DAURORA_ENABLE_RMLUI",
                                       "-DRMLUI_STATIC_LIB",
                                       "-I", os.path.join(AURORA_DEPS_RMLUI, "rmlui-src", "Include"),
                                       "-I", os.path.join(AURORA_DEPS_RMLUI, "abseil-cpp-src"),
                                       "-I", AURORA_ROOT]
            zig_lang = "c++"
        cmd = [ZIG, zig_lang] + base_flags + flags + ["-c", src, "-o", obj]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    ok = proc.returncode == 0 and os.path.exists(obj)
    return (src, obj, ok, proc.stdout + proc.stderr, False)  # actually invoked the compiler


def _resolve_aurora_link_items():
    """AURORA_LINK_ITEMS entries are either a raw `-lxxx` linker flag (passed
    through as-is) or a path relative to the aurora build tree (resolved to
    an absolute, forward-slash path -- see patch_headers()'s own header
    comment on the CMakeRCCompiler.cmake backslash-escaping bug for why
    forward slashes specifically matter to this toolchain).

    The WINDOWED link resolves against AURORA_BUILD_RMLUI -- the
    RmlUi-enabled tree (see its comment block up top). Same aurora pin,
    same third-party pins, same Debug config; the aurora_core/aurora_gx
    archives differ only by the AURORA_ENABLE_RMLUI module being compiled
    in. The canonical aurora/build tree stays byte-untouched; --headless
    still consumes it (zlib) unchanged."""
    out = []
    for item in AURORA_LINK_ITEMS:
        if item.startswith("-l"):
            out.append(item)
        else:
            out.append(os.path.normpath(os.path.join(AURORA_BUILD_RMLUI, item)).replace("\\", "/"))
    return out


def copy_aurora_runtime_dlls(dst_dir):
    """examples/CMakeLists.txt never calls aurora_copy_runtime_dlls() for
    the `simple` target either -- there being no CMake step to invoke here,
    this driver copies the same runtime DLLs itself, next to
    build/mp6native.exe."""
    import shutil
    for src in AURORA_RUNTIME_DLLS:
        if not os.path.exists(src):
            print(f"[WARN] copy_aurora_runtime_dlls: missing {src} -- aurora/build may not be built")
            continue
        dst = os.path.join(dst_dir, os.path.basename(src))
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue
        shutil.copy2(src, dst)
        print(f"  copied {os.path.basename(src)}")


def build_android(args):
    """the whole `--target
    aarch64-android` flow, kept as its own function (ending in its own
    link) so the Windows flow in main() is textually untouched. Redirects
    the generated-tree globals (OBJ_DIR/MSL_OVERRIDE/PATCHED_INCLUDE) into
    build/android/ -- the Windows build's own trees and incremental state
    are never read or written by an android build. build/patched-src
    (apply_patches) IS shared deliberately: its content is
    target-independent decomp source and its writer is
    _write_if_changed-idempotent, so sharing costs no mtime churn."""
    global OBJ_DIR, MSL_OVERRIDE, PATCHED_INCLUDE, _ANDROID_CC

    ndk_root = _find_ndk_root()
    clang = _ndk_clang(ndk_root) if ndk_root else None
    if not clang or not os.path.exists(clang):
        print("[FATAL] --target aarch64-android: NDK clang not found "
              f"({clang or 'no NDK under port/android-sdk/ndk/'}) -- set ANDROID_NDK_ROOT "
              "or install the NDK per port/android-probe/README.md")
        return 1
    print(f"Mode: --target aarch64-android (headless .so + launcher; NDK {os.path.basename(ndk_root)})")

    if args.coro_fibers:
        # The fiber backend is win32-only (host_win32.c); android always
        # runs the shared arena/minicoro backend -- same default as Windows.
        print("[FATAL] --coro-fibers is a win32-only A/B lever; the android row always uses "
              "the arena/minicoro backend")
        return 1

    OBJ_DIR = ANDROID_OBJ_DIR
    MSL_OVERRIDE = ANDROID_MSL_OVERRIDE
    PATCHED_INCLUDE = ANDROID_PATCHED_INCLUDE
    _ANDROID_CC = {"clang": clang, "flags": android_common_flags(),
                   "aurora_flags": android_aurora_flags()}

    # --windowed = the aurora/SDL3/Dawn
    # graphics build as libmp6game.so for the APK shell. Default (no flag)
    # stays byte-for-byte the headless row.
    windowed = getattr(args, "windowed", False)
    if windowed and not os.path.isdir(AURORA_BUILD_ANDROID_RMLUI):
        print(f"[FATAL] --windowed: {AURORA_BUILD_ANDROID_RMLUI} not configured/built -- see the "
              "AURORA_BUILD_ANDROID_RMLUI comment block in tools/build.py (or "
              "for the cmake recipe")
        return 1
    if windowed and not os.path.exists(NOD_ANDROID_LIB):
        print(f"[FATAL] --windowed: {NOD_ANDROID_LIB} missing (the disc-import backend) -- "
              f"{NOD_RECIPE_HINT}")
        return 1
    if windowed and not os.path.isdir(NOD_INCLUDE):
        print(f"[FATAL] --windowed: {NOD_INCLUDE} missing -- {NOD_RECIPE_HINT}")
        return 1

    os.makedirs(OBJ_DIR, exist_ok=True)
    if args.clean:
        import shutil
        shutil.rmtree(ANDROID_BUILD_DIR, ignore_errors=True)
        os.makedirs(OBJ_DIR, exist_ok=True)

    patch_headers(dst_root=ANDROID_PATCHED_INCLUDE)
    patch_msl_override(dst_dir=ANDROID_MSL_OVERRIDE, find_real=_find_ndk_libc_dir,
                       libc_desc="the NDK sysroot/clang-resource headers")
    patch_abi_struct_headers(dst_root=ANDROID_PATCHED_INCLUDE,
                             extra_patches=ANDROID_HEADER_CONTENT_PATCHES)
    apply_decomp_override_headers(dst_root=ANDROID_PATCHED_INCLUDE)  # shield (see docstring)
    apply_patches.apply_all()  # shared patched-src, see docstring

    units = collect_units(headless=not windowed, coro_fibers=False, android=True)
    if windowed:
        # the touch-overlay TU -- android
        # --windowed ONLY (appended here, not in collect_units, so neither
        # the Windows aurora build nor the headless rows ever see it). C++
        # because it draws through aurora's ImGui pass ("aurora_cpp" flavor,
        # see compile_one). IMGUI_USER_CONFIG must byte-match what aurora's
        # own libimgui.a was compiled with (extern/CMakeLists.txt declares
        # it PUBLIC) -- imgui.h #includes the config header, so an
        # unmatched TU would be an ODR/ABI hazard by construction.
        units.append((os.path.join(NATIVE_ROOT, "platform", "android", "touch_pad.cpp"),
                      ['-DIMGUI_USER_CONFIG="aurora/imgui_config.h"'],
                      "plat_touch_pad_aurora.o", "aurora_cpp"))
        # the SAF bridge -- the JNI poll surface
        # between the launcher's onboarding dialog and Mp6Activity's
        # ACTION_OPEN_DOCUMENT_TREE flow. Plain C, aurora flavor (jni.h
        # comes from the NDK sysroot; no SDL/aurora headers needed, but the
        # flavor keeps its flags in the windowed family). Android-only by
        # construction, like the touch overlay.
        units.append((os.path.join(NATIVE_ROOT, "platform", "android", "saf_bridge.c"),
                      [], "plat_saf_bridge_aurora.o", "aurora"))
    print(f"Total translation units: {len(units)}")

    objs = []
    failures = []
    skipped = 0
    compiled = 0
    if not args.link_only:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
            results = list(ex.map(compile_one, units))
        for src, obj, ok, log, was_skipped in results:
            objs.append(obj)
            if not ok:
                failures.append((src, log))
            elif was_skipped:
                skipped += 1
            else:
                compiled += 1
    else:
        objs = [os.path.join(OBJ_DIR, u[2]) for u in units]

    print(f"Compiled: {compiled}  Skipped(up-to-date): {skipped}  Failed: {len(failures)}")
    if failures:
        print("\n===== COMPILE FAILURES =====")
        for src, log in failures:
            print(f"\n--- {os.path.relpath(src, DECOMP if src.startswith(DECOMP) else NATIVE_ROOT)} ---")
            print(log.strip())
        print(f"\n{len(failures)} file(s) failed to compile. Not linking.")
        return 1

    # ---- savestate link stamp (android) ------------------------------------
    # Same contract as the desktop stamp above (see that block's comment):
    # savestate.c references `extern const char mp6_link_stamp[]`, and the
    # stamp must be a function of the LINKED IMAGE. The desktop link path
    # generates it; this android path compiles savestate.c too, so it must
    # generate its own stamp TU or the .so link fails with an undefined
    # symbol -- which is exactly how this block's absence was discovered.
    # Distinct per android mode so aurora/headless never share a cached obj.
    stamp_hash = hashlib.sha1()
    stamp_inputs = sorted(objs)
    if windowed:
        stamp_inputs = stamp_inputs + sorted(
            [it for it in _resolve_android_aurora_link_items() if os.path.isfile(it)])
    for it in stamp_inputs:
        try:
            with open(it, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    stamp_hash.update(chunk)
        except OSError:
            stamp_hash.update(it.encode())  # missing input: still deterministic
    stamp_hex = stamp_hash.hexdigest()
    stamp_mode = "_android_aurora" if windowed else "_android_headless"
    stamp_src = os.path.join(BUILD_DIR, f"mp6_link_stamp{stamp_mode}.c")
    stamp_obj = stamp_src[:-2] + ".o"
    stamp_body = (
        "/* AUTOGENERATED by tools/build.py -- the savestate link stamp\n"
        " * (android). sha1 over every link input; see build.py. */\n"
        f"const char mp6_link_stamp[] = \"{stamp_hex}\";\n")
    old_body = ""
    if os.path.exists(stamp_src):
        with open(stamp_src, "r", encoding="utf-8") as f:
            old_body = f.read()
    if old_body != stamp_body or not os.path.exists(stamp_obj):
        with open(stamp_src, "w", encoding="utf-8") as f:
            f.write(stamp_body)
        proc = subprocess.run([clang, "-target", ANDROID_TRIPLE, "-fPIC",
                               "-c", stamp_src, "-o", stamp_obj],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            print("[BUILD] FATAL: android link-stamp TU failed to compile:\n" + proc.stderr)
            return 1
    objs = objs + [stamp_obj]

    # ---- link libmp6game.so ------------------------------------------------
    # -shared -fPIC: the image is placed low at RUNTIME by the launcher's
    # android_dlopen_ext(ANDROID_DLEXT_RESERVED_ADDRESS) -- the proven
    # equivalent of the Windows --image-base/--no-dynamicbase pair; the
    # in-.so startup assert (mp6_assert_image_low -> dl_iterate_phdr)
    # enforces it. 16KB max-page-size on every android link from day one
    # (per the design's own risk analysis). -lz = bionic's PUBLIC libz, ABI-compatible with the
    # decomp's vendored zlib.h exactly like zlib-ng's compat build on
    # Windows (z_stream layout unchanged since 1.1.x; version check
    # compares major only). -lm/-ldl are separate libs on bionic.
    if windowed:
        # The aurora graphics .so, kept in its own subdir so the
        # headless artifact (and its byte-compare re-runs) are never
        # clobbered. clang++ (not clang) for the same reason the Windows
        # aurora link uses `zig c++`: aurora's archives are C++ and need
        # libc++ -- linked STATIC (-static-libstdc++, mirroring the android
        # tree's own CMake link line) so the APK ships no libc++_shared.so.
        # --no-undefined turns any missed symbol into a LINK error here
        # rather than a dlopen failure on the phone.
        os.makedirs(ANDROID_AURORA_OUT_DIR, exist_ok=True)
        so_path = os.path.join(ANDROID_AURORA_OUT_DIR, "libmp6game.so")
        clangxx = clang.replace("clang.exe", "clang++.exe")
        link_cmd = [clangxx, "-target", ANDROID_TRIPLE, "-shared"] + objs + [
            "-o", so_path,
            "-Wl,-soname,libmp6game.so",
            "-Wl,-z,max-page-size=16384",
            "-static-libstdc++",
            "-Wl,--build-id=sha1",
            "-Wl,--no-undefined",
            "-Wl,--gc-sections",
            # aurora's own android CMake link line carries this keep-alive:
            # SDL3-static's JNI_OnLoad (the RegisterNatives entry the APK
            # bootstrap chain-calls) must survive --gc-sections even if some
            # future link stops pulling SDL_android.c.o for other reasons.
            "-Wl,-u,JNI_OnLoad",
        ] + _resolve_android_aurora_link_items() + [
            # nod staticlib (disc-image import; cargo cross-build, see
            # NOD_ANDROID_LIB's comment). After the aurora items: its only
            # consumers are content_import.o (an objs[] member) and its own
            # libc/liblog/libm/libdl needs, all already on this line.
            NOD_ANDROID_LIB.replace("\\", "/"),
        ]
    else:
        so_path = os.path.join(ANDROID_BUILD_DIR, "libmp6game.so")
        link_cmd = [clang, "-target", ANDROID_TRIPLE, "-shared"] + objs + [
            "-o", so_path,
            "-Wl,-soname,libmp6game.so",
            "-Wl,-z,max-page-size=16384",
            "-lz", "-ldl", "-lm",
        ]
    print(f"\nLinking {so_path}...")
    proc = subprocess.run(link_cmd, capture_output=True, text=True)
    print(proc.stdout)
    print(proc.stderr)
    if proc.returncode != 0:
        print("LINK FAILED")
        return 1

    if windowed:
        # ---- libmain.so (the APK bootstrap shell) + staging ----------------
        # platform/android/mp6shell.c: pure-JNI bootstrap --
        # System.loadLibrary("main") fires its JNI_OnLoad, which installs
        # the stdout/stderr -> logcat pump, reuses mp6launcher.c's
        # reserve+dlext shape verbatim to place libmp6game.so low, then
        # chain-calls the game image's own (SDL3-static) JNI_OnLoad so SDL
        # RegisterNatives binds the org.libsdl.app classes to the low
        # image. No SDL on this link line by design.
        shell_src = os.path.join(NATIVE_ROOT, "platform", "android", "mp6shell.c")
        shell_path = os.path.join(ANDROID_AURORA_OUT_DIR, "libmain.so")
        shell_cmd = [clang, "-target", ANDROID_TRIPLE, "-O2", "-g", "-Wall", "-Wextra",
                     "-shared", "-fPIC", shell_src, "-o", shell_path,
                     "-Wl,-soname,libmain.so",
                     "-Wl,-z,max-page-size=16384",
                     "-Wl,--no-undefined",
                     "-llog", "-ldl"]
        print("Building libmain.so (APK bootstrap shell)...")
        proc = subprocess.run(shell_cmd, capture_output=True, text=True)
        print(proc.stdout)
        print(proc.stderr)
        if proc.returncode != 0:
            print("SHELL BUILD FAILED")
            return 1

        # Stage both .so into the gradle project's jniLibs (the partyboard
        # decoupling: gradle never runs the NDK; this driver is the only
        # build brain and gradle only packages what it stages).
        #
        # staging now STRIPS release-style
        # (llvm-strip --strip-unneeded -- the debug APK was 141MB because
        # the RelWithDebInfo DWARF rode into jniLibs; gradle's own strip
        # task silently skips hand-placed jniLibs when it can't find the
        # NDK). --strip-unneeded drops .debug_* AND the static .symtab but
        # KEEPS .dynsym -- which is what dladdr symbolization
        # (host_android.c's crash handler) actually reads, and every game
        # symbol in the .so is dynamic-exported. The UNSTRIPPED originals
        # stay in build/android/aurora/ as the debugging artifact; a
        # dynsym sanity probe below fails the build if the stripped image
        # ever stops carrying the symbols dladdr needs.
        import shutil
        llvm_strip = os.path.join(os.path.dirname(clang), "llvm-strip.exe")
        llvm_nm = os.path.join(os.path.dirname(clang), "llvm-nm.exe")
        jni_dir = os.path.join(NATIVE_ROOT, "platforms", "android", "app", "src",
                               "main", "jniLibs", "arm64-v8a")
        os.makedirs(jni_dir, exist_ok=True)
        for src in (so_path, shell_path):
            dst = os.path.join(jni_dir, os.path.basename(src))
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                if os.path.exists(llvm_strip):
                    proc = subprocess.run([llvm_strip, "--strip-unneeded", "-o", dst, src],
                                          capture_output=True, text=True)
                    if proc.returncode != 0:
                        print(f"[FATAL] llvm-strip failed on {src}:\n{proc.stderr}")
                        return 1
                    shutil.copystat(src, dst)
                    print(f"  staged {os.path.basename(src)} -> {os.path.relpath(dst, NATIVE_ROOT)} "
                          f"(stripped {os.path.getsize(src) / 1e6:.1f}MB -> {os.path.getsize(dst) / 1e6:.1f}MB; "
                          f"unstripped kept at {os.path.relpath(src, NATIVE_ROOT)})")
                else:
                    shutil.copy2(src, dst)
                    print(f"[WARN] llvm-strip not found at {llvm_strip} -- staged UNSTRIPPED "
                          f"{os.path.basename(src)}")
        # dladdr-symbolization sanity: the stripped game image must still
        # export its dynamic symbols (dladdr reads .dynsym, not .symtab).
        if os.path.exists(llvm_nm):
            probe = subprocess.run([llvm_nm, "-D", os.path.join(jni_dir, "libmp6game.so")],
                                   capture_output=True, text=True)
            wanted = ("mp6_android_main", "GameMain")
            missing = [w for w in wanted if w not in probe.stdout]
            if missing:
                print(f"[FATAL] stripped libmp6game.so lost dynamic symbols {missing} -- dladdr "
                      "symbolization (crash handler) would break; check the strip flags")
                return 1
            print(f"  dynsym probe: {', '.join(wanted)} present post-strip (dladdr symbolization intact)")

        print(f"\nBuilt {so_path}")
        print(f"Built {shell_path}")
        print("APK: cd platforms/android && gradlew assembleDebug (or tools/apk_package.py "
              "for the gradle-free path)")
        return 0

    # ---- build mp6launcher (single-TU exe, plain C, no game headers) -------
    launcher_src = os.path.join(NATIVE_ROOT, "platform", "android", "mp6launcher.c")
    launcher_path = os.path.join(ANDROID_BUILD_DIR, "mp6launcher")
    launcher_cmd = [clang, "-target", ANDROID_TRIPLE, "-O2", "-g", "-Wall", "-Wextra",
                    "-fPIE", "-pie", launcher_src, "-o", launcher_path,
                    "-ldl", "-Wl,-z,max-page-size=16384"]
    print("Building mp6launcher...")
    proc = subprocess.run(launcher_cmd, capture_output=True, text=True)
    print(proc.stdout)
    print(proc.stderr)
    if proc.returncode != 0:
        print("LAUNCHER BUILD FAILED")
        return 1

    print(f"\nBuilt {so_path}")
    print(f"Built {launcher_path}")
    print("\nOn-device smoke:")
    print(f"  adb push {launcher_path} {so_path} {ANDROID_DEVICE_BASE}/")
    print(f"  adb push <disc>/sys/fst.bin {ANDROID_DEVICE_BASE}/GP6E01/sys/")
    print(f"  adb push <disc>/files/sound {ANDROID_DEVICE_BASE}/GP6E01/files/sound")
    print(f"  adb shell chmod 755 {ANDROID_DEVICE_BASE}/mp6launcher")
    print(f"  adb shell {ANDROID_DEVICE_BASE}/mp6launcher 600")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--link-only", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    # the design's own target-row table.
    # Default = today's Windows behavior, byte-for-byte (the whole Windows
    # flow below is untouched; the android row branches out immediately).
    ap.add_argument("--target", choices=["win-x86_64", "aarch64-android"],
                     default="win-x86_64",
                     help="Build target row (the design's own target-row table). Default: the Windows "
                          "build exactly as before. aarch64-android: NDK-clang headless "
                          "build/android/{libmp6game.so,mp6launcher} (implies "
                          "--headless semantics, aurora/SDL excluded).")
    # A BUILD-time (not run-time) choice, deliberately -- see
    # platform/main_native.c's own header comment for why a run-time flag
    # alone (linking Aurora unconditionally, just skipping
    # aurora_initialize()) is not an option: Aurora's GX layer is a
    # software FIFO only ever drained by aurora_end_frame(), and every
    # other GX call the game makes would still link straight to Aurora's
    # real, strong symbols regardless of a run-time flag -- an unbounded-
    # accumulation risk over a long CI run with nothing ever draining it.
    ap.add_argument("--headless", action="store_true",
                     help="Build the null-platform-only executable (no Aurora "
                          "anywhere in the link) as build/mp6native_headless.exe, for CI-style "
                          "runs with no display. Default (no flag): links Aurora in for a real "
                          "window, as build/mp6native.exe.")
    # android-row-only. The default
    # android row stays byte-for-byte the headless build (so its
    # committed-log byte-compare re-runs keep working unchanged).
    ap.add_argument("--windowed", action="store_true",
                     help="With --target aarch64-android only: build the aurora/SDL3/Dawn "
                          "graphics libmp6game.so + the libmain.so APK bootstrap shell into "
                          "build/android/aurora/, and stage jniLibs for platforms/android. "
                          "Requires external_refs/repos/aurora/build-android (see "
                          "). Ignored on the Windows rows.")
    ap.add_argument("--coro-fibers", action="store_true",
                     help="A/B lever: build the Win32 FIBER "
                          "coroutine backend (platform/host/host_win32.c) instead of the DEFAULT "
                          "arena-backed minicoro backend (platform/host/coro_arena.c). Fibers "
                          "self-allocate an OS stack not guaranteed below 4GB; the minicoro backend "
                          "puts every HuPrc process stack in the low-4GB arena. Emits a separate "
                          "'_corofib'-suffixed exe/objects so both backends can be A/B'd side by side.")
    args = ap.parse_args()

    if args.target == "aarch64-android":
        return build_android(args)  # self-contained; never touches the flow below

    # Canonical single-workspace checkout: plain exe names.
    coro_exe_suffix = "_corofib" if args.coro_fibers else ""
    out_exe = os.path.join(
        BUILD_DIR,
        (f"mp6native_headless{coro_exe_suffix}.exe" if args.headless
         else f"mp6native{coro_exe_suffix}.exe"))

    os.makedirs(OBJ_DIR, exist_ok=True)

    if args.clean:
        import shutil
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        os.makedirs(OBJ_DIR, exist_ok=True)

    patch_headers()
    patch_msl_override()
    patch_abi_struct_headers()
    apply_decomp_override_headers()  # shield against foreign decomp WIP (see its docstring)
    apply_patches.apply_all()  # build/patched-src/* for the be16/be32 endianness fixes

    # the windowed build now links nod (the
    # launcher's disc-image import backend). Fail fast with the recipe
    # rather than dying later on a missing include/import-lib.
    if not args.headless and (not os.path.isdir(NOD_INCLUDE) or not os.path.exists(NOD_WIN_LIB)):
        print(f"[FATAL] nod toolchain missing under {NOD_DIR} (the launcher's disc-image import "
              f"backend) -- {NOD_RECIPE_HINT}")
        return 1

    units = collect_units(args.headless, args.coro_fibers)
    print(f"Mode: {'--headless (null platform only)' if args.headless else 'aurora (default)'}"
          f"{' [coro=fibers]' if args.coro_fibers else ' [coro=arena/minicoro]'}")
    print(f"Total translation units: {len(units)}")

    objs = []
    failures = []
    skipped = 0
    compiled = 0

    if not args.link_only:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
            results = list(ex.map(compile_one, units))
        for src, obj, ok, log, was_skipped in results:
            objs.append(obj)
            if not ok:
                failures.append((src, log))
            elif was_skipped:
                skipped += 1
            else:
                compiled += 1
    else:
        objs = [os.path.join(OBJ_DIR, u[2]) for u in units]

    print(f"Compiled: {compiled}  Skipped(up-to-date): {skipped}  Failed: {len(failures)}")
    if failures:
        print("\n===== COMPILE FAILURES =====")
        for src, log in failures:
            print(f"\n--- {os.path.relpath(src, DECOMP if src.startswith(DECOMP) else NATIVE_ROOT)} ---")
            print(log.strip())
        print(f"\n{len(failures)} file(s) failed to compile. Not linking.")
        return 1

    # ------------------------------------------------------------------
    # Savestate link stamp (review finding, savestate-x1). The old guard was
    # savestate.c's own __DATE__/__TIME__, which needs_rebuild() only refreshes
    # when savestate.c ITSELF recompiles -- so any edit to another .c relinked
    # a new exe carrying the old stamp, and the "same exact binary" check that
    # savestates depend on silently passed on a stale state whose coroutine
    # return addresses pointed into moved code. The stamp must be a function of
    # the LINKED IMAGE, so compute it here as a hash over every link input:
    # all objects, plus (windowed) the aurora archives and import libs. A
    # no-op rebuild reproduces the same hash, keeping old states valid -- the
    # per-TU __DATE__ approach would have invalidated them for nothing.
    # The stamp TU is rewritten/recompiled only when the hash changes.
    stamp_hash = hashlib.sha1()
    stamp_inputs = sorted(objs)
    if not args.headless:
        stamp_inputs = stamp_inputs + sorted(
            [it for it in _resolve_aurora_link_items() if os.path.isfile(it)]
            + [NOD_WIN_LIB]
        )
    else:
        stamp_inputs = stamp_inputs + [MP6_ZLIB_LIB_ITEM]
    for it in stamp_inputs:
        try:
            with open(it, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    stamp_hash.update(chunk)
        except OSError:
            stamp_hash.update(it.encode())  # missing input: still deterministic
    stamp_hex = stamp_hash.hexdigest()  # 40 chars, fits buildStamp[48]
    stamp_src = os.path.join(BUILD_DIR, f"mp6_link_stamp{mode_suffix_for_stamp(args)}.c")
    stamp_obj = os.path.join(OBJ_DIR, f"mp6_link_stamp{mode_suffix_for_stamp(args)}.o")
    stamp_body = (
        "/* AUTOGENERATED by tools/build.py -- the savestate link stamp.\n"
        " * sha1 over every link input; see build.py's stamp comment. */\n"
        f"const char mp6_link_stamp[] = \"{stamp_hex}\";\n")
    old_body = ""
    if os.path.exists(stamp_src):
        with open(stamp_src, "r", encoding="utf-8") as f:
            old_body = f.read()
    if old_body != stamp_body or not os.path.exists(stamp_obj):
        with open(stamp_src, "w", encoding="utf-8") as f:
            f.write(stamp_body)
        proc = subprocess.run([ZIG, "cc", "-target", TARGET, "-c", stamp_src, "-o", stamp_obj],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            print("[BUILD] FATAL: link-stamp TU failed to compile:\n" + proc.stderr)
            return 1
    objs = objs + [stamp_obj]

    if not args.headless:
        verify_aurora_carveout()  # the archives must carry the carve-out sections

    if args.headless:
        # The null-platform link recipe -- no AURORA anywhere -- plus real
        # zlib (see MP6_ZLIB_LIB_ITEM's own comment above).
        link_cmd = [ZIG, "cc", "-target", TARGET] + objs + [
            "-o", out_exe,
            f"-Wl,--image-base={IMAGE_BASE}",
            "-Wl,--no-dynamicbase",
            "-lkernel32",
            "-ldbghelp", "-lpsapi",  # the crash/RSS dbghelp+psapi consumers -- platform/host/host_win32.c
            MP6_ZLIB_LIB_ITEM.replace("\\", "/"),
        ]
    else:
        # `zig c++` (not `zig cc`): Aurora's static libs are compiled C++
        # (STL, exceptions) and need libc++/libc++abi/libunwind auto-linked,
        # which only the C++ driver adds automatically -- linking Aurora
        # this way keeps it sharing one C++ ABI/CRT with the rest of the port.
        link_cmd = [ZIG, "c++", "-target", TARGET] + objs + [
            "-o", out_exe,
            f"-Wl,--image-base={IMAGE_BASE}",
            "-Wl,--no-dynamicbase",
            f"-L{AURORA_STUB_LIBS}",  # comsuppw.lib/.a stub
        ] + _resolve_aurora_link_items() + [
            # nod import lib (disc-image import). An MSVC import lib is
            # ABI-neutral to consume -- the same class of prebuilt as
            # SDL3.lib/webgpu_dawn.lib already on this line; nod.dll rides
            # AURORA_RUNTIME_DLLS next to the exe.
            NOD_WIN_LIB.replace("\\", "/"),
        ]

    print("\nLinking...")
    proc = subprocess.run(link_cmd, capture_output=True, text=True)
    print(proc.stdout)
    print(proc.stderr)
    if proc.returncode != 0:
        print("LINK FAILED")
        return 1

    if not args.headless:
        print("\nCopying Aurora runtime DLLs next to the exe...")
        copy_aurora_runtime_dlls(os.path.dirname(out_exe))
        # Stage the ripped launcher resources
        # (res/rml stylesheets + res/fonts) next to the exe. The launcher
        # resolves res/ under the CWD first (the documented run-from-repo-
        # root workflow reads the repo's own res/ directly), exe-relative
        # second (double-click runs) -- see launcher_core.cpp's
        # mp6_resource_base(). Launcher mode only ever READS these;
        # automation runs never touch them.
        import shutil
        res_src = os.path.join(NATIVE_ROOT, "res")
        res_dst = os.path.join(os.path.dirname(out_exe), "res")
        if os.path.isdir(res_src):
            copied = 0
            for root, _dirs, files in os.walk(res_src):
                rel = os.path.relpath(root, res_src)
                dst_root = os.path.join(res_dst, rel) if rel != "." else res_dst
                os.makedirs(dst_root, exist_ok=True)
                for fn in files:
                    s = os.path.join(root, fn)
                    d = os.path.join(dst_root, fn)
                    if not os.path.exists(d) or os.path.getmtime(d) < os.path.getmtime(s):
                        shutil.copy2(s, d)
                        copied += 1
            if copied:
                print(f"  staged {copied} launcher resource file(s) -> build/res")
    else:
        # --headless links real zlib too (MP6_ZLIB_LIB_ITEM above) -- needs
        # its DLL alongside the exe same as any other dynamically-linked
        # import library, headless or not.
        dst_dir = os.path.dirname(out_exe)
        if os.path.exists(MP6_ZLIB_DLL):
            dst = os.path.join(dst_dir, os.path.basename(MP6_ZLIB_DLL))
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(MP6_ZLIB_DLL):
                import shutil
                shutil.copy2(MP6_ZLIB_DLL, dst)
                print(f"  copied {os.path.basename(MP6_ZLIB_DLL)}")
        else:
            print(f"[WARN] {MP6_ZLIB_DLL} missing -- build external_refs/repos/aurora first "
                  f"or mp6native_headless.exe won't start")

    print(f"\nBuilt {out_exe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
