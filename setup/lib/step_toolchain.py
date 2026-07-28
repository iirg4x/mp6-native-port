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
import shutil
import stat
import sys
import tarfile
import urllib.request
import zipfile

from . import common

ZIG_CONST_RE = re.compile(r'ZIG\s*=\s*os\.path\.join\(PORT_ROOT,\s*"toolchain",\s*"(zig-[\w.\-]+)"')
ZIG_VERSION_RE = re.compile(r'zig-(?:x86_64|aarch64)-windows-([\w.\-]+)$')
ZIG_INDEX_URL = "https://ziglang.org/download/index.json"
ZIG_INSTALL_MANIFEST_VERSION = 2
ZIG_TREE_MAX_FILES = 100_000
ZIG_TREE_MAX_BYTES = 4 * 1024 * 1024 * 1024
_ZIG_TREE_EXCLUDED_DIRS = {".cache", ".zig-cache", "zig-cache"}

# Release-index values copied into the port beside the version pin. The live
# official index is still consulted before a download, but an existing local
# install can now be authenticated without network access and a compromised
# or unexpectedly changed index cannot silently bless different bytes.
PINNED_ZIG_RELEASES = {
    ("0.16.0", "x86_64-windows"): {
        "url": "https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip",
        "sha256": "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e",
        "size": 97217739,
    },
}

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


def _zig_manifest_path(dirname):
    return os.path.join(common.TOOLCHAIN_DIR, dirname, ".mp6-zig-toolchain.json")


