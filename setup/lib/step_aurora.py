"""setup/lib/step_aurora.py -- Aurora (encounter/aurora) detect-or-build.

Aurora is the GX/VI/PAD/MTX/SI/CARD reimplementation layer + Dawn/SDL3/RmlUi
glue tools/build.py links the windowed exe against. Building it from scratch
is a heavy, ONE-TIME-per-update step (CMake + Ninja, Dawn/RmlUi/abseil),
utterly unlike the per-player decomp/disc steps -- so the default and
primary path here is DETECT-AND-REUSE: if a working Aurora build already
exists (as it will on any machine that's ever finished this step, or this
dev workspace), skip straight past it.

Readiness is checked by literally importing tools/build.py and calling its
OWN `_resolve_aurora_link_items()` -- the single source of truth for which
archives the link step needs -- rather than re-deriving/duplicating that
~90-item list here (which would drift the moment build.py's list changes).

If Aurora truly isn't built yet, the default behavior is to print the exact
manual recipe (ground-truthed against this workspace's own
external_refs/repos/aurora/{build,build-rmlui}/CMakeCache.txt, which is how
the two trees here were actually configured) and stop -- Aurora already
being read-only, uninvited pipeline should never blindly start a 20-60
minute CMake build the first time it notices something missing.

Passing auto_build=True (--build-aurora) additionally attempts to DO that
build: clone Aurora at its pinned commit (only if the directory doesn't
exist yet -- this step refuses to touch an aurora checkout that's already
there, exactly like the "aurora stays read-only" rule), apply the patch
series (platform/gx/aurora-patches/), and drive the two CMake configure+
build invocations. This path is real (not a stub) but inherently less
battle-tested than the reuse path -- see docs/SETUP_TOOL.md for exactly
which parts of it this project's own verification did and didn't exercise.
"""
import os
import re
import sys

from . import common

DEFAULT_AURORA_URL = "https://github.com/encounter/aurora.git"
AURORA_PIN_RE = re.compile(r"aurora pin \(`([0-9a-f]{7,40})`\)")
DEFAULT_AURORA_PIN_FALLBACK = "1dde08f"

BUILD_TARGETS = ["aurora_pad", "aurora_si", "aurora_card", "aurora_mtx",
                  "aurora_core", "aurora_gx", "aurora_main", "aurora_vi"]


