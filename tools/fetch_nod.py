#!/usr/bin/env python3
"""Fetch the pinned nod C-FFI artifacts used by the MP6 import pipeline.

Every network payload is SHA-256 pinned. Installed artifacts carry a
content manifest, so setup/build reject incomplete or stale toolchains
instead of accepting files merely because their names exist.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import urllib.parse

NOD_VERSION = "v2.0.0-alpha.10"  # aurora's AURORA_NOD_VERSION pin
ANDROID_MANIFEST_VERSION = 6
ANDROID_RUST_TARGET = "aarch64-linux-android"
ANDROID_RUST_TOOLCHAIN = "1.97.1-x86_64-pc-windows-msvc"
ANDROID_RUST_RELEASE = "1.97.1"
ANDROID_RUST_COMMIT = "8bab26f4f68e0e26f0bb7960be334d5b520ea452"
ANDROID_RUST_HOST = "x86_64-pc-windows-msvc"
ANDROID_NDK_VERSION = "27.3.13750724"
MAX_RUST_TREE_FILES = 4096
MAX_RUST_TREE_BYTES = 2 * 1024 * 1024 * 1024
ANDROID_FEATURES = (
    "compress-bzip2-static,compress-lzma-static,compress-zlib-static,"
    "compress-zstd-static,threading"
)

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
NOD_DIR = os.path.join(PORT_ROOT, "toolchain", "nod")
RUST_ROOT = os.path.join(PORT_ROOT, "toolchain", "rust")

RELEASE_BASE = f"https://github.com/encounter/nod/releases/download/{NOD_VERSION}"
HOST_PACKAGES = {
    ("Windows", "x86_64"): {
        "id": "windows-x86_64",
        "asset": "libnod-windows-x86_64.tar.gz",
        "sha256": "48616461145b979744821c5076b793fa246f4d55bfe647c1defc1e40699d0a1f",
        "required": ("bin/nod.dll", "lib/nod.lib"),
    },
    ("Linux", "x86_64"): {
        "id": "linux-x86_64",
        "asset": "libnod-linux-x86_64.tar.gz",
        "sha256": "f0d02a071d57bc7c7378b0456e3fc9b67061f0a50580d98612ec0f3556c40eb4",
        "required": ("lib/libnod.so",),
    },
    ("Darwin", "aarch64"): {
        "id": "macos-aarch64",
        "asset": "libnod-macos-aarch64.tar.gz",
        "sha256": "878fa0afb92175c555ec949322c263886b38a304499a43c12861261e5be61e87",
        "required": ("lib/libnod.dylib",),
    },
}
SOURCE_URL = f"https://github.com/encounter/nod/archive/refs/tags/{NOD_VERSION}.tar.gz"
SOURCE_SHA256 = "58bef467548fb2ee09fcfe026ebb2cd0f6369d9e5003ea243ae9afc79b5cb529"
LICENSES = {
    "LICENSE-MIT": (
        f"https://raw.githubusercontent.com/encounter/nod/{NOD_VERSION}/LICENSE-MIT",
        "544ffe9befaefa6b59ab1fdde58789069ba7c1b65432bdea273a16b2117909b7",
    ),
    "LICENSE-APACHE": (
        f"https://raw.githubusercontent.com/encounter/nod/{NOD_VERSION}/LICENSE-APACHE",
        "fd363f1eb5135402a2e66f4a6173e6eff06daabe79d150acd7c669d33b985b4e",
    ),
}

# The verified release payload provides stable Windows binaries. These
# extra checks make a hand-written manifest insufficient to bless a foreign
# DLL/import library/header at the most common host target.
WINDOWS_FILE_SHA256 = {
    "include/nod.h": "8fa7751dfa1868b3e26fbf01d218a25de216617e2b04caf8c1c6eaf1ac141580",
    "windows-x86_64/bin/nod.dll": "1186486bc236e16d7cdf114bcee9e998aeb89f83b9560078c5edc42eafbcd22b",
    "windows-x86_64/lib/nod.lib": "7b1b846802cdcaa322c1421c4282e0201fe03c56fca657ea604491156d9beabb",
}


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_json_atomic(path, value):
    tmp = f"{path}.part-{os.getpid()}"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(value, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)
    finally:
        try:
            os.remove(tmp)
        except FileNotFoundError:
            pass


def _download(url, dst, expected_sha256):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isfile(dst):
        got = _sha256_file(dst)
        if got == expected_sha256:
            print(f"  cached + verified: {os.path.basename(dst)} ({got})")
            return
        print(f"  cached digest mismatch; replacing {dst} (got {got})")
        os.remove(dst)
    print(f"  fetching {url}")
    tmp = dst + ".part"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "mp6-port-setup"})
        with urllib.request.urlopen(req, timeout=60) as response, open(tmp, "wb") as out:
            shutil.copyfileobj(response, out)
        got = _sha256_file(tmp)
        if got != expected_sha256:
            raise RuntimeError(
                f"SHA-256 mismatch for {url}: got {got}, expected {expected_sha256}"
            )
        os.replace(tmp, dst)
        print(f"  sha256 verified: {got}")
    finally:
        try:
            os.remove(tmp)
        except FileNotFoundError:
            pass


def _safe_extract(archive, destination):
    root = os.path.realpath(destination)
    os.makedirs(root, exist_ok=True)
    with tarfile.open(archive, "r:gz") as tf:
        members = tf.getmembers()
        for member in members:
            target = os.path.realpath(os.path.join(root, member.name))
            try:
                contained = os.path.commonpath((root, target)) == root
            except ValueError:
                contained = False
            if not contained or member.issym() or member.islnk():
                raise RuntimeError(f"unsafe path/link in {archive}: {member.name!r}")
        try:
            tf.extractall(root, filter="fully_trusted")
        except TypeError:  # Python 3.8-3.11 have no filter= argument.
            tf.extractall(root)


def _replace_tree(stage, destination):
    previous = f"{destination}.previous-{os.getpid()}"
    shutil.rmtree(previous, ignore_errors=True)
    moved_old = False
    try:
        if os.path.exists(destination):
            os.replace(destination, previous)
            moved_old = True
        os.replace(stage, destination)
    except Exception:
        if moved_old and not os.path.exists(destination) and os.path.exists(previous):
            os.replace(previous, destination)
        raise
    shutil.rmtree(previous, ignore_errors=True)


def _copy_file_atomic(source, destination):
    tmp = f"{destination}.part-{os.getpid()}"
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    try:
        shutil.copy2(source, tmp)
        os.replace(tmp, destination)
    finally:
        try:
            os.remove(tmp)
        except FileNotFoundError:
            pass


def _normalize_machine(machine):
    value = machine.lower()
    if value in ("amd64", "x86_64"):
        return "x86_64"
    if value in ("arm64", "aarch64"):
        return "aarch64"
    return value


def host_spec(system=None, machine=None):
    key = (system or platform.system(), _normalize_machine(machine or platform.machine()))
    spec = HOST_PACKAGES.get(key)
    if spec is None:
        supported = ", ".join(f"{s}/{m}" for s, m in HOST_PACKAGES)
        raise RuntimeError(f"nod has no pinned host package for {key[0]}/{key[1]}; supported: {supported}")
    return spec


def _host_manifest_path(spec):
    return os.path.join(NOD_DIR, f".mp6-nod-host-{spec['id']}.json")


def _manifest_files(platform_dir, spec):
    paths = [os.path.join(NOD_DIR, "include", "nod.h")]
    for root, _dirs, files in os.walk(platform_dir):
        paths.extend(os.path.join(root, name) for name in files)
    return {
        os.path.relpath(path, NOD_DIR).replace("\\", "/"): _sha256_file(path)
        for path in sorted(paths)
    }


def check_host_install(system=None, machine=None):
    try:
        spec = host_spec(system, machine)
    except RuntimeError as exc:
        return False, str(exc)
    manifest_path = _host_manifest_path(spec)
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        return False, f"missing/invalid nod host manifest {manifest_path}: {exc}"
    if (manifest.get("version") != NOD_VERSION
            or manifest.get("host") != spec["id"]
            or manifest.get("package_sha256") != spec["sha256"]):
        return False, f"stale nod host manifest: {manifest_path}"
    required = ["include/nod.h"] + [f"{spec['id']}/{rel}" for rel in spec["required"]]
    recorded = manifest.get("files")
    if not isinstance(recorded, dict) or any(rel not in recorded for rel in required):
        return False, f"incomplete nod host manifest: {manifest_path}"
    for rel, expected in recorded.items():
        path = os.path.join(NOD_DIR, *rel.split("/"))
        if not os.path.isfile(path):
            return False, f"nod host artifact is missing: {path}"
        if _sha256_file(path) != expected:
            return False, f"nod host artifact is stale/corrupt: {path}"
    if spec["id"] == "windows-x86_64":
        for rel, expected in WINDOWS_FILE_SHA256.items():
            if recorded.get(rel) != expected:
                return False, f"nod Windows artifact digest does not match {NOD_VERSION}: {rel}"
    return True, None


def verify_host_install(system=None, machine=None):
    return check_host_install(system, machine)[0]


def fetch_licenses():
    os.makedirs(NOD_DIR, exist_ok=True)
    for name, (url, digest) in LICENSES.items():
        _download(url, os.path.join(NOD_DIR, name), digest)


def fetch_host(system=None, machine=None):
    spec = host_spec(system, machine)
    print(f"[nod] {spec['id']} prebuilt ({NOD_VERSION})")
    dl = os.path.join(NOD_DIR, "_dl")
    package = os.path.join(dl, f"{NOD_VERSION}-{spec['asset']}")
    _download(f"{RELEASE_BASE}/{spec['asset']}", package, spec["sha256"])

    extracted = os.path.join(dl, f"extract-{spec['id']}-{os.getpid()}")
    shutil.rmtree(extracted, ignore_errors=True)
    try:
        _safe_extract(package, extracted)
        header = os.path.join(extracted, "include", "nod.h")
        required = [os.path.join(extracted, *rel.split("/")) for rel in spec["required"]]
        if not os.path.isfile(header) or any(not os.path.isfile(path) for path in required):
            raise RuntimeError(f"{spec['asset']} does not contain its documented nod.h/runtime files")

        destination = os.path.join(NOD_DIR, spec["id"])
        stage = f"{destination}.staging-{os.getpid()}"
        shutil.rmtree(stage, ignore_errors=True)
        os.makedirs(stage)
        for dirname in ("bin", "lib"):
            source_dir = os.path.join(extracted, dirname)
            if os.path.isdir(source_dir):
                shutil.copytree(source_dir, os.path.join(stage, dirname))
        _replace_tree(stage, destination)
        _copy_file_atomic(header, os.path.join(NOD_DIR, "include", "nod.h"))

        files = _manifest_files(destination, spec)
        if spec["id"] == "windows-x86_64":
            for rel, expected in WINDOWS_FILE_SHA256.items():
                if files.get(rel) != expected:
                    raise RuntimeError(
                        f"verified package extracted unexpected {rel} digest: {files.get(rel)}"
                    )
        _write_json_atomic(_host_manifest_path(spec), {
            "version": NOD_VERSION,
            "host": spec["id"],
            "package": spec["asset"],
            "package_sha256": spec["sha256"],
            "files": files,
        })
    finally:
        shutil.rmtree(extracted, ignore_errors=True)
    ok, problem = check_host_install(system, machine)
    if not ok:
        raise RuntimeError(problem)
    print(f"  -> {os.path.join(NOD_DIR, spec['id'])} (content manifest verified)")


def fetch_windows():
    return fetch_host("Windows", "x86_64")


def _find_ndk_root():
    env = os.environ.get("ANDROID_NDK_ROOT")
    candidate = env if env else os.path.join(
        PORT_ROOT, "android-sdk", "ndk", ANDROID_NDK_VERSION
    )
    candidate = os.path.realpath(candidate)
    if not os.path.isdir(candidate) or os.path.basename(candidate) != ANDROID_NDK_VERSION:
        return None
    properties = os.path.join(candidate, "source.properties")
    try:
        with open(properties, "r", encoding="utf-8") as f:
            revisions = [
                line.split("=", 1)[1].strip() for line in f
                if "=" in line and line.split("=", 1)[0].strip() == "Pkg.Revision"
            ]
    except OSError:
        return None
    return candidate if revisions == [ANDROID_NDK_VERSION] else None


def _android_tools():
    cargo_name = "cargo.exe" if os.name == "nt" else "cargo"
    rustc_name = "rustc.exe" if os.name == "nt" else "rustc"
    rustup_name = "rustup.exe" if os.name == "nt" else "rustup"
    cargo = os.path.join(RUST_ROOT, "cargo", "bin", cargo_name)
    ndk = _find_ndk_root()
    if not ndk:
        raise RuntimeError("no NDK under port/android-sdk/ndk (or ANDROID_NDK_ROOT)")
    host_dir = "windows-x86_64" if os.name == "nt" else (
        "darwin-x86_64" if platform.system() == "Darwin" else "linux-x86_64"
    )
    suffix = ".cmd" if os.name == "nt" else ""
    exe_suffix = ".exe" if os.name == "nt" else ""
    ndk_bin = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host_dir, "bin")
    tools = {
        "cargo": cargo,
        "rustc": os.path.join(RUST_ROOT, "cargo", "bin", rustc_name),
        "rustup": os.path.join(RUST_ROOT, "cargo", "bin", rustup_name),
        "target_cc": os.path.join(ndk_bin, f"aarch64-linux-android28-clang{suffix}"),
        "clang": os.path.join(ndk_bin, f"clang{exe_suffix}"),
        "ar": os.path.join(ndk_bin, f"llvm-ar{exe_suffix}"),
        "ranlib": os.path.join(ndk_bin, f"llvm-ranlib{exe_suffix}"),
    }
    missing = [path for path in tools.values() if not os.path.isfile(path)]
    if missing:
        raise RuntimeError("Android nod toolchain file(s) missing: " + ", ".join(missing))
    return tools


def _rust_environment(toolchain=None):
    env = dict(os.environ)
    # Do not let ambient wrappers or flags silently change the compiler whose
    # identity is recorded below. fetch_android() adds only its documented
    # target/linker variables after calling this helper.
    blocked = {
        "RUSTC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "RUSTFLAGS",
        "RUSTDOCFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTUP_TOOLCHAIN",
        "CARGO_BUILD_RUSTC", "CARGO_BUILD_RUSTC_WRAPPER", "CARGO_BUILD_RUSTFLAGS",
        "CARGO_BUILD_TARGET", "CARGO_TARGET_DIR",
        "CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS",
        "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER",
        "CC", "CXX", "AR", "RANLIB", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
        "CC_AARCH64_LINUX_ANDROID", "CXX_AARCH64_LINUX_ANDROID",
        "AR_AARCH64_LINUX_ANDROID", "RANLIB_AARCH64_LINUX_ANDROID",
        "ARFLAGS", "HOST_AR", "HOST_CC", "HOST_CFLAGS", "HOST_CXX",
        "HOST_CXXFLAGS", "LD", "NM", "OBJCOPY", "STRIP", "TARGET_AR",
        "TARGET_CC", "TARGET_CFLAGS", "TARGET_CXX", "TARGET_CXXFLAGS",
    }
    # Native dependency build scripts recognize additional escape hatches
    # outside Rust/Cargo's own namespace (notably ZSTD_SYS_USE_PKG_CONFIG and
    # cc-rs' CRATE_CC_NO_DEFAULTS). A caller's shell must not be able to swap
    # in host libraries, compilers, flags, CMake toolchains, or vcpkg state.
    blocked_prefixes = (
        "BZIP", "CMAKE", "CRATE_CC", "LIBLZMA", "LIBZ", "LZMA", "PKG_CONFIG",
        "RUST", "CARGO", "VCPKG", "XZ_", "ZLIB", "ZSTD",
    )
    for name in list(env):
        upper = name.upper()
        if upper.startswith(blocked_prefixes) or upper in blocked:
            env.pop(name, None)
    env["RUSTUP_HOME"] = os.path.join(RUST_ROOT, "rustup")
    env["CARGO_HOME"] = os.path.join(RUST_ROOT, "cargo")
    if toolchain:
        env["RUSTUP_TOOLCHAIN"] = toolchain
    return env


def _capture_tool_output(command, env):
    try:
        proc = subprocess.run(
            command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace",
        )
    except OSError as exc:
        raise RuntimeError(f"cannot run Rust tool {command[0]}: {exc}") from exc
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        raise RuntimeError(
            f"Rust tool failed ({' '.join(command)}): {detail or f'exit {proc.returncode}'}"
        )
    value = proc.stdout.strip()
    if not value:
        raise RuntimeError(f"Rust tool produced no identity output: {' '.join(command)}")
    return value


def _bounded_tree_fingerprint(root, label):
    root = os.path.realpath(root)
    if not os.path.isdir(root):
        raise RuntimeError(f"Rust {label} directory is missing: {root}")
    digest = hashlib.sha256()
    count = 0
    total_bytes = 0
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        files.sort()
        for dirname in dirs:
            path = os.path.join(current, dirname)
            if os.path.islink(path):
                raise RuntimeError(f"Rust {label} contains an unsupported directory link: {path}")
        for filename in files:
            path = os.path.join(current, filename)
            if os.path.islink(path) or not os.path.isfile(path):
                raise RuntimeError(f"Rust {label} contains an unsupported file/link: {path}")
            size = os.path.getsize(path)
            count += 1
            total_bytes += size
            if count > MAX_RUST_TREE_FILES or total_bytes > MAX_RUST_TREE_BYTES:
                raise RuntimeError(
                    f"Rust {label} exceeds fingerprint bounds "
                    f"({count} files, {total_bytes} bytes)"
                )
            relative = os.path.relpath(path, root).replace("\\", "/").encode("utf-8")
            digest.update(len(relative).to_bytes(4, "big"))
            digest.update(relative)
            digest.update(size.to_bytes(8, "big"))
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    digest.update(chunk)
    if not count:
        raise RuntimeError(f"Rust {label} directory is empty: {root}")
    return {
        "path": root,
        "files": count,
        "bytes": total_bytes,
        "sha256": digest.hexdigest(),
    }


def _android_toolchain_identity(tools):
    identity = {
        name: {"path": os.path.realpath(path), "sha256": _sha256_file(path)}
        for name, path in sorted(tools.items())
    }
    rust_env = _rust_environment(ANDROID_RUST_TOOLCHAIN)
    try:
        active_output = _capture_tool_output(
            [tools["rustup"], "show", "active-toolchain"], rust_env
        )
        rustc_vv = _capture_tool_output([tools["rustc"], "-Vv"], rust_env)
    except RuntimeError as exc:
        raise RuntimeError(
            f"Android Nod requires the exact Rust toolchain {ANDROID_RUST_TOOLCHAIN}; "
            f"install it with `rustup toolchain install {ANDROID_RUST_TOOLCHAIN} "
            f"--profile minimal --target {ANDROID_RUST_TARGET}`: {exc}"
        ) from exc
    active_toolchain = active_output.split(None, 1)[0]
    if active_toolchain != ANDROID_RUST_TOOLCHAIN:
        raise RuntimeError(
            f"rustup selected {active_toolchain!r}, expected {ANDROID_RUST_TOOLCHAIN!r}"
        )
    version_fields = {}
    for line in rustc_vv.splitlines()[1:]:
        if ": " in line:
            key, value = line.split(": ", 1)
            version_fields[key] = value
    expected_fields = {
        "release": ANDROID_RUST_RELEASE,
        "commit-hash": ANDROID_RUST_COMMIT,
        "host": ANDROID_RUST_HOST,
    }
    if any(version_fields.get(key) != value for key, value in expected_fields.items()):
        raise RuntimeError(
            f"Rust compiler identity mismatch for {ANDROID_RUST_TOOLCHAIN}: "
            f"expected {expected_fields!r}, found {version_fields!r}"
        )
    cargo_vv = _capture_tool_output([tools["cargo"], "-Vv"], rust_env)
    sysroot = os.path.realpath(_capture_tool_output(
        [tools["rustc"], "--print", "sysroot"], rust_env
    ))
    toolchains_root = os.path.realpath(os.path.join(RUST_ROOT, "rustup", "toolchains"))
    try:
        local_sysroot = os.path.commonpath((toolchains_root, sysroot)) == toolchains_root
    except ValueError:
        local_sysroot = False
    if not local_sysroot or not os.path.isdir(sysroot):
        raise RuntimeError(
            f"active Rust sysroot is not inside the self-contained toolchain: {sysroot}"
        )
    target_libdir = os.path.realpath(_capture_tool_output(
        [tools["rustc"], "--print", "target-libdir", "--target", ANDROID_RUST_TARGET],
        rust_env,
    ))
    try:
        local_target = os.path.commonpath((sysroot, target_libdir)) == sysroot
    except ValueError:
        local_target = False
    if not local_target:
        raise RuntimeError(f"Rust target library escaped the active sysroot: {target_libdir}")
    host_libdir = os.path.realpath(_capture_tool_output(
        [tools["rustc"], "--print", "target-libdir", "--target", ANDROID_RUST_HOST],
        rust_env,
    ))
    try:
        local_host = os.path.commonpath((sysroot, host_libdir)) == sysroot
    except ValueError:
        local_host = False
    if not local_host:
        raise RuntimeError(f"Rust host library escaped the active sysroot: {host_libdir}")
    executable_suffix = ".exe" if os.name == "nt" else ""
    actual_rustc = os.path.join(sysroot, "bin", "rustc" + executable_suffix)
    actual_cargo = os.path.join(sysroot, "bin", "cargo" + executable_suffix)
    for label, path in (("rustc", actual_rustc), ("cargo", actual_cargo)):
        if not os.path.isfile(path):
            raise RuntimeError(f"active Rust {label} binary is missing: {path}")
    identity["rust"] = {
        "active_toolchain": active_toolchain,
        "active_toolchain_output": active_output,
        "rustc_vv": rustc_vv,
        "cargo_vv": cargo_vv,
        "sysroot": sysroot,
        "rustc": {"path": actual_rustc, "sha256": _sha256_file(actual_rustc)},
        "cargo": {"path": actual_cargo, "sha256": _sha256_file(actual_cargo)},
        "compiler_bin": _bounded_tree_fingerprint(os.path.join(sysroot, "bin"), "compiler bin"),
        "host": ANDROID_RUST_HOST,
        "host_stdlib": _bounded_tree_fingerprint(host_libdir, "host target stdlib"),
        "target": ANDROID_RUST_TARGET,
        "target_stdlib": _bounded_tree_fingerprint(target_libdir, "Android target stdlib"),
    }
    return identity


def _cargo_lock_registry_packages(lock_bytes):
    try:
        lines = lock_bytes.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"Cargo.lock is not UTF-8: {exc}") from exc
    packages = []
    current = None

    def finish():
        if current is None:
            return
        source = current.get("source")
        if source is None:
            return  # workspace package
        if source != "registry+https://github.com/rust-lang/crates.io-index":
            raise RuntimeError(f"unsupported non-crates.io Cargo.lock source: {source!r}")
        missing = [key for key in ("name", "version", "checksum") if key not in current]
        if missing or not re.fullmatch(r"[0-9a-f]{64}", current.get("checksum", "")):
            raise RuntimeError(f"invalid registry package in Cargo.lock: {current!r}")
        packages.append(dict(current))

    assignment = re.compile(r"^(name|version|source|checksum)\s*=\s*(\"(?:[^\"\\]|\\.)*\")\s*$")
    for raw_line in lines:
        line = raw_line.strip()
        if line == "[[package]]":
            finish()
            current = {}
            continue
        if current is None:
            continue
        match = assignment.match(line)
        if match:
            try:
                current[match.group(1)] = json.loads(match.group(2))
            except ValueError as exc:
                raise RuntimeError(f"invalid Cargo.lock string: {line!r}") from exc
    finish()
    if not packages:
        raise RuntimeError("Cargo.lock contains no crates.io packages")
    packages.sort(key=lambda item: (item["name"], item["version"]))
    filenames = [f"{item['name']}-{item['version']}.crate" for item in packages]
    if len(filenames) != len(set(filenames)):
        raise RuntimeError("Cargo.lock has ambiguous duplicate crate filenames")
    for item, filename in zip(packages, filenames):
        item["filename"] = filename
    return packages


def _source_cargo_lock(source_package):
    if not os.path.isfile(source_package) or _sha256_file(source_package) != SOURCE_SHA256:
        raise RuntimeError(f"pinned Nod source package is missing/corrupt: {source_package}")
    with tarfile.open(source_package, "r:gz") as tf:
        members = [member for member in tf.getmembers() if member.name.endswith("/Cargo.lock")]
        if len(members) != 1 or members[0].size > 2 * 1024 * 1024:
            raise RuntimeError("pinned Nod source has an invalid/ambiguous Cargo.lock")
        stream = tf.extractfile(members[0])
        if stream is None:
            raise RuntimeError("cannot read Cargo.lock from pinned Nod source")
        return stream.read()


def _verified_crate_archives(lock_bytes, allow_download):
    packages = _cargo_lock_registry_packages(lock_bytes)
    verified_cache = os.path.join(NOD_DIR, "_cargo-crates")
    if allow_download:
        os.makedirs(verified_cache, exist_ok=True)
    elif not os.path.isdir(verified_cache):
        raise RuntimeError(f"verified Cargo crate cache is missing: {verified_cache}")
    cargo_cache = os.path.join(RUST_ROOT, "cargo", "registry", "cache")
    records = []
    for package in packages:
        filename = package["filename"]
        expected = package["checksum"]
        destination = os.path.join(verified_cache, filename)
        valid = os.path.isfile(destination) and _sha256_file(destination) == expected
        if not valid and allow_download:
            seeded = False
            if os.path.isdir(cargo_cache):
                for current, _dirs, files in os.walk(cargo_cache):
                    if filename not in files:
                        continue
                    candidate = os.path.join(current, filename)
                    if _sha256_file(candidate) == expected:
                        _copy_file_atomic(candidate, destination)
                        seeded = True
                        break
            if not seeded:
                crate = urllib.parse.quote(package["name"], safe="")
                archive = urllib.parse.quote(filename, safe="")
                _download(
                    f"https://static.crates.io/crates/{crate}/{archive}",
                    destination,
                    expected,
                )
            valid = os.path.isfile(destination) and _sha256_file(destination) == expected
        if not valid:
            got = _sha256_file(destination) if os.path.isfile(destination) else "missing"
            raise RuntimeError(
                f"verified Cargo crate cache mismatch for {filename}: expected {expected}, got {got}"
            )
        records.append((package, destination, os.path.getsize(destination)))

    digest = hashlib.sha256()
    total_bytes = 0
    for package, _path, size in records:
        encoded = package["filename"].encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
        digest.update(size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(package["checksum"]))
        total_bytes += size
    identity = {
        "cargo_lock_sha256": hashlib.sha256(lock_bytes).hexdigest(),
        "registry": "https://github.com/rust-lang/crates.io-index",
        "archives": len(records),
        "bytes": total_bytes,
        "sha256": digest.hexdigest(),
    }
    return records, identity


def _extract_verified_crate(package, archive, vendor_root):
    expected_dirname = f"{package['name']}-{package['version']}"
    destination = os.path.join(vendor_root, expected_dirname)
    if os.path.exists(destination):
        raise RuntimeError(f"duplicate vendored Cargo package directory: {destination}")
    root = os.path.realpath(vendor_root)
    count = 0
    total_bytes = 0
    with tarfile.open(archive, "r:gz") as tf:
        members = tf.getmembers()
        for member in members:
            normalized = member.name.replace("\\", "/")
            if not normalized.startswith(expected_dirname + "/"):
                raise RuntimeError(f"crate {archive} has unexpected root path {member.name!r}")
            target = os.path.realpath(os.path.join(root, *normalized.split("/")))
            try:
                contained = os.path.commonpath((root, target)) == root
            except ValueError:
                contained = False
            if (not contained or member.issym() or member.islnk()
                    or not (member.isdir() or member.isfile())):
                raise RuntimeError(f"unsafe/special path in crate {archive}: {member.name!r}")
            if member.isfile():
                count += 1
                total_bytes += member.size
                if count > 20000 or total_bytes > 512 * 1024 * 1024:
                    raise RuntimeError(f"crate extraction bounds exceeded for {archive}")
        try:
            tf.extractall(root, filter="fully_trusted")
        except TypeError:
            tf.extractall(root)

    files = {}
    for current, dirs, names in os.walk(destination, followlinks=False):
        dirs.sort()
        names.sort()
        if any(os.path.islink(os.path.join(current, name)) for name in dirs):
            raise RuntimeError(f"vendored crate contains a directory link: {destination}")
        for name in names:
            path = os.path.join(current, name)
            if os.path.islink(path) or not os.path.isfile(path):
                raise RuntimeError(f"vendored crate contains a file link: {path}")
            relative = os.path.relpath(path, destination).replace("\\", "/")
            if relative != ".cargo-checksum.json":
                files[relative] = _sha256_file(path)
    _write_json_atomic(os.path.join(destination, ".cargo-checksum.json"), {
        "files": files,
        "package": package["checksum"],
    })


def _prepare_vendored_cargo_home(records):
    cargo_home = os.path.join(NOD_DIR, "_cargo-home-build")
    shutil.rmtree(cargo_home, ignore_errors=True)
    vendor_root = os.path.join(cargo_home, "vendor")
    try:
        os.makedirs(vendor_root)
        for package, archive, _size in records:
            _extract_verified_crate(package, archive, vendor_root)
        config = os.path.join(cargo_home, "config.toml")
        vendor_toml = vendor_root.replace("\\", "/").replace('"', '\\"')
        with open(config, "w", encoding="utf-8", newline="\n") as f:
            f.write('[source.crates-io]\nreplace-with = "mp6-vendored"\n\n')
            f.write('[source.mp6-vendored]\n')
            f.write(f'directory = "{vendor_toml}"\n\n[net]\noffline = true\n')
        return cargo_home
    except Exception:
        shutil.rmtree(cargo_home, ignore_errors=True)
        raise


def _cargo_build_invocation(tools, source_root, cargo_home):
    """Keep Cargo config discovery inside the controlled home, not source parents."""
    work_dir = os.path.join(cargo_home, "work")
    os.makedirs(work_dir, exist_ok=True)
    manifest = os.path.realpath(os.path.join(source_root, "Cargo.toml"))
    if not os.path.isfile(manifest):
        raise RuntimeError(f"pinned Nod source has no Cargo.toml: {manifest}")
    command = [
        tools["cargo"], "build", "--manifest-path", manifest,
        "-p", "nod-ffi", "--release", "--locked", "--offline",
        "--target", ANDROID_RUST_TARGET, "--no-default-features",
        "--features", ANDROID_FEATURES,
    ]
    return command, work_dir


def _android_manifest_path():
    return os.path.join(NOD_DIR, "android-aarch64", ".mp6-nod-android.json")


def _android_build_contract():
    return {
        "cargo_locked": True,
        "cargo_offline": True,
        "cargo_incremental": "0",
        "source_date_epoch": "0",
        "rust_path_remap": "/mp6/nod-source;/mp6/cargo-vendor",
        "c_path_remap": "/mp6/nod-source;/mp6/cargo-vendor",
        "native_env_isolation": "deny-rust-cargo-native-v2",
    }


def check_android_install():
    artifact = os.path.join(NOD_DIR, "android-aarch64", "libnod.a")
    manifest_path = _android_manifest_path()
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        tools = _android_tools()
        toolchain_identity = _android_toolchain_identity(tools)
        source_package = os.path.join(NOD_DIR, "_dl", f"nod-src-{NOD_VERSION}.tar.gz")
        lock_bytes = _source_cargo_lock(source_package)
        _records, cargo_inputs = _verified_crate_archives(lock_bytes, allow_download=False)
    except (OSError, ValueError, RuntimeError) as exc:
        return False, f"missing/invalid Android nod build manifest: {exc}"
    if (manifest.get("manifest_version") != ANDROID_MANIFEST_VERSION
            or manifest.get("version") != NOD_VERSION
            or manifest.get("source_sha256") != SOURCE_SHA256
            or manifest.get("features") != ANDROID_FEATURES
            or manifest.get("build_contract") != _android_build_contract()
            or manifest.get("cargo_inputs") != cargo_inputs
            or manifest.get("toolchain") != toolchain_identity):
        return False, f"stale Android nod build manifest: {manifest_path}"
    if not os.path.isfile(artifact) or _sha256_file(artifact) != manifest.get("artifact_sha256"):
        return False, f"Android nod library is missing/stale/corrupt: {artifact}"
    return True, None


def verify_android_install():
    return check_android_install()[0]


def _fetch_android_locked():
    print(f"[nod] android-aarch64 staticlib ({NOD_VERSION}, cargo cross-build)")
    try:
        tools = _android_tools()
        toolchain_identity = _android_toolchain_identity(tools)
    except RuntimeError as exc:
        print(f"[FATAL] {exc}")
        return 1

    dl = os.path.join(NOD_DIR, "_dl")
    source_package = os.path.join(dl, f"nod-src-{NOD_VERSION}.tar.gz")
    _download(SOURCE_URL, source_package, SOURCE_SHA256)
    extract_stage = os.path.join(dl, "source-build")
    shutil.rmtree(extract_stage, ignore_errors=True)
    _safe_extract(source_package, extract_stage)
    source_root = os.path.join(extract_stage, f"nod-{NOD_VERSION.lstrip('v')}")
    if not os.path.isdir(source_root):
        print(f"[FATAL] source extraction did not produce {source_root}")
        shutil.rmtree(extract_stage, ignore_errors=True)
        return 1

    lock_path = os.path.join(source_root, "Cargo.lock")
    try:
        with open(lock_path, "rb") as f:
            lock_bytes = f.read()
        records, cargo_inputs = _verified_crate_archives(lock_bytes, allow_download=True)
        cargo_home_stage = _prepare_vendored_cargo_home(records)
    except (OSError, RuntimeError, tarfile.TarError) as exc:
        print(f"[FATAL] cannot prepare checksum-verified Cargo vendor tree: {exc}")
        shutil.rmtree(extract_stage, ignore_errors=True)
        return 1

    cargo_target_stage = os.path.join(NOD_DIR, "_cargo-target-build")
    shutil.rmtree(cargo_target_stage, ignore_errors=True)
    env = _rust_environment(toolchain_identity["rust"]["active_toolchain"])
    env["CARGO_HOME"] = cargo_home_stage
    env["CARGO_TARGET_DIR"] = cargo_target_stage
    env["CARGO_INCREMENTAL"] = "0"
    env["SOURCE_DATE_EPOCH"] = "0"
    source_remap = f"--remap-path-prefix={os.path.realpath(source_root)}=/mp6/nod-source"
    vendor_remap = (
        f"--remap-path-prefix={os.path.realpath(os.path.join(cargo_home_stage, 'vendor'))}="
        "/mp6/cargo-vendor"
    )
    env["CARGO_ENCODED_RUSTFLAGS"] = source_remap + "\x1f" + vendor_remap
    env["CFLAGS_aarch64_linux_android"] = (
        f"-ffile-prefix-map={os.path.realpath(source_root)}=/mp6/nod-source "
        f"-ffile-prefix-map={os.path.realpath(os.path.join(cargo_home_stage, 'vendor'))}="
        "/mp6/cargo-vendor"
    )
    env["CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"] = tools["target_cc"]
    env["CC_aarch64_linux_android"] = tools["target_cc"]
    env["AR_aarch64_linux_android"] = tools["ar"]
    env["RANLIB_aarch64_linux_android"] = tools["ranlib"]
    cmd, cargo_work_dir = _cargo_build_invocation(
        tools, source_root, cargo_home_stage
    )
    print("  " + " ".join(cmd))
    try:
        proc = subprocess.run(cmd, cwd=cargo_work_dir, env=env)
        if proc.returncode != 0:
            print("[FATAL] cargo build failed")
            return 1

        built = os.path.join(cargo_target_stage, ANDROID_RUST_TARGET, "release", "libnod.a")
        if not os.path.isfile(built):
            print(f"[FATAL] cargo build produced no {built}")
            return 1
        out = os.path.join(NOD_DIR, "android-aarch64")
        os.makedirs(out, exist_ok=True)
        artifact = os.path.join(out, "libnod.a")
        _copy_file_atomic(built, artifact)
        manifest = {
            "manifest_version": ANDROID_MANIFEST_VERSION,
            "version": NOD_VERSION,
            "source_sha256": SOURCE_SHA256,
            "features": ANDROID_FEATURES,
            "build_contract": _android_build_contract(),
            "cargo_inputs": cargo_inputs,
            "toolchain": toolchain_identity,
            "artifact_sha256": _sha256_file(artifact),
        }
        _write_json_atomic(_android_manifest_path(), manifest)
        ok, problem = check_android_install()
        if not ok:
            print(f"[FATAL] {problem}")
            return 1
        print(f"  -> {artifact} ({os.path.getsize(artifact) / 1e6:.1f}MB; manifest verified)")
        return 0
    finally:
        shutil.rmtree(extract_stage, ignore_errors=True)
        shutil.rmtree(cargo_home_stage, ignore_errors=True)
        shutil.rmtree(cargo_target_stage, ignore_errors=True)


def fetch_android():
    os.makedirs(NOD_DIR, exist_ok=True)
    lock_path = os.path.join(NOD_DIR, ".mp6-nod-android-build.lock")
    token = str(os.getpid())
    try:
        fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        print(f"[FATAL] another Android Nod build owns {lock_path}")
        return 1
    try:
        with os.fdopen(fd, "w", encoding="ascii") as f:
            f.write(token + "\n")
        return _fetch_android_locked()
    finally:
        try:
            with open(lock_path, "r", encoding="ascii") as f:
                owned = f.read().strip() == token
            if owned:
                os.remove(lock_path)
        except FileNotFoundError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", action="store_true", help="install the package for this host")
    ap.add_argument("--windows", action="store_true", help="install the Windows x86_64 package")
    ap.add_argument("--android", action="store_true", help="cross-build the Android aarch64 staticlib")
    args = ap.parse_args()
    default = not (args.host or args.windows or args.android)

    os.makedirs(NOD_DIR, exist_ok=True)
    try:
        fetch_licenses()
        if args.host or default:
            fetch_host()
        if args.windows:
            fetch_windows()
        if args.android or (default and os.name == "nt"):
            return fetch_android() or 0
    except (OSError, RuntimeError, tarfile.TarError) as exc:
        print(f"[FATAL] {exc}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
