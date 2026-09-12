#!/usr/bin/env python3
"""Build the Android release artifact from a reproducible port checkout.

Release builds deliberately fail closed when the port checkout contains
source changes or unexpected ignored files.  Build outputs are allowed to
remain in the checkout because the native and Gradle build steps create them
as part of this command; the source tree itself must still match ``HEAD``.
"""
import argparse
import os
import subprocess
import sys


NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

if NATIVE_ROOT not in sys.path:
    sys.path.insert(0, NATIVE_ROOT)

from setup.lib import common, step_android  # noqa: E402


# These are the generated trees/files documented by docs/RELEASING.md and the
# Android setup/build instructions.  Keep this list path-based: a generated
# file in a build tree is harmless, while an ignored file in a source tree can
# still change what gets compiled or packaged.
_GENERATED_PREFIXES = (
    "build/",
    "packaging/android/.gradle/",
    "packaging/android/app/build/",
    "packaging/android/build/",
    "packaging/android/app/src/main/jnilibs/",
    # Importing the release/build drivers creates these ignored bytecode
    # caches before the first integrity check.  Keep the exception narrow;
    # Python caches elsewhere in the source tree remain unexpected inputs.
    "setup/lib/__pycache__/",
    "tools/__pycache__/",
)
_GENERATED_FILES = {
    "packaging/android/local.properties",
}


def _normalise_status_path(path):
    """Return a Git status path in the portable form used by the allowlist."""
    # Git emits repository-relative paths with forward slashes.  A backslash
    # is a legal filename character on POSIX, so only treat it as a separator
    # on Windows where the checkout itself does.
    # The porcelain separator is removed by the caller.  Preserve any
    # whitespace that is part of the filename; trimming it could turn a
    # source path such as `` build/foo`` into an allowlisted ``build/foo``.
    # Git's path spelling is case-sensitive on POSIX and follows the host
    # filesystem's case rules on Windows.  Do not case-fold a path on POSIX:
    # ``Build/`` is a different source tree from the ignored ``build/``
    # directory there.  Apply ``normcase`` before restoring slash spelling
    # because ntpath.normcase itself changes slash spelling.
    path = os.path.normcase(path)
    if os.name == "nt":
        path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    return path.rstrip("/")


def _is_generated_output(path, root=None):
    directory_record = path.endswith(("/", "\\"))
    path = _normalise_status_path(path)
    candidate = None
    if root is not None:
        candidate = os.path.join(os.fspath(root), *path.split("/"))
        # Generated output trees must not be filesystem redirects. A symlink
        # under an allowlisted name could otherwise make a release step read
        # or overwrite content outside the verified checkout.
        if os.path.islink(candidate):
            return False
    if path in _GENERATED_FILES:
        return True
    if any(path == prefix.rstrip("/") or path.startswith(prefix)
           for prefix in _GENERATED_PREFIXES):
        return True
    # The testing/release gates write their logs alongside the checkout.  Do
    # not extend this to arbitrary subdirectories: an ignored log under res/
    # or another source tree is still an unexpected release input.
    return (
        not directory_record
        and "/" not in path
        and path.endswith(".log")
        and (candidate is None or os.path.isfile(candidate))
    )


def _status_paths(root):
    """Return (paths, problem), including ignored paths outside the allowlist."""
    try:
        proc = subprocess.run(
            [
                "git", "-C", root, "status", "--porcelain=v1",
                "--untracked-files=all", "--ignored=matching",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        return [], f"could not inspect release checkout at {root}: {exc}"
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()
        suffix = f": {detail}" if detail else ""
        return [], f"could not inspect release checkout at {root}{suffix}"

    paths = []
    for line in (proc.stdout or "").splitlines():
        if len(line) < 4:
            # A real porcelain record is at least ``XY `` plus a path.  Do
            # not silently treat malformed output as a clean checkout: that
            # would make a broken or unexpected Git status provider fail open.
            if line:
                paths.append(line)
            continue
        # Porcelain v1 uses two status columns followed by a space.  For a
        # rename, retaining the full ``old -> new`` value gives the caller a
        # useful diagnostic and is conservative with respect to cleanliness.
        status = line[:2]
        path = line[3:]
        # An allowlisted path is safe only when Git says it is an ignored or
        # untracked output.  A tracked file under ``build/`` (or a tracked
        # root-level log) remains a release-integrity failure; otherwise the
        # allowlist would silently hide a checked-in source mutation.
        if status not in {"??", "!!"} or not _is_generated_output(path, root):
            paths.append(path)
    return paths, None


def release_checkout_problem(root=NATIVE_ROOT):
    """Return a release-integrity diagnostic, or ``None`` when it is clean.

    ``git status`` is run with ignored entries enabled.  Ordinary status
    checks miss ignored files, but an ignored file can still be a release
    input (for example ``res/surprise.log``), so only the documented generated
    outputs are exempted.
    """
    root = os.path.realpath(os.fspath(root))
    if not os.path.exists(os.path.join(root, ".git")):
        return f"{root} is not a git checkout"

    paths, problem = _status_paths(root)
    if problem:
        return problem
    if not paths:
        return None
    shown = ", ".join(paths[:5])
    suffix = f" (+{len(paths) - 5} more)" if len(paths) > 5 else ""
    return f"release checkout differs from HEAD: {shown}{suffix}"


def _require_release_checkout(root=NATIVE_ROOT):
    problem = release_checkout_problem(root)
    if problem:
        print(f"[FATAL] {problem}", file=sys.stderr)
        return False
    return True


def _native_release_command():
    return [
        sys.executable,
        os.path.join(NATIVE_ROOT, "tools", "build.py"),
        "--target", "aarch64-android",
        "--windowed",
        "--configuration", "release",
        "--clean",
    ]


def main(argv=None):
    argparse.ArgumentParser(
        description="Build a clean, release-profile Android APK for this checkout."
    ).parse_args(argv)

    if not _require_release_checkout():
        return 1

    command = _native_release_command()
    print("[release-android] building clean release-profile native libraries")
    try:
        proc = subprocess.run(command, cwd=NATIVE_ROOT, check=False)
    except OSError as exc:
        print(f"[FATAL] could not start Android native build: {exc}", file=sys.stderr)
        return 1
    if proc.returncode != 0:
        print(
            f"[FATAL] Android native release build failed (exit {proc.returncode})",
            file=sys.stderr,
        )
        return proc.returncode or 1

    # The native build is expected to create only the generated trees in the
    # allowlist.  Recheck before Gradle reads/stages resources so a build hook
    # or stale ignored source file cannot become APK input after the initial
    # clean-checkout gate.
    if not _require_release_checkout():
        return 1

    print("[release-android] assembling and inspecting release APK")
    try:
        step_android.build_apk(NATIVE_ROOT, variant="release")
    except common.SetupError as exc:
        print(f"[FATAL] Android release APK build failed: {exc.message}", file=sys.stderr)
        if exc.hint:
            print(f"        {exc.hint}", file=sys.stderr)
        return 1

    # Gradle should touch only its own generated trees/local.properties.  A
    # final check keeps the successful release result tied to the same clean
    # source snapshot verified before the build began.
    if not _require_release_checkout():
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
