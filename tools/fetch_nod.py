#!/usr/bin/env python3
"""fetch_nod.py -- populates port/toolchain/nod for the launcher's
content-import backend.

nod (https://github.com/encounter/nod) is the GC/Wii disc-image library
(ISO/GCM/CISO/GCZ/RVZ/WIA/...) behind the launcher's first-run import --
the exact library+version aurora's own AURORA_ENABLE_DVD dependency table
pins (cmake/AuroraDependencyVersions.cmake), consumed through its C FFI
(nod-ffi). Dual-licensed MIT OR Apache-2.0 (license texts are fetched next
to the artifacts).

Two artifacts, both pinned to NOD_VERSION:

  windows-x86_64/  the official prebuilt release package (nod.dll + MSVC
                   import lib + nod.h). An MSVC-built DLL exposes a plain C
                   ABI -- the same prebuilt class as the SDL3.lib /
                   webgpu_dawn.lib lines the Windows link already carries.

  android-aarch64/ cargo cross-build of the nod-ffi STATICLIB (no android
                   prebuilt exists upstream; the MP4 port's CI does this
                   same rustup-target build). Requires the self-contained
                   rust toolchain under port/toolchain/rust (rustup-init
                   -y --no-modify-path --default-toolchain stable --profile
                   minimal --target aarch64-linux-android, with
                   RUSTUP_HOME/CARGO_HOME pointed into that directory) and
                   the NDK the android rows already use. Compression
                   features use the -static (cargo-vendored) variants --
                   nod's own CMakeLists' default mode.

Everything lands under port/toolchain/ (machine-local, like zig/the NDK;
nothing is committed to the repo).

Usage:
    python tools/fetch_nod.py             # both platforms
    python tools/fetch_nod.py --windows   # prebuilt package only
    python tools/fetch_nod.py --android   # cargo cross-build only
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

NOD_VERSION = "v2.0.0-alpha.10"  # = aurora's AURORA_NOD_VERSION pin

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
NOD_DIR = os.path.join(PORT_ROOT, "toolchain", "nod")
RUST_ROOT = os.path.join(PORT_ROOT, "toolchain", "rust")

WIN_PKG_URL = (f"https://github.com/encounter/nod/releases/download/{NOD_VERSION}/"
               "libnod-windows-x86_64.tar.gz")
SRC_URL = f"https://github.com/encounter/nod/archive/refs/tags/{NOD_VERSION}.tar.gz"
LICENSE_URLS = {
    "LICENSE-MIT": f"https://raw.githubusercontent.com/encounter/nod/{NOD_VERSION}/LICENSE-MIT",
    "LICENSE-APACHE": f"https://raw.githubusercontent.com/encounter/nod/{NOD_VERSION}/LICENSE-APACHE",
}


def _download(url, dst):
    if os.path.exists(dst):
        print(f"  cached: {os.path.basename(dst)}")
        return
    print(f"  fetching {url}")
    tmp = dst + ".part"
    urllib.request.urlretrieve(url, tmp)
    os.replace(tmp, dst)


def fetch_licenses():
    for name, url in LICENSE_URLS.items():
        _download(url, os.path.join(NOD_DIR, name))


def fetch_windows():
    print(f"[nod] windows-x86_64 prebuilt ({NOD_VERSION})")
    dl = os.path.join(NOD_DIR, "_dl")
    os.makedirs(dl, exist_ok=True)
    pkg = os.path.join(dl, "libnod-windows-x86_64.tar.gz")
    _download(WIN_PKG_URL, pkg)
    stage = os.path.join(dl, "win-extract")
    shutil.rmtree(stage, ignore_errors=True)
    with tarfile.open(pkg, "r:gz") as tf:
        tf.extractall(stage)
    # Package layout: include/nod.h, lib/nod.lib, lib/nod_static.lib, bin/nod.dll
    os.makedirs(os.path.join(NOD_DIR, "include"), exist_ok=True)
    shutil.copy2(os.path.join(stage, "include", "nod.h"), os.path.join(NOD_DIR, "include", "nod.h"))
    win = os.path.join(NOD_DIR, "windows-x86_64")
    os.makedirs(os.path.join(win, "lib"), exist_ok=True)
    os.makedirs(os.path.join(win, "bin"), exist_ok=True)
    shutil.copy2(os.path.join(stage, "lib", "nod.lib"), os.path.join(win, "lib", "nod.lib"))
    shutil.copy2(os.path.join(stage, "bin", "nod.dll"), os.path.join(win, "bin", "nod.dll"))
    print(f"  -> {win} (nod.lib + nod.dll), {os.path.join(NOD_DIR, 'include', 'nod.h')}")


def _find_ndk_root():
    env = os.environ.get("ANDROID_NDK_ROOT")
    if env and os.path.isdir(env):
        return env
    ndk_base = os.path.join(PORT_ROOT, "android-sdk", "ndk")
    if os.path.isdir(ndk_base):
        vers = sorted(d for d in os.listdir(ndk_base) if os.path.isdir(os.path.join(ndk_base, d)))
        if vers:
            return os.path.join(ndk_base, vers[-1])
    return None


def fetch_android():
    print(f"[nod] android-aarch64 staticlib ({NOD_VERSION}, cargo cross-build)")
    cargo = os.path.join(RUST_ROOT, "cargo", "bin", "cargo.exe")
    if not os.path.exists(cargo):
        print(f"[FATAL] {cargo} missing -- install the self-contained rust toolchain first "
              "(see this script's docstring)")
        return 1
    ndk = _find_ndk_root()
    if not ndk:
        print("[FATAL] no NDK under port/android-sdk/ndk (or ANDROID_NDK_ROOT)")
        return 1
    ndk_bin = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin")
    target_cc = os.path.join(ndk_bin, "aarch64-linux-android28-clang.cmd")
    if not os.path.exists(target_cc):
        print(f"[FATAL] NDK target clang wrapper missing: {target_cc}")
        return 1

    dl = os.path.join(NOD_DIR, "_dl")
    os.makedirs(dl, exist_ok=True)
    src_pkg = os.path.join(dl, f"nod-src-{NOD_VERSION}.tar.gz")
    _download(SRC_URL, src_pkg)
    src_root = os.path.join(dl, f"nod-{NOD_VERSION.lstrip('v')}")
    if not os.path.isdir(src_root):
        with tarfile.open(src_pkg, "r:gz") as tf:
            tf.extractall(dl)
    if not os.path.isdir(src_root):
        print(f"[FATAL] source extraction did not produce {src_root}")
        return 1

    env = dict(os.environ)
    env["RUSTUP_HOME"] = os.path.join(RUST_ROOT, "rustup")
    env["CARGO_HOME"] = os.path.join(RUST_ROOT, "cargo")
    # cc-rs / cargo cross env: the NDK api-28 clang wrapper for both the
    # linker (cdylib half of nod-ffi's crate-type) and the -sys crates' C
    # builds (bzip2/lzma/zlib/zstd, vendored-static features).
    env["CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"] = target_cc
    env["CC_aarch64_linux_android"] = target_cc
    env["AR_aarch64_linux_android"] = os.path.join(ndk_bin, "llvm-ar.exe")
    env["RANLIB_aarch64_linux_android"] = os.path.join(ndk_bin, "llvm-ranlib.exe")

    # Feature set = nod's own CMakeLists default ("Cargo-vendored
    # compression: -static features"), threading kept (preloader off by
    # default at runtime; harmless and matches the release packages).
    features = "compress-bzip2-static,compress-lzma-static,compress-zlib-static,compress-zstd-static,threading"
    cmd = [cargo, "build", "-p", "nod-ffi", "--release",
           "--target", "aarch64-linux-android",
           "--no-default-features", "--features", features]
    print("  " + " ".join(cmd))
    proc = subprocess.run(cmd, cwd=src_root, env=env)
    if proc.returncode != 0:
        print("[FATAL] cargo build failed")
        return 1

    built = os.path.join(src_root, "target", "aarch64-linux-android", "release", "libnod.a")
    if not os.path.exists(built):
        print(f"[FATAL] cargo build produced no {built}")
        return 1
    out = os.path.join(NOD_DIR, "android-aarch64")
    os.makedirs(out, exist_ok=True)
    shutil.copy2(built, os.path.join(out, "libnod.a"))
    print(f"  -> {os.path.join(out, 'libnod.a')} ({os.path.getsize(built) / 1e6:.1f}MB)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--windows", action="store_true")
    ap.add_argument("--android", action="store_true")
    args = ap.parse_args()
    both = not args.windows and not args.android

    os.makedirs(NOD_DIR, exist_ok=True)
    fetch_licenses()
    rc = 0
    if args.windows or both:
        fetch_windows()
    if args.android or both:
        rc = fetch_android() or 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
