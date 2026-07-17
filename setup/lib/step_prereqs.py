"""setup/lib/step_prereqs.py -- step 1: prerequisite check.

Checks for git, python3, cmake, ninja, and reports what's missing with
install hints. Never silently continues past a HARD requirement -- but
"cmake" (only needed for the rare from-scratch Aurora build) is a SOFT
requirement, reported as a warning rather than a hard failure, since most
runs reuse an already-built Aurora tree and never touch CMake at all.
"""
import platform
import sys

from . import common

MIN_PYTHON = (3, 8)


class Requirement:
    def __init__(self, name, hard, check_fn, hint_windows, hint_other):
        self.name = name
        self.hard = hard
        self.check_fn = check_fn
        self.hint_windows = hint_windows
        self.hint_other = hint_other


def _check_git():
    path = common.which("git")
    if not path:
        return False, None
    rc, out = common.run_capture(["git", "--version"])
    return rc == 0, out


def _check_cmake():
    path = common.which("cmake")
    if not path:
        return False, None
    rc, out = common.run_capture(["cmake", "--version"])
    return rc == 0, out.splitlines()[0] if out else out


def _check_ninja():
    path = common.which("ninja")
    if not path:
        return False, None
    rc, out = common.run_capture(["ninja", "--version"])
    return rc == 0, out


def _check_python():
    ok = sys.version_info[:2] >= MIN_PYTHON
    return ok, platform.python_version()


REQUIREMENTS = [
    Requirement(
        "git", True, _check_git,
        "winget install --id Git.Git -e  (or https://git-scm.com/download/win)",
        "apt install git  /  brew install git",
    ),
    Requirement(
        "python3 (>= 3.8)", True, _check_python,
        "https://www.python.org/downloads/ or the Microsoft Store 'Python 3' package",
        "apt install python3  /  brew install python3",
    ),
    Requirement(
        "ninja", True, _check_ninja,
        "pip install ninja  (or winget install Ninja-build.Ninja)",
        "apt install ninja-build  /  brew install ninja",
    ),
    Requirement(
        "cmake", False, _check_cmake,
        "winget install --id Kitware.CMake -e  (or https://cmake.org/download/)",
        "apt install cmake  /  brew install cmake",
    ),
]


def check(assume_yes=False):
    """Returns True if every HARD requirement is satisfied. Prints a full
    report either way (soft/optional tools are reported too, just don't
    fail the gate)."""
    all_hard_ok = True
    for req in REQUIREMENTS:
        try:
            found, detail = req.check_fn()
        except Exception as exc:  # noqa: BLE001 -- a probe must never crash setup
            found, detail = False, str(exc)
        tag = "required" if req.hard else "optional -- only for building Aurora from scratch"
        if found:
            common.ok(f"{req.name}: {detail or 'found'}")
        else:
            if req.hard:
                common.error(f"{req.name}: NOT FOUND ({tag})")
                all_hard_ok = False
            else:
                common.warn(f"{req.name}: not found ({tag})")
            install_hint = req.hint_windows if common.IS_WINDOWS else req.hint_other
            common.hint(f"install: {install_hint}")

    if not all_hard_ok:
        common.error("one or more REQUIRED tools are missing -- install them and re-run this tool")
        return False

    common.ok("all required prerequisites present")
    return True
