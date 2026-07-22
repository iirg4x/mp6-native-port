"""setup/lib/step_build.py -- step 5: build + assemble dist/.

Drives tools/build.py (the ONLY build path -- see its own docstring on why
there's no CMake step for the game itself) as a subprocess, for both the
windowed (default, links Aurora) and --headless (CI/automation, no Aurora
anywhere in the link) modes, then assembles a self-contained dist/ folder:
the exe(s), whatever runtime DLLs build.py staged next to them, and res/
(fonts + RmlUi stylesheets for the launcher UI). tools/build.py already
copies the Aurora runtime DLLs + res/ next to build/mp6native.exe itself
(see its own copy_aurora_runtime_dlls() call) -- this step just harvests
whatever ended up in build/ rather than re-deriving that DLL list a second
time, so it can never drift from what build.py actually produces.

dist/ is only ever declared "Done" against an EXACT manifest for the mode the
user selected (required_artifacts()) -- build.py warns rather than fails when a
runtime DLL is missing, so without that check a dist/ could ship an exe that
cannot start. The exes additionally have to be newer than the build that just
ran, so a stale leftover in build/ can never be passed off as this run's
output. Assembly is transactional: everything is staged into a sibling
directory and swapped in at the end, so an interrupted run never leaves a
half-populated dist/ behind.
"""
import os
import shutil
import sys
import time

from . import common


def _run_build_py(native_root, extra_args, jobs=None):
    build_py = os.path.join(native_root, "tools", "build.py")
    cmd = [sys.executable, build_py] + list(extra_args)
    if jobs:
        cmd += ["-j", str(jobs)]
    common.run(cmd, cwd=native_root)


def exe_name(headless, coro_fibers):
    return f"mp6native{'_headless' if headless else ''}{'_corofib' if coro_fibers else ''}.exe"


def required_artifacts(native_root, headless=True, windowed=True, coro_fibers=False):
    """The EXACT file list dist/ must contain for the selected mode, as
    (name-in-build/, kind) pairs. Derived from tools/build.py's own constants
    (AURORA_RUNTIME_DLLS / MP6_ZLIB_DLL), not a second hand-maintained copy, so
    it cannot drift from what the link step actually needs at runtime."""
    tools_dir = os.path.join(native_root, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import build  # tools/build.py

    items = []
    if windowed:
        items.append((exe_name(False, coro_fibers), "exe"))
        for dll in build.AURORA_RUNTIME_DLLS:
            items.append((os.path.basename(dll), "dll"))
        items.append(("res", "dir"))
    if headless:
        items.append((exe_name(True, coro_fibers), "exe"))
        # --headless links real zlib; its DLL has to travel with the exe.
        items.append((os.path.basename(build.MP6_ZLIB_DLL), "dll"))
    seen, out = set(), []
    for name, kind in items:
        if name not in seen:
            seen.add(name)
            out.append((name, kind))
    return out


def build(native_root, headless=True, windowed=True, jobs=None, coro_fibers=False):
    """Runs the requested build(s) and returns the wall-clock instant sampled
    just before the first one -- assemble_dist() uses it to prove the exes it
    ships were produced by THIS run rather than left over from an older one."""
    started = time.time() - 1.0  # -1s: filesystem mtime granularity
    if windowed:
        common.info("building the windowed exe (links Aurora) -- build/mp6native.exe")
        extra = ["--coro-fibers"] if coro_fibers else []
        _run_build_py(native_root, extra, jobs=jobs)
        exe = os.path.join(native_root, "build", exe_name(False, coro_fibers))
        if not os.path.exists(exe):
            raise common.SetupError(f"build.py reported success but {exe} doesn't exist")
        common.ok(f"built {exe}")

    if headless:
        common.info("building the headless exe (no Aurora/SDL in the link) -- build/mp6native_headless.exe")
        extra = ["--headless"] + (["--coro-fibers"] if coro_fibers else [])
        _run_build_py(native_root, extra, jobs=jobs)
        exe = os.path.join(native_root, "build", exe_name(True, coro_fibers))
        if not os.path.exists(exe):
            raise common.SetupError(f"build.py reported success but {exe} doesn't exist")
        common.ok(f"built {exe}")
    return started


def assemble_dist(native_root, dist_dir=None, headless=True, windowed=True, coro_fibers=False,
                  built_after=None):
    """Harvests build/ into a flat, runnable dist/ folder. Fails loudly, listing
    every missing/stale file, rather than shipping a dist/ the user can't run.
    Returns the dist dir path."""
    build_dir = os.path.join(native_root, "build")
    dist_dir = dist_dir or os.path.join(native_root, "dist")

    required = required_artifacts(native_root, headless=headless, windowed=windowed,
                                  coro_fibers=coro_fibers)
    problems = []
    for name, kind in required:
        src = os.path.join(build_dir, name)
        exists = os.path.isdir(src) if kind == "dir" else os.path.isfile(src)
        if not exists:
            problems.append(f"missing: build/{name}"
                            + ("  (build external_refs/repos/aurora, or re-run tools/fetch_nod.py)"
                               if kind == "dll" else ""))
        elif kind == "exe" and built_after is not None and os.path.getmtime(src) < built_after:
            problems.append(f"stale: build/{name} predates this run's build -- it was NOT produced now")
    if problems:
        raise common.SetupError(
            "the build did not produce a complete, runnable set of artifacts:\n    "
            + "\n    ".join(problems),
            hint="dist/ was left untouched. Fix the errors above and re-run; a partially assembled "
                 "dist/ that fails to start is worse than none.")

    # Stage into a sibling first, swap at the end: an interruption (or a copy
    # error partway through) leaves the previous dist/ intact instead of a
    # half-written mixture of two builds.
    staging = dist_dir + f".staging-{os.getpid()}"
    previous = dist_dir + f".previous-{os.getpid()}"
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    common.ensure_dir(staging)
    copied = []
    try:
        for name, kind in required:
            src = os.path.join(build_dir, name)
            dst = os.path.join(staging, name)
            if kind == "dir":
                shutil.copytree(src, dst)
                copied.append(name + "/")
            else:
                shutil.copy2(src, dst)
                copied.append(name)
        # Debug symbols aren't required, but ship them when the build made them.
        for name in os.listdir(build_dir):
            if name.lower().endswith(".pdb") and os.path.isfile(os.path.join(build_dir, name)):
                shutil.copy2(os.path.join(build_dir, name), os.path.join(staging, name))
                copied.append(name)

        if os.path.isdir(dist_dir):
            os.replace(dist_dir, previous)
        os.replace(staging, dist_dir)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        if os.path.isdir(previous) and not os.path.isdir(dist_dir):
            os.replace(previous, dist_dir)  # put the old one back
        raise
    finally:
        shutil.rmtree(previous, ignore_errors=True)

    common.ok(f"assembled dist/ ({len(copied)} item(s)): {', '.join(sorted(copied))}")
    return dist_dir