def _load_build_module():
    tools_dir = os.path.join(common.NATIVE_ROOT, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import build  # tools/build.py -- see its own module docstring
    return build


def read_aurora_pin():
    doc = os.path.join(common.NATIVE_ROOT, "docs", "PARTYBOARD_PROVENANCE.md")
    if os.path.exists(doc):
        with open(doc, "r", encoding="utf-8") as f:
            m = AURORA_PIN_RE.search(f.read())
        if m:
            return m.group(1)
        common.warn(f"couldn't find an 'aurora pin (`...`)' marker in {doc}")
    else:
        common.warn(f"{doc} not found")
    common.warn(f"falling back to the hardcoded aurora pin {DEFAULT_AURORA_PIN_FALLBACK!r}")
    return DEFAULT_AURORA_PIN_FALLBACK


def is_ready():
    """(bool ready, list missing_or_error) -- reuses tools/build.py's own
    archive resolution so this can never silently drift from what the link
    step actually needs."""
    try:
        build = _load_build_module()
        items = build._resolve_aurora_link_items()
    except Exception as exc:  # noqa: BLE001
        return False, [f"couldn't resolve aurora link items: {exc}"]
    missing = [i for i in items if not i.startswith("-") and not os.path.exists(i)]
    return (len(missing) == 0), missing


def _print_manual_recipe(missing):
    wrappers = os.path.join(common.TOOLCHAIN_DIR, "zig-cc-wrappers")
    common.banner("Aurora is not built yet -- manual recipe (docs/BUILDING.md)")
    print(f"  Missing archives (showing up to 10 of {len(missing)}):")
    for m in missing[:10]:
        print(f"    {m}")
    print()
    print("  One-time setup (also see docs/BUILDING.md's \"Aurora build trees\" section):")
    print(f"""
  1. Clone Aurora at the pinned commit:
       git clone {DEFAULT_AURORA_URL} {common.AURORA_DIR}
       git -C {common.AURORA_DIR} checkout {read_aurora_pin()}

  2. Apply this port's patch series (in numeric order):
       {os.path.join(common.NATIVE_ROOT, 'platform', 'gx', 'aurora-patches')}\\000N-*.patch
     (patches against the Aurora tree itself apply directly; the abseil-cpp
     patch (0001) applies to _deps/abseil-cpp-src AFTER the first configure
     below has fetched it -- see that patch file's own header comment.)

  3. Generate the zig compiler wrapper scripts CMake needs (a bare "zig cc"
     doesn't survive CMake's compiler-detection bootstrap):
       {wrappers}\\zigcc.bat   -> zig.exe cc
       {wrappers}\\zigcxx.bat  -> zig.exe c++
       {wrappers}\\zigrc.bat   -> (see zigrc_impl.ps1 already in that folder)

  4. Configure + build BOTH trees (plain, then RmlUi-enabled):
       cmake -S {common.AURORA_DIR} -B {common.AURORA_DIR}\\build -G Ninja ^
         -DCMAKE_BUILD_TYPE=Debug ^
         -DCMAKE_C_COMPILER={wrappers}\\zigcc.bat ^
         -DCMAKE_CXX_COMPILER={wrappers}\\zigcxx.bat ^
         -DCMAKE_RC_COMPILER={wrappers}\\zigrc.bat ^
         -DCMAKE_EXE_LINKER_FLAGS=-L{wrappers}\\stub-libs ^
         -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=package ^
         -DAURORA_ENABLE_RMLUI=OFF
       cmake --build {common.AURORA_DIR}\\build --target {' '.join(BUILD_TARGETS)}

       cmake -S {common.AURORA_DIR} -B {common.AURORA_DIR}\\build-rmlui -G Ninja ^
         (same flags as above, but -DAURORA_ENABLE_RMLUI=ON)
       cmake --build {common.AURORA_DIR}\\build-rmlui --target {' '.join(BUILD_TARGETS)}

  Or re-run this tool with --build-aurora to attempt this automatically
  (best-effort; see docs/SETUP_TOOL.md for its exact scope/limits).
""")


def _write_wrapper_scripts(zig_exe):
    wrappers = common.ensure_dir(os.path.join(common.TOOLCHAIN_DIR, "zig-cc-wrappers"))
    common.ensure_dir(os.path.join(wrappers, "stub-libs"))
    with open(os.path.join(wrappers, "zigcc.bat"), "w", newline="\r\n") as f:
        f.write(f'@echo off\r\n"{zig_exe}" cc %*\r\n')
    with open(os.path.join(wrappers, "zigcxx.bat"), "w", newline="\r\n") as f:
        f.write(f'@echo off\r\n"{zig_exe}" c++ %*\r\n')
    zigrc = os.path.join(wrappers, "zigrc.bat")
    if not os.path.exists(zigrc):
        raise common.SetupError(
            f"{zigrc} (+ zigrc_impl.ps1) doesn't exist and this tool doesn't know how to generate "
            "an RC-compiler wrapper from scratch -- copy it from an existing mp6-native checkout, "
            "or complete the Aurora build manually (see the recipe above)"
        )
    return wrappers


def _apply_patch_best_effort(apply_patches_mod, patch_path, roots):
    """Finds the patch's target file under any of `roots` (in order) and
    applies it in place. Returns True if applied, False if no candidate
    root currently has the target file (e.g. a FetchContent dep not fetched
    yet -- caller retries after configure)."""
    with open(patch_path, "r", encoding="utf-8", errors="surrogateescape") as f:
        patch_text = f.read()
    m = re.search(r"^--- a/(.+)$", patch_text, re.MULTILINE)
    if not m:
        common.warn(f"{patch_path}: no '--- a/...' header found, skipping")
        return True  # not a file patch we know how to place -- don't block the sequence
    rel = m.group(1).strip()
    for root in roots:
        target = os.path.join(root, *rel.split("/"))
        if os.path.exists(target):
            with open(target, "r", encoding="utf-8", errors="surrogateescape") as f:
                original = f.read()
            patched = apply_patches_mod.apply_unified_diff(original, patch_text, label=rel)
            with open(target, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(patched)
            common.ok(f"applied {os.path.basename(patch_path)} -> {target}")
            return True
    return False


def _configure_and_build_tree(cmake, build_dir, wrappers, zig_exe, rmlui, apply_patches_mod, patches_dir):
    del zig_exe  # baked into the wrapper scripts already
    args = [
        cmake, "-S", common.AURORA_DIR, "-B", build_dir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_C_COMPILER={os.path.join(wrappers, 'zigcc.bat')}",
        f"-DCMAKE_CXX_COMPILER={os.path.join(wrappers, 'zigcxx.bat')}",
        f"-DCMAKE_RC_COMPILER={os.path.join(wrappers, 'zigrc.bat')}",
        f"-DCMAKE_EXE_LINKER_FLAGS=-L{os.path.join(wrappers, 'stub-libs')}",
        "-DAURORA_DAWN_PROVIDER=package", "-DAURORA_SDL3_PROVIDER=package",
        f"-DAURORA_ENABLE_RMLUI={'ON' if rmlui else 'OFF'}",
    ]
    env = {"MSYS2_ARG_CONV_EXCL": "*"}
    common.run(args, env=env)

    # Second-pass patches: anything targeting a FetchContent dep now that
    # configure has populated _deps/.
    deps_dir = os.path.join(build_dir, "_deps")
    candidate_roots = [common.AURORA_DIR]
    if os.path.isdir(deps_dir):
        candidate_roots += [os.path.join(deps_dir, d) for d in os.listdir(deps_dir) if d.endswith("-src")]
    still_missing = []
    for name in sorted(os.listdir(patches_dir)):
        if not name.endswith(".patch"):
            continue
        if not _apply_patch_best_effort(apply_patches_mod, os.path.join(patches_dir, name), candidate_roots):
            still_missing.append(name)
    if still_missing:
        common.warn(f"{build_dir}: couldn't locate a target for {still_missing} under any known root -- "
                    "these may need the manual step (see the recipe printed above / docs/BUILDING.md)")

    common.run([cmake, "--build", build_dir, "--target"] + BUILD_TARGETS)


def _auto_build(url, assume_yes):
    cmake = common.which("cmake")
    if not cmake:
        raise common.SetupError("cmake not found on PATH", hint="see the prerequisite check output above")

    from . import step_toolchain
    zig_exe = step_toolchain.zig_exe_path()
    if not os.path.exists(zig_exe):
        raise common.SetupError(f"zig not found at {zig_exe}", hint="run the toolchain step first")

    if os.path.isdir(common.AURORA_DIR) and os.listdir(common.AURORA_DIR):
        raise common.SetupError(
            f"{common.AURORA_DIR} already exists -- refusing to touch it automatically "
            "(Aurora stays read-only once present; if it's genuinely incomplete/broken, "
            "resolve that by hand per docs/BUILDING.md, or remove the directory yourself "
            "first if you really want a from-scratch rebuild)"
        )

    pin = read_aurora_pin()
    common.info(f"cloning Aurora {url} @ {pin}")
    common.ensure_dir(os.path.dirname(common.AURORA_DIR))
    # core.longpaths=true -- see step_decomp.py's clone call for why.
    common.run(["git", "-c", "core.longpaths=true", "clone", url, common.AURORA_DIR])
    common.run(["git", "-c", "core.longpaths=true", "-C", common.AURORA_DIR, "checkout", "--detach", pin])

    wrappers = _write_wrapper_scripts(zig_exe)

    patches_dir = os.path.join(common.NATIVE_ROOT, "platform", "gx", "aurora-patches")
    sys.path.insert(0, os.path.join(common.NATIVE_ROOT, "tools"))
    import apply_patches as apply_patches_mod  # tools/apply_patches.py -- reused, not reimplemented

    # Patches that target the Aurora tree itself can go on right away.
    still_pending = []
    for name in sorted(os.listdir(patches_dir)):
        if not name.endswith(".patch"):
            continue
        if not _apply_patch_best_effort(apply_patches_mod, os.path.join(patches_dir, name), [common.AURORA_DIR]):
            still_pending.append(name)
    common.info(f"{len(still_pending)} patch(es) target FetchContent dependencies -- applied after configure")

    common.info("configuring + building the plain tree (build/) ...")
    _configure_and_build_tree(cmake, os.path.join(common.AURORA_DIR, "build"), wrappers, zig_exe,
                              rmlui=False, apply_patches_mod=apply_patches_mod, patches_dir=patches_dir)
    common.info("configuring + building the RmlUi-enabled tree (build-rmlui/) ...")
    _configure_and_build_tree(cmake, os.path.join(common.AURORA_DIR, "build-rmlui"), wrappers, zig_exe,
                              rmlui=True, apply_patches_mod=apply_patches_mod, patches_dir=patches_dir)


def ensure_aurora(auto_build=False, url=None, assume_yes=False):
    url = url or os.environ.get("MP6_AURORA_URL") or DEFAULT_AURORA_URL
    ready, missing = is_ready()
    if ready:
        common.ok(f"Aurora already built and ready: {common.AURORA_DIR}")
        return True

    if not auto_build:
        _print_manual_recipe(missing)
        raise common.SetupError(
            "Aurora is not built -- see the recipe just printed (or re-run with --build-aurora)"
        )

    common.warn("--build-aurora: attempting a from-scratch Aurora build. This is a best-effort path "
                "(20-60+ minutes, network-heavy CMake FetchContent fetches) -- see docs/SETUP_TOOL.md "
                "for exactly what this does and does not cover.")
    _auto_build(url, assume_yes)

    ready, missing = is_ready()
    if not ready:
        raise common.SetupError(
            f"--build-aurora finished but {len(missing)} archive(s) are still missing",
            hint="finish the remaining steps manually per docs/BUILDING.md; the recipe above shows exactly "
                 "what this attempted",
        )
    common.ok(f"Aurora built and ready: {common.AURORA_DIR}")
    return True
