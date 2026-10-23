"""Shared helpers for tests that read Aurora's port-modified source.

Aurora is vendored in third_party/aurora with every port change applied, so
the source a test wants is simply the file there. The former patch queue is
history: a request for the source *before* a given patch now skips."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build


ROOT = Path(__file__).resolve().parents[1]
AURORA_BASE_PATCHES = ROOT / "compat/aurora/base"  # removed; kept for old callers
AURORA_SOURCE = ROOT / "third_party/aurora"


def vendored(target: str) -> str:
    """Current port-modified Aurora source of *target* (e.g. lib/gx/gx.cpp)."""
    return (AURORA_SOURCE / target).read_text(encoding="utf-8", errors="surrogateescape")


def git_executable() -> str:
    """Return Git from PATH, or skip when this optional replay dependency is absent."""

    executable = shutil.which("git")
    if executable is None:
        raise unittest.SkipTest(
            "Aurora replay requires Git on PATH; install Git or add git.exe to PATH"
        )
    return executable


def git_command(*args: object) -> list[str]:
    """Build a Git command using the PATH-resolved executable."""

    return [git_executable(), *(str(arg) for arg in args)]


def pinned_source(target: str) -> str:
    """Read *target* from the documented Aurora pin without a local path assumption."""

    executable = git_executable()
    aurora_root = Path(build.AURORA_ROOT)
    if not aurora_root.is_dir():
        raise unittest.SkipTest(
            f"Aurora replay source is unavailable at {aurora_root}; run the Aurora setup step"
        )
    pin = step_aurora.read_aurora_pin()
    result = subprocess.run(
        [executable, "-C", str(aurora_root), "show", f"{pin}:{target}"],
        capture_output=True,
        check=True,
        timeout=20,
    )
    return result.stdout.decode("utf-8", "surrogateescape")


def replay(
    target: str,
    *,
    stop_before: str | None = None,
    stop_after: str | None = None,
    patch_dir: Path = AURORA_BASE_PATCHES,
) -> str:
    """The port-modified source of *target*.

    ``stop_before`` asked for the source before a patch of the former queue;
    that state is history now, so the test skips. ``stop_after`` and the
    unbounded form both return the current vendored source."""

    if stop_before is not None:
        raise unittest.SkipTest(
            f"pre-{stop_before} source is history: Aurora is vendored in third_party/aurora"
        )
    return vendored(target)