def zig_tree_fingerprint(root):
    """Hash every regular installed Zig file except mutable local caches/our manifest."""
    root = os.path.realpath(root)
    digest = hashlib.sha256(b"mp6-zig-install-tree-v1\0")
    file_count = 0
    byte_count = 0
    for current, dirnames, filenames in os.walk(root):
        kept_dirs = []
        for name in sorted(dirnames):
            path = os.path.join(current, name)
            if name in _ZIG_TREE_EXCLUDED_DIRS:
                continue
            if os.path.islink(path):
                raise common.SetupError(f"Zig install contains an unexpected directory link: {path}")
            kept_dirs.append(name)
        dirnames[:] = kept_dirs
        for name in sorted(filenames):
            path = os.path.join(current, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel == ".mp6-zig-toolchain.json" or rel.startswith(".mp6-zig-toolchain.json.part-"):
                continue
            info = os.stat(path, follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                raise common.SetupError(f"Zig install contains an unexpected non-file entry: {path}")
            file_count += 1
            byte_count += info.st_size
            if file_count > ZIG_TREE_MAX_FILES or byte_count > ZIG_TREE_MAX_BYTES:
                raise common.SetupError(
                    "Zig install tree exceeds the 100,000-file / 4 GiB verification bound"
                )
            rel_bytes = rel.encode("utf-8")
            digest.update(len(rel_bytes).to_bytes(4, "big"))
            digest.update(rel_bytes)
            digest.update(info.st_size.to_bytes(8, "big"))
            digest.update(bytes.fromhex(_sha256_file(path)))
    return {
        "algorithm": "sha256-path-size-content-v1",
        "files": file_count,
        "bytes": byte_count,
        "sha256": digest.hexdigest(),
    }


def _write_json_atomic(path, value):
    tmp = f"{path}.part-{os.getpid()}"
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


def _check_zig_install(dirname, wanted_version, host_tag, release):
    exe = os.path.join(common.TOOLCHAIN_DIR, dirname, "zig.exe")
    if not os.path.isfile(exe):
        return False, f"missing {exe}"
    try:
        with open(_zig_manifest_path(dirname), "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        return False, f"missing/invalid Zig install manifest: {exc}"
    try:
        tree = zig_tree_fingerprint(os.path.dirname(exe))
    except (OSError, common.SetupError) as exc:
        return False, f"could not fingerprint the complete Zig install: {exc}"
    expected = {
        "manifest_version": ZIG_INSTALL_MANIFEST_VERSION,
        "version": wanted_version,
        "host": host_tag,
        "archive_url": release["url"],
        "archive_sha256": release["sha256"],
        "zig_exe_sha256": _sha256_file(exe),
        "tree": tree,
    }
    if manifest != expected:
        return False, "Zig install manifest or full-tree digest is stale"
    rc, out = common.run_capture([exe, "version"])
    if rc != 0 or out != wanted_version:
        return False, f"zig version probe returned {out!r} (exit {rc}), expected {wanted_version}"
    return True, None


def verified_required_zig_tree():
    """Verify the pinned install and return its full-tree identity for build stamps."""
    dirname = required_zig_dirname()
    wanted_version = _zig_version_from_dirname(dirname)
    host_tag = _host_zig_tag()
    release = PINNED_ZIG_RELEASES.get((wanted_version, host_tag))
    if release is None:
        raise common.SetupError(f"no archive digest is pinned for Zig {wanted_version} on {host_tag}")
    valid, problem = _check_zig_install(dirname, wanted_version, host_tag, release)
    if not valid:
        raise common.SetupError(problem)
    with open(_zig_manifest_path(dirname), "r", encoding="utf-8") as f:
        return json.load(f)["tree"]


def _safe_extract_zig(archive_path, extract_root):
    root = os.path.realpath(extract_root)
    common.ensure_dir(root)
    if archive_path.endswith(".zip"):
        with zipfile.ZipFile(archive_path) as zf:
            for info in zf.infolist():
                target = os.path.realpath(os.path.join(root, info.filename))
                if os.path.commonpath((root, target)) != root:
                    raise common.SetupError(f"unsafe path in Zig archive: {info.filename!r}")
            zf.extractall(root)
    else:
        with tarfile.open(archive_path) as tf:
            for member in tf.getmembers():
                target = os.path.realpath(os.path.join(root, member.name))
                if (os.path.commonpath((root, target)) != root
                        or member.issym() or member.islnk()):
                    raise common.SetupError(f"unsafe path/link in Zig archive: {member.name!r}")
            try:
                tf.extractall(root, filter="fully_trusted")
            except TypeError:
                tf.extractall(root)


def ensure_zig(assume_yes=False):
    dirname = required_zig_dirname()
    exe = zig_exe_path()
    wanted_version = _zig_version_from_dirname(dirname)
    host_tag = _host_zig_tag()
    if not host_tag:
        raise common.SetupError(
            "don't know the ziglang.org platform tag for this host (unsupported platform.system()/machine())",
            hint="download zig manually from https://ziglang.org/download/ and extract it to "
                 f"{os.path.join(common.TOOLCHAIN_DIR, dirname)}",
        )
    release = PINNED_ZIG_RELEASES.get((wanted_version, host_tag))
    if release is None:
        raise common.SetupError(
            f"no archive digest is pinned for Zig {wanted_version} on {host_tag}",
            hint="update PINNED_ZIG_RELEASES in setup/lib/step_toolchain.py when changing the Zig pin",
        )
    valid, problem = _check_zig_install(dirname, wanted_version, host_tag, release)
    if valid:
        common.ok(f"zig toolchain present + digest verified: {exe} (zig {wanted_version})")
        return True
    if os.path.exists(exe):
        common.warn(f"existing Zig install is not trusted ({problem}) -- restoring the pinned archive")
    else:
        common.info(f"zig toolchain not found -- need {dirname} (tools/build.py's pinned version)")

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

    live = {
        "url": entry["tarball"],
        "sha256": entry.get("shasum"),
        "size": int(entry.get("size", 0)) or None,
    }
    if live != release:
        raise common.SetupError(
            f"ziglang.org index no longer matches this port's pinned Zig manifest: "
            f"live={live!r}, pinned={release!r}",
            hint="do not accept new toolchain bytes implicitly; review and update the pin explicitly",
        )
    url = release["url"]
    expected_shasum = release["sha256"]
    expected_size = release["size"]
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
        part = archive_path + ".part"
        try:
            _download_with_progress(url, part, expected_size)
            got = _sha256_file(part)
            if got != expected_shasum:
                raise common.SetupError(
                    f"sha256 mismatch on downloaded zig archive: got {got}, expected {expected_shasum}",
                    hint="the download may have been corrupted or intercepted -- re-run this step",
                )
            os.replace(part, archive_path)
        finally:
            try:
                os.remove(part)
            except FileNotFoundError:
                pass
        if expected_shasum:
            got = _sha256_file(archive_path)
            if got != expected_shasum:
                os.remove(archive_path)
                raise common.SetupError(
                    f"sha256 mismatch on downloaded zig archive: got {got}, expected {expected_shasum}",
                    hint="the download may have been corrupted or intercepted -- re-run this step",
                )
            common.ok(f"sha256 verified: {got}")

    common.info(f"extracting exact toolchain tree to {common.TOOLCHAIN_DIR} ...")
    extract_root = os.path.join(common.TOOLCHAIN_DIR, f".zig-extract-{os.getpid()}")
    shutil.rmtree(extract_root, ignore_errors=True)
    _safe_extract_zig(archive_path, extract_root)
    extracted = os.path.join(extract_root, dirname)
    destination = os.path.join(common.TOOLCHAIN_DIR, dirname)
    previous = destination + f".previous-{os.getpid()}"
    if not os.path.isdir(extracted):
        shutil.rmtree(extract_root, ignore_errors=True)
        raise common.SetupError(f"Zig archive did not contain expected top-level directory {dirname!r}")
    shutil.rmtree(previous, ignore_errors=True)
    moved_old = False
    try:
        if os.path.exists(destination):
            os.replace(destination, previous)
            moved_old = True
        os.replace(extracted, destination)
    except Exception:
        if moved_old and not os.path.exists(destination) and os.path.exists(previous):
            os.replace(previous, destination)
        raise
    finally:
        shutil.rmtree(extract_root, ignore_errors=True)
    shutil.rmtree(previous, ignore_errors=True)

    if not os.path.exists(exe):
        # Zig's archive top-level folder name should already match `dirname`
        # (that's the whole point of pinning by that exact string), but be
        # defensive: look for whatever folder the archive actually produced.
        raise common.SetupError(
            f"extraction finished but {exe} still doesn't exist -- the archive's internal folder name "
            f"may not match the expected {dirname!r}; check {common.TOOLCHAIN_DIR}",
        )
    rc, out = common.run_capture([exe, "version"])
    if rc != 0 or out != wanted_version:
        raise common.SetupError(
            f"freshly-extracted zig.exe returned {out!r} (exit {rc}), expected {wanted_version}"
        )
    _write_json_atomic(_zig_manifest_path(dirname), {
        "manifest_version": ZIG_INSTALL_MANIFEST_VERSION,
        "version": wanted_version,
        "host": host_tag,
        "archive_url": release["url"],
        "archive_sha256": release["sha256"],
        "zig_exe_sha256": _sha256_file(exe),
        "tree": zig_tree_fingerprint(os.path.dirname(exe)),
    })
    valid, problem = _check_zig_install(dirname, wanted_version, host_tag, release)
    if not valid:
        raise common.SetupError(f"fresh Zig install failed verification: {problem}")
    common.ok(f"zig {out} installed with verified digest at {exe}")
    return True


def ensure_nod(assume_yes=False):
    tools_dir = os.path.join(common.NATIVE_ROOT, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import fetch_nod

    ok, problem = fetch_nod.check_host_install()
    if ok:
        spec = fetch_nod.host_spec()
        common.ok(f"nod {fetch_nod.NOD_VERSION} present + digest verified: {spec['id']}")
        return True

    common.info(f"nod host library is missing/stale ({problem}) -- fetching the pinned package")
    fetch_script = os.path.join(common.NATIVE_ROOT, "tools", "fetch_nod.py")
    common.run([sys.executable, fetch_script, "--host"])
    ok, problem = fetch_nod.check_host_install()
    if not ok:
        raise common.SetupError(f"tools/fetch_nod.py ran but host nod verification failed: {problem}")
    common.ok(f"nod {fetch_nod.NOD_VERSION} installed with verified artifact digests")
    return True
