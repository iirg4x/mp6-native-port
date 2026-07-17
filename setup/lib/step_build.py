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
"""
import os
import shutil
import sys

from . import common


def _run_build_py(native_root, extra_args, jobs=None):
    build_py = os.path.join(native_root, "tools", "build.py")
    cmd = [sys.executable, build_py] + list(extra_args)
    if jobs:
        cmd += ["-j", str(jobs)]
    common.run(cmd, cwd=native_root)


def build(native_root, headless=True, windowed=True, jobs=None, coro_fibers=False):
    if windowed:
        common.info("building the windowed exe (links Aurora) -- build/mp6native.exe")
        extra = ["--coro-fibers"] if coro_fibers else []
        _run_build_py(native_root, extra, jobs=jobs)
        exe = os.path.join(native_root, "build", f"mp6native{'_corofib' if coro_fibers else ''}.exe")
        if not os.path.exists(exe):
            raise common.SetupError(f"build.py reported success but {exe} doesn't exist")
        common.ok(f"built {exe}")

    if headless:
        common.info("building the headless exe (no Aurora/SDL in the link) -- build/mp6native_headless.exe")
        extra = ["--headless"] + (["--coro-fibers"] if coro_fibers else [])
        _run_build_py(native_root, extra, jobs=jobs)
        exe = os.path.join(native_root, "build", f"mp6native_headless{'_corofib' if coro_fibers else ''}.exe")
        if not os.path.exists(exe):
            raise common.SetupError(f"build.py reported success but {exe} doesn't exist")
        common.ok(f"built {exe}")


def assemble_dist(native_root, dist_dir=None):
    """Harvests build/ into a flat, runnable dist/ folder. Returns the dist
    dir path."""
    build_dir = os.path.join(native_root, "build")
    dist_dir = dist_dir or os.path.join(native_root, "dist")
    common.ensure_dir(dist_dir)

    copied = []
    for name in os.listdir(build_dir):
        src = os.path.join(build_dir, name)
        if not os.path.isfile(src):
            continue
        ext = os.path.splitext(name)[1].lower()
        if ext in (".exe", ".dll", ".pdb"):
            dst = os.path.join(dist_dir, name)
            shutil.copy2(src, dst)
            copied.append(name)

    res_src = os.path.join(build_dir, "res")
    res_dst = os.path.join(dist_dir, "res")
    if os.path.isdir(res_src):
        if os.path.isdir(res_dst):
            shutil.rmtree(res_dst)
        shutil.copytree(res_src, res_dst)
        copied.append("res/")

    if not copied:
        raise common.SetupError(f"nothing to assemble into dist/ -- {build_dir} has no exe/dll/res output yet")

    common.ok(f"assembled dist/: {', '.join(sorted(copied))}")
    return dist_dir
