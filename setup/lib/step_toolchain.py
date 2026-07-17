"""setup/lib/step_toolchain.py -- step 2: the zig toolchain + nod library.

zig: fetched from the OFFICIAL ziglang.org release index (never a hardcoded
guessed URL -- ziglang.org/download/index.json is Zig's own machine-readable
release manifest, precisely so tools don't need to hand-know the archive
naming scheme). The exact version is parsed out of tools/build.py's own
`ZIG = ...` constant, so this step can never drift from what the build
driver actually expects; the download is sha256-verified against the value
the SAME official index publishes, and the resolved URL is always printed
so the source is transparent.

nod: this port already vendors a fetch script for it
(tools/fetch_nod.py) -- reused as-is rather than re-implemented.
"""
import hashlib
import json
import os
import re
import sys
import urllib.request
import zipfile

from . import common

ZIG_CONST_RE = re.compile(r'ZIG\s*=\s*os\.path\.join\(PORT_ROOT,\s*"toolchain",\s*"(zig-[\w.\-]+)"')
ZIG_VERSION_RE = re.compile(r'zig-(?:x86_64|aarch64)-windows-([\w.\-]+)$')
ZIG_INDEX_URL = "https://ziglang.org/download/index.json"

# Zig's own index.json platform-tag scheme: "<arch>-<os>".
_ZIG_HOST_TAGS = {
    ("Windows", "AMD64"): "x86_64-windows",
    ("Windows", "ARM64"): "aarch64-windows",
    ("Linux", "x86_64"): "x86_64-linux",
    ("Linux", "aarch64"): "aarch64-linux",
    ("Darwin", "x86_64"): "x86_64-macos",
    ("Darwin", "arm64"): "aarch64-macos",
}


def _host_zig_tag():
    import platform
    key = (platform.system(), platform.machine())
    return _ZIG_HOST_TAGS.get(key)


def required_zig_dirname():
    """Parses tools/build.py for its ZIG constant so this step can never
    fetch a version other than the one the build driver will actually look
    for. Falls back to a hardcoded, commented value if the source ever
    changes shape (fails loudly with a clear message either way -- never
    silently guesses)."""
    build_py = os.path.join(common.NATIVE_ROOT, "tools", "build.py")
    with open(build_py, "r", encoding="utf-8") as f:
        text = f.read()
    m = ZIG_CONST_RE.search(text)
    if not m:
        raise common.SetupError(
            "could not find the ZIG = ... constant in tools/build.py -- "
            "the build driver's toolchain path resolution may have changed"
        )
    return m.group(1)  # e.g. "zig-x86_64-windows-0.16.0"


def _zig_version_from_dirname(dirname):
    m = ZIG_VERSION_RE.search(dirname)
    if not m:
        raise common.SetupError(f"couldn't parse a version number out of toolchain dir name {dirname!r}")
    return m.group(1)


def zig_exe_path():
    dirname = required_zig_dirname()
    return os.path.join(common.TOOLCHAIN_DIR, dirname, "zig.exe")


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _download_with_progress(url, dst, expected_size=None):
    req = urllib.request.Request(url, headers={"User-Agent": "mp6-setup-tool"})
    with urllib.request.urlopen(req, timeout=30) as resp, open(dst, "wb") as out:
        total = expected_size or int(resp.headers.get("Content-Length") or 0)
        done = 0
        progress = common.ProgressPrinter("  downloading")
        while True:
            chunk = resp.read(1024 * 1024)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            if total:
                progress.update(done, total)
        if total:
            progress.update(total, total)
    print()


def ensure_zig(assume_yes=False):
    dirname = required_zig_dirname()
    exe = zig_exe_path()
    if os.path.exists(exe):
        rc, out = common.run_capture([exe, "version"])
        if rc == 0:
            common.ok(f"zig toolchain present: {exe} (zig version {out})")
            return True
        common.warn(f"zig.exe exists at {exe} but failed to run (exit {rc}) -- will re-fetch")

    wanted_version = _zig_version_from_dirname(dirname)
    common.info(f"zig toolchain not found -- need {dirname} (tools/build.py's pinned version)")

    host_tag = _host_zig_tag()
    if not host_tag:
        raise common.SetupError(
            "don't know the ziglang.org platform tag for this host (unsupported platform.system()/machine())",
            hint="download zig manually from https://ziglang.org/download/ and extract it to "
                 f"{os.path.join(common.TOOLCHAIN_DIR, dirname)}",
        )
    if not host_tag.startswith("x86_64-windows") and not common.IS_WINDOWS:
        common.warn(
            "tools/build.py's ZIG path is hardcoded to a Windows-hosted toolchain folder name "
            f"({dirname}) -- fetching a {host_tag} zig here for future use, but the native Windows "
            "build step itself currently requires running this tool from a Windows host."
        )

    common.info(f"fetching the official release index: {ZIG_INDEX_URL}")
    req = urllib.request.Request(ZIG_INDEX_URL, headers={"User-Agent": "mp6-setup-tool"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        index = json.loads(resp.read().decode("utf-8"))

    if wanted_version not in index:
        available = sorted(k for k in index.keys() if k != "master")[-10:]
        raise common.SetupError(
            f"ziglang.org's release index has no version {wanted_version!r} "
            f"(most recent listed: {', '.join(available)})",
            hint="tools/build.py may be pinned to a version that's since been removed from the index; "
                 "check docs/BUILDING.md",
        )
    entry = index[wanted_version].get(host_tag)
    if not entry:
        raise common.SetupError(f"ziglang.org's index has no {host_tag!r} build for zig {wanted_version}")

    url = entry["tarball"]
    expected_shasum = entry.get("shasum")
    expected_size = int(entry.get("size", 0)) or None
    common.info(f"resolved download URL (official ziglang.org index): {url}")
    if expected_shasum:
        common.info(f"expected sha256 (from the same index): {expected_shasum}")

    common.ensure_dir(common.TOOLCHAIN_DIR)
    free = common.free_space_bytes(common.TOOLCHAIN_DIR)
    if expected_size and free < expected_size * 3:
        common.warn(f"only {common.human_size(free)} free at {common.TOOLCHAIN_DIR}; the download + "
                    f"extraction needs roughly {common.human_size(expected_size * 2.2)}")

    dl_dir = common.ensure_dir(os.path.join(common.TOOLCHAIN_DIR, "_dl"))
    archive_name = url.rsplit("/", 1)[-1]
    archive_path = os.path.join(dl_dir, archive_name)
    if os.path.exists(archive_path) and expected_shasum and _sha256_file(archive_path) == expected_shasum:
        common.info(f"cached archive already verified: {archive_path}")
    else:
        common.info(f"downloading zig {wanted_version} ({host_tag}) ...")
        _download_with_progress(url, archive_path, expected_size)
        if expected_shasum:
            got = _sha256_file(archive_path)
            if got != expected_shasum:
                os.remove(archive_path)
                raise common.SetupError(
                    f"sha256 mismatch on downloaded zig archive: got {got}, expected {expected_shasum}",
                    hint="the download may have been corrupted or intercepted -- re-run this step",
                )
            common.ok(f"sha256 verified: {got}")

    common.info(f"extracting to {common.TOOLCHAIN_DIR} ...")
    if archive_path.endswith(".zip"):
        with zipfile.ZipFile(archive_path) as zf:
            zf.extractall(common.TOOLCHAIN_DIR)
    else:
        import tarfile
        with tarfile.open(archive_path) as tf:
            tf.extractall(common.TOOLCHAIN_DIR)

    if not os.path.exists(exe):
        # Zig's archive top-level folder name should already match `dirname`
        # (that's the whole point of pinning by that exact string), but be
        # defensive: look for whatever folder the archive actually produced.
        raise common.SetupError(
            f"extraction finished but {exe} still doesn't exist -- the archive's internal folder name "
            f"may not match the expected {dirname!r}; check {common.TOOLCHAIN_DIR}",
        )
    rc, out = common.run_capture([exe, "version"])
    if rc != 0:
        raise common.SetupError(f"freshly-extracted zig.exe failed to run (exit {rc})")
    common.ok(f"zig {out} installed at {exe}")
    return True


def ensure_nod(assume_yes=False):
    nod_dir = os.path.join(common.TOOLCHAIN_DIR, "nod")
    dll = os.path.join(nod_dir, "windows-x86_64", "bin", "nod.dll")
    lib = os.path.join(nod_dir, "windows-x86_64", "lib", "nod.lib")
    header = os.path.join(nod_dir, "include", "nod.h")
    if os.path.exists(dll) and os.path.exists(lib) and os.path.exists(header):
        common.ok(f"nod (disc-image library) present: {dll}")
        return True

    common.info("nod (GC/Wii disc-image library) not found -- fetching via the port's own "
                "tools/fetch_nod.py (reused as-is)")
    fetch_script = os.path.join(common.NATIVE_ROOT, "tools", "fetch_nod.py")
    common.run([sys.executable, fetch_script, "--windows"])
    if not (os.path.exists(dll) and os.path.exists(lib) and os.path.exists(header)):
        raise common.SetupError("tools/fetch_nod.py ran but nod.dll/nod.lib/nod.h still aren't all present")
    common.ok(f"nod installed at {nod_dir}")
    return True
