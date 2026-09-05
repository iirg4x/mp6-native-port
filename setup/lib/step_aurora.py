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
Existence is necessary but NOT sufficient: the archives must also be
CURRENT, so is_ready() validates a build fingerprint (the verified checkout
commit + every patch file's content, stamped into the checkout after a build)
and content-checks the patched source/carve-out contract before adopting a
hand-built stamp-less tree. A stale tree is reported as such
instead of being handed to a link that would then fail.

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
build invocations. Patch application is per-TARGET-FILE: six of the
patches touch 2-11 files each, so the diff is split into one section per
file and applied all-or-nothing under whichever root holds them (the
Aurora tree, or a FetchContent dependency under a tree's _deps/). See
docs/SETUP_TOOL.md for exactly which parts of this path this project's own
verification did and didn't exercise.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

from . import common

DEFAULT_AURORA_URL = "https://github.com/encounter/aurora.git"
AURORA_PIN_RE = re.compile(r"aurora pin \(`([0-9a-f]{7,40})`\)")
DEFAULT_AURORA_PIN_FALLBACK = "1dde08f"

BUILD_TARGETS = ["aurora_pad", "aurora_si", "aurora_card", "aurora_mtx",
                  "aurora_core", "aurora_gx", "aurora_main", "aurora_vi"]

# Written next to the Aurora checkout after a successful --build-aurora.
# Records WHAT was built (pin + exact patch set), so is_ready() can reject a
# tree whose archives exist but predate the current pin/patch series -- mere
# existence would happily hand a stale archive to the link step, which then
# fails (or worse, links code that no longer matches the patches in tree).
STAMP_NAME = ".mp6-aurora-build.json"
# 5: the carve-out flags enter the fingerprint by repo-RELATIVE header path, so
#    a worktree and its main checkout agree. See _fingerprint_carveout_args().
# 6: the Zig archive tools are explicit; CMake cannot infer them through the
#    compiler batch wrappers on Windows.
BUILD_CONTRACT_VERSION = 6
ARTIFACT_STAMP_VERSION = 4


def _host_section_header():
    return os.path.join(common.NATIVE_ROOT, "shim", "include", "mp6_host_section.h")


def _cmake_path(path):
    """Format a filesystem path for a CMake command-line argument.

    CMake treats backslashes in ``-D`` values as escape characters while
    parsing its cache entries.  That turns a normal Windows path such as
    ``C:\\Users\\...`` into an invalid compiler path (notably for
    ``CMAKE_RC_COMPILER``).  Forward slashes are accepted by Windows tools
    and keep the same argument unambiguous on every host.
    """
    return os.fspath(path).replace("\\", "/")


def _cmake_flag_path(path):
    """Quote one path inside a CMake compiler/linker flag string."""
    path = _cmake_path(path)
    return f'"{path}"' if any(c.isspace() for c in path) else path


def _carveout_args_for(header):
    """The carve-out flag triple, built around an already-formatted *header*.

    Single source of truth for the flag SHAPE, so the real cmake arguments and
    their fingerprint form cannot drift apart.
    """
    forced = f"-include {_cmake_flag_path(header)}"
    return [
        f"-DCMAKE_C_FLAGS={forced}",
        f"-DCMAKE_CXX_FLAGS={forced}",
        "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
    ]


def _carveout_cmake_args():
    """Flags required by tools/build.py's verify_aurora_carveout().

    These go to a real cmake invocation, so the forced include has to name the
    header by ABSOLUTE path. Do not hash these -- see
    _fingerprint_carveout_args().
    """
    return _carveout_args_for(_host_section_header().replace("\\", "/"))


def _fingerprint_carveout_args():
    """_carveout_cmake_args() with the checkout's LOCATION factored out.

    The forced-include flag necessarily carries mp6_host_section.h's absolute
    path, which differs between the main checkout and a worktree of the very
    same commit. Hashing it made provenance a property of WHERE a tree sits
    rather than WHAT it contains, so byte-identical trees disagreed and every
    worktree failed the check. Bind the repo-relative path instead; the
    header's CONTENT is hashed separately in build_fingerprint(), so editing
    the carve-out still moves the fingerprint -- only relocating the tree no
    longer does.
    """
    rel = os.path.relpath(_host_section_header(), common.NATIVE_ROOT)
    return _carveout_args_for(rel.replace("\\", "/"))


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


def patches_dir():
    return os.path.join(common.NATIVE_ROOT, "platform", "gx", "aurora-patches")


def _patch_files():
    d = patches_dir()
    if not os.path.isdir(d):
        return []
    return [os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".patch")]


def verified_checkout_commit(pin=None):
    """Return Aurora's full HEAD commit after proving it resolves to *pin*."""
    pin = pin or read_aurora_pin()

    def resolve(ref):
        try:
            proc = subprocess.run(
                ["git", "-C", common.AURORA_DIR, "rev-parse", "--verify", ref],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
        except OSError as exc:
            raise RuntimeError(f"cannot run Git to verify Aurora checkout: {exc}") from exc
        commit = proc.stdout.strip().lower()
        if proc.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
            detail = proc.stderr.strip()
            raise RuntimeError(
                f"cannot resolve Aurora Git ref {ref!r} in {common.AURORA_DIR}"
                + (f": {detail}" if detail else "")
            )
        return commit

    head = resolve("HEAD")
    expected = resolve(pin)
    if head != expected:
        raise RuntimeError(
            f"Aurora checkout is at {head}, but the pinned commit {pin!r} resolves to {expected}"
        )
    return head


def build_fingerprint(pin=None, zig_tree=None):
    """A hash of exactly what a built Aurora tree is supposed to CONTAIN: the
    pinned upstream commit plus every patch file's name and content. Any pin
    bump or patch edit changes it, which is precisely when the built archives
    stop matching the sources and have to be rebuilt."""
    h = hashlib.sha256()
    h.update(f"contract:{BUILD_CONTRACT_VERSION}\n".encode("ascii"))
    pin = pin or read_aurora_pin()
    h.update(pin.encode("utf-8"))
    h.update(b"\ncheckout-commit:")
    h.update(verified_checkout_commit(pin).encode("ascii"))
    for arg in _fingerprint_carveout_args():
        h.update(arg.encode("utf-8"))
    header = _host_section_header()
    if os.path.isfile(header):
        with open(header, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    # Aurora compiler output depends on Zig's bundled headers/libraries as
    # well as zig.exe. Bind the full verified install tree, not a shallow
    # executable hash.
    from . import step_toolchain
    zig = step_toolchain.zig_exe_path()
    if not os.path.isfile(zig):
        raise RuntimeError(f"pinned Zig executable is missing: {zig}")
    if zig_tree is None:
        zig_tree = step_toolchain.verified_required_zig_tree()
    h.update(json.dumps(zig_tree, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    for p in _patch_files():
        h.update(os.path.basename(p).encode("utf-8"))
        with open(p, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def _stamp_path():
    return os.path.join(common.AURORA_DIR, STAMP_NAME)


def read_stamp():
    try:
        with open(_stamp_path(), "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:  # noqa: BLE001 -- absent/corrupt both mean "no stamp"
        return None


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact_manifest(paths):
    records = []
    seen = set()
    for raw_path in paths:
        path = os.path.realpath(os.fspath(raw_path))
        key = os.path.normcase(path)
        if key in seen:
            continue
        seen.add(key)
        if not os.path.isfile(path):
            raise RuntimeError(f"required Aurora link/runtime/config input is missing: {path}")
        records.append({
            "path": path,
            "bytes": os.path.getsize(path),
            "sha256": _sha256_file(path),
        })
    records.sort(key=lambda item: os.path.normcase(item["path"]))
    return records


def _profile_paths(build, profile, items=None):
    if profile == "windows":
        items = build._resolve_aurora_link_items() if items is None else items
        paths = [item for item in items if not item.startswith("-")]
        paths += list(build.AURORA_RUNTIME_DLLS)
        paths += [build.NOD_WIN_LIB]
        build_dir = build.AURORA_BUILD_RMLUI
    elif profile == "android":
        items = build._resolve_android_aurora_link_items() if items is None else items
        paths = [item for item in items if not item.startswith("-")]
        paths += [build.NOD_ANDROID_LIB]
        build_dir = build.AURORA_BUILD_ANDROID_RMLUI
    else:
        raise RuntimeError(f"unknown Aurora artifact profile: {profile}")
    paths += [os.path.join(build_dir, "CMakeCache.txt"), os.path.join(build_dir, "build.ninja")]
    return paths


def _artifact_profile_problem(paths, recorded):
    try:
        actual = _artifact_manifest(paths)
    except RuntimeError as exc:
        return str(exc)
    if not isinstance(recorded, list):
        return "Aurora stamp has no artifact manifest for this link profile"
    if actual != recorded:
        by_path = {os.path.normcase(item.get("path", "")): item for item in recorded
                   if isinstance(item, dict)}
        for item in actual:
            prior = by_path.get(os.path.normcase(item["path"]))
            if prior != item:
                return (f"Aurora artifact identity mismatch for {item['path']}: "
                        f"stamp={prior!r}, actual={item!r}")
        return "Aurora artifact manifest contains stale or extra inputs"
    return None


def verify_link_inputs(profile, build_module=None, items=None, check_fingerprint=True):
    build = build_module or _load_build_module()
    stamp = read_stamp()
    if not isinstance(stamp, dict) or stamp.get("artifact_stamp_version") != ARTIFACT_STAMP_VERSION:
        return [f"Aurora artifact stamp is absent/obsolete: {_stamp_path()}"]
    if check_fingerprint:
        try:
            current_fingerprint = build_fingerprint(
                zig_tree=getattr(build, "_VERIFIED_ZIG_TREE_IDENTITY", None)
            )
        except Exception as exc:  # noqa: BLE001
            return [f"couldn't verify Aurora source/Zig build fingerprint: {exc}"]
        if stamp.get("fingerprint") != current_fingerprint:
            return [
                f"Aurora source/Zig fingerprint mismatch: stamp={stamp.get('fingerprint')!r}, "
                f"actual={current_fingerprint!r}"
            ]
    try:
        paths = _profile_paths(build, profile, items=items)
    except Exception as exc:  # noqa: BLE001
        return [f"couldn't resolve Aurora {profile} profile: {exc}"]
    problem = _artifact_profile_problem(paths, (stamp.get("profiles") or {}).get(profile))
    return [problem] if problem else []


def write_stamp(pin, trees):
    build = _load_build_module()
    checkout_commit = verified_checkout_commit(pin)
    profiles = {}
    for profile in ("windows", "android"):
        try:
            profiles[profile] = _artifact_manifest(_profile_paths(build, profile))
        except Exception:
            if profile == "windows":
                raise
    data = {
        "artifact_stamp_version": ARTIFACT_STAMP_VERSION,
        "fingerprint": build_fingerprint(pin),
        "pin": pin,
        "checkout_commit": checkout_commit,
        "patches": [os.path.basename(p) for p in _patch_files()],
        "profiles": profiles,
        "trees": list(trees),
    }
    path = _stamp_path()
    tmp = f"{path}.part-{os.getpid()}"
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)
    finally:
        try:
            os.remove(tmp)
        except FileNotFoundError:
            pass
    return data


def _stale_by_mtime(archives):
    """Fallback staleness check for a tree with no stamp (hand-built, e.g. per
    docs/BUILDING.md's manual recipe). An archive older than the newest patch
    file cannot possibly contain that patch."""
    newest_src, newest_name = 0.0, None
    for p in _patch_files():
        m = os.path.getmtime(p)
        if m > newest_src:
            newest_src, newest_name = m, os.path.basename(p)
    if newest_name is None:
        return []
    return [f"{a} predates {newest_name}" for a in archives
            if os.path.exists(a) and os.path.getmtime(a) < newest_src]


def _patch_source_roots():
    roots = [common.AURORA_DIR]
    # Prefer the RmlUi tree because it is what the windowed executable links.
    for tree_name in ("build-rmlui", "build"):
        deps = os.path.join(common.AURORA_DIR, tree_name, "_deps")
        if os.path.isdir(deps):
            roots.extend(os.path.join(deps, d) for d in sorted(os.listdir(deps))
                         if d.endswith("-src") and os.path.isdir(os.path.join(deps, d)))
    return roots


def _section_is_applied(apply_patches_mod, text, body):
    lines = text.splitlines(keepends=True)
    for hunk in apply_patches_mod._parse_hunks(body):
        new_lines = hunk["new_lines"]
        if not new_lines:
            continue
        begin = hunk["new_start"] - 1
        if begin < 0 or lines[begin:begin + len(new_lines)] != new_lines:
            return False
    return True


def _verify_patch_sources():
    """Content-verifies a stamp-less hand-built checkout.

    Comparing every dependency archive mtime to the newest patch rejected
    correct trees forever (prebuilt/unaffected libraries naturally keep older
    mtimes). Instead verify the post-patch text itself in the shared Aurora or
    fetched dependency source trees.
    """
    sys.path.insert(0, os.path.join(common.NATIVE_ROOT, "tools"))
    import apply_patches as apply_patches_mod

    roots = _patch_source_roots()
    problems = []
    expected = {}
    actual_paths = {}
    dependency_sections = []
    pin = read_aurora_pin()
    for patch in _patch_files():
        with open(patch, "r", encoding="utf-8", errors="surrogateescape") as f:
            sections = split_patch_sections(f.read())
        for section in sections:
            rel = section["new"] or section["old"]
            if rel not in expected:
                proc = subprocess.run(
                    ["git", "-C", common.AURORA_DIR, "show", f"{pin}:{rel}"],
                    capture_output=True,
                )
                if proc.returncode == 0:
                    expected[rel] = proc.stdout.decode("utf-8", "surrogateescape")
                    actual_paths[rel] = os.path.join(common.AURORA_DIR, *rel.split("/"))
                elif section["old"] is None:
                    expected[rel] = ""
                    actual_paths[rel] = os.path.join(common.AURORA_DIR, *rel.split("/"))
                else:
                    dependency_sections.append((patch, section, rel))
                    continue
            try:
                expected[rel] = apply_patches_mod.apply_unified_diff(
                    expected[rel], section["body"], label=f"verify:{os.path.basename(patch)}:{rel}")
            except ValueError as exc:
                problems.append(str(exc))

    # Aurora-owned files can be verified exactly by replaying the whole patch
    # queue from the pinned Git object. This remains correct when later patches
    # intentionally edit lines introduced by earlier ones.
    for rel, want in expected.items():
        target = actual_paths[rel]
        try:
            with open(target, "r", encoding="utf-8", errors="surrogateescape") as f:
                got = f.read()
        except OSError:
            problems.append(f"patched Aurora target is absent: {rel}")
            continue
        if got != want:
            problems.append(f"patched Aurora target differs from replayed queue: {rel}")

    # FetchContent dependencies are outside Aurora's Git object database.
    # No later port patch edits the same dependency targets, so verify their
    # final added/context blocks directly in the linked RmlUi dependency tree.
    for patch, section, rel in dependency_sections:
        candidates = [os.path.join(root, *rel.split("/")) for root in roots]
        candidates = [p for p in candidates if os.path.isfile(p)]
        if not candidates:
            problems.append(f"{os.path.basename(patch)}: dependency target is absent: {rel}")
            continue
        applied = False
        for target in candidates:
            with open(target, "r", encoding="utf-8", errors="surrogateescape") as f:
                if _section_is_applied(apply_patches_mod, f.read(), section["body"]):
                    applied = True
                    break
        if not applied:
            problems.append(f"{os.path.basename(patch)}: post-patch content not found in {rel}")
    return problems


def _verify_carveout(build):
    checks = [
        (os.path.join(build.AURORA_BUILD_RMLUI, "libaurora_gx.a"), ".mp6hbss"),
        (os.path.join(build.AURORA_BUILD_RMLUI, "librmlui.a"), ".mp6hbss"),
        (os.path.join(build.AURORA_BUILD_RMLUI, "extern", "libsqlite3.a"), ".mp6hdat"),
    ]
    return [f"{path}: missing required savestate carve-out section {section}"
            for path, section in checks
            if not os.path.isfile(path) or not build._ar_has_section(path, section)]


def is_ready():
    """(bool ready, list problems) -- reuses tools/build.py's own archive
    resolution so this can never silently drift from what the link step
    actually needs, then checks those archives are CURRENT (fingerprint stamp
    or verified stamp-less source content) rather than merely present."""
    try:
        build = _load_build_module()
        items = build._resolve_aurora_link_items()
    except Exception as exc:  # noqa: BLE001
        return False, [f"couldn't resolve aurora link items: {exc}"]
    archives = [i for i in items if not i.startswith("-")]
    missing = [i for i in archives if not os.path.exists(i)]
    if missing:
        return False, missing

    stamp = read_stamp()
    if stamp is not None:
        try:
            want = build_fingerprint()
        except Exception as exc:  # noqa: BLE001
            return False, [f"couldn't fingerprint full Zig/Aurora build contract: {exc}"]
        if stamp.get("fingerprint") != want:
            return False, [
                f"aurora build fingerprint mismatch: {_stamp_path()} records "
                f"{str(stamp.get('fingerprint'))[:12]}... (pin {stamp.get('pin')}, "
                f"{len(stamp.get('patches') or [])} patch(es)) but the current pin + "
                f"platform/gx/aurora-patches/ hash to {want[:12]}... -- the archives are "
                f"stale and must be rebuilt"
            ]
        structural = (verify_link_inputs("windows", build_module=build, items=items,
                                         check_fingerprint=False)
                      + _verify_patch_sources() + _verify_carveout(build))
        if "android" in (stamp.get("profiles") or {}):
            structural = (verify_link_inputs(
                "android", build_module=build, check_fingerprint=False
            ) + structural)
        return (not structural), structural

    # Stamp-less/manual build recovery: verify actual patched source content
    # and the archive section invariant, then ensure_aurora() can adopt it by
    # writing the current fingerprint. This is deterministic and does not use
    # unrelated third-party archive mtimes as a proxy for patch application.
    structural = _verify_patch_sources() + _verify_carveout(build)
    return (not structural), structural


def _print_manual_recipe(missing):
    wrappers = _cmake_path(os.path.join(common.TOOLCHAIN_DIR, "zig-cc-wrappers"))
    aurora = _cmake_path(common.AURORA_DIR)
    patches = _cmake_path(os.path.join(common.NATIVE_ROOT, "platform", "gx", "aurora-patches"))

    def configure_recipe(build_dir, rmlui):
        args = _configure_args("cmake", build_dir, wrappers, rmlui)
        # CMake flag values contain their own quotes when a path has spaces.
        # Escape those for Windows argv parsing as well as quoting the -D arg.
        return " ^\n         ".join(
            [subprocess.list2cmdline(args[:7])]
            + [subprocess.list2cmdline([arg]) for arg in args[7:]]
        )

    plain_configure = configure_recipe(f"{aurora}/build", False)
    rmlui_configure = configure_recipe(f"{aurora}/build-rmlui", True)
    common.banner("Aurora is not built/current -- manual recipe (docs/BUILDING.md)")
    print(f"  Problems (showing up to 10 of {len(missing)}):")
    for m in missing[:10]:
        print(f"    {m}")
    print()
    print("  One-time setup (also see docs/BUILDING.md's \"Aurora build trees\" section):")
    print(f"""
  1. Clone Aurora at the pinned commit:
       git clone "{DEFAULT_AURORA_URL}" "{aurora}"
       git -C "{aurora}" checkout "{read_aurora_pin()}"

  2. Apply this port's patch series (in numeric order):
       "{patches}/000N-*.patch"
     (patches against the Aurora tree itself apply directly; the abseil-cpp
     patch (0001) applies to _deps/abseil-cpp-src AFTER the first configure
     below has fetched it -- see that patch file's own header comment.)

  3. Generate the zig compiler wrapper scripts CMake needs (a bare "zig cc"
     doesn't survive CMake's compiler-detection bootstrap). --build-aurora
     writes all five itself; by hand they are:
       "{wrappers}/zigcc.bat"      -> zig.exe cc  -target x86_64-windows-gnu
       "{wrappers}/zigcxx.bat"     -> zig.exe c++ -target x86_64-windows-gnu
       "{wrappers}/zigar.bat"      -> zig.exe ar
       "{wrappers}/zigranlib.bat"  -> zig.exe ranlib
       "{wrappers}/zigrc.bat"      -> powershell -File zigrc_impl.ps1
       "{wrappers}/zigrc_impl.ps1" -> zig.exe rc, with valued -D defines
                                     pre-expanded into the .rc source

  4. Configure + build BOTH trees (plain, then RmlUi-enabled):
       {plain_configure}
       cmake --build "{aurora}/build" --target {' '.join(BUILD_TARGETS)}

       {rmlui_configure}
       cmake --build "{aurora}/build-rmlui" --target {' '.join(BUILD_TARGETS)}

  Or re-run this tool with --build-aurora to attempt this automatically
  (best-effort; see docs/SETUP_TOOL.md for its exact scope/limits).
""")


# The RC-compiler wrapper. zig 0.16.0's `zig rc` (resinator) mis-parses
# "-D NAME=VALUE": it drops the "=" and forwards NAME and VALUE as two argv
# entries, after which resinator misreads the trailing positionals and the real
# input .rc file gets swallowed ("zlib1.rc" -> "1.rc"). This wrapper pre-expands
# any valued define into a scratch copy of the .rc source instead of passing it
# on the command line, and passes value-less -D through untouched.
#
# No param() block on purpose: a declared parameter (even with
# ValueFromRemainingArguments) turns on PowerShell's cmdlet-style named-parameter
# binding, which then matches tokens like "-I" against -InformationAction and
# aborts with "ambiguous parameter". Reading $args skips that binding entirely.
_ZIGRC_IMPL_PS1 = r'''$RawArgs = $args
$zig = "{zig_exe}"

$passthrough = New-Object System.Collections.Generic.List[string]
$defineLines = New-Object System.Collections.Generic.List[string]

$i = 0
while ($i -lt $RawArgs.Count) {{
  $a = $RawArgs[$i]
  if ($a -match '^-O$') {{
    # windres-style "-O coff": drop it, resinator infers format from the output extension.
    $i += 2
    continue
  }}
  if ($a -match '^[-/][Dd](.*)$') {{
    $rest = $Matches[1]
    if ($rest -eq '') {{
      $i++
      $rest = $RawArgs[$i]
    }}
    if ($rest -match '^([^=]+)=(.*)$') {{
      $defineLines.Add("#define $($Matches[1]) $($Matches[2])")
    }} else {{
      $passthrough.Add("-D$rest")
    }}
  }} else {{
    $passthrough.Add($a)
  }}
  $i++
}}

if ($passthrough.Count -eq 0) {{
  Write-Error "zigrc_impl.ps1: no input file found in arguments"
  exit 1
}}

$inputFile = $passthrough[$passthrough.Count - 1]

if ($defineLines.Count -gt 0 -and (Test-Path -LiteralPath $inputFile)) {{
  $dir = Split-Path -Parent $inputFile
  $name = Split-Path -Leaf $inputFile
  $tempRc = Join-Path $dir "zigrc_injected.$name"
  $original = Get-Content -Raw -LiteralPath $inputFile
  $prefix = ($defineLines -join "`r`n") + "`r`n"
  Set-Content -LiteralPath $tempRc -Value ($prefix + $original) -NoNewline
  $passthrough[$passthrough.Count - 1] = $tempRc
}}

& $zig rc @passthrough
exit $LASTEXITCODE
'''


def _write_wrapper_scripts(zig_exe):
    """Generate every Zig tool wrapper needed by CMake's Windows bootstrap.

    A bare "zig cc" does not survive compiler detection, and CMake cannot
    infer an archiver from the compiler batch wrappers. Nothing has to be
    copied from another checkout for a fresh machine to build Aurora.
    """
    wrappers = common.ensure_dir(os.path.join(common.TOOLCHAIN_DIR, "zig-cc-wrappers"))
    common.ensure_dir(os.path.join(wrappers, "stub-libs"))
    # A per-project zig cache keeps the wrappers self-contained (CMake probes
    # run with whatever cwd it likes); the explicit -target keeps every object
    # on the same ABI as tools/build.py's own link line.
    cache = os.path.join(common.TOOLCHAIN_DIR, "zig-cache")
    common.ensure_dir(cache)
    for name, verb in (("zigcc.bat", "cc"), ("zigcxx.bat", "c++")):
        with open(os.path.join(wrappers, name), "w", newline="\r\n") as f:
            f.write("@echo off\r\n"
                    f"set ZIG_GLOBAL_CACHE_DIR={cache}\r\n"
                    f"set ZIG_LOCAL_CACHE_DIR={cache}\r\n"
                    f'"{zig_exe}" {verb} -target x86_64-windows-gnu %*\r\n')
    for name, verb in (("zigar.bat", "ar"), ("zigranlib.bat", "ranlib")):
        with open(os.path.join(wrappers, name), "w", newline="\r\n") as f:
            f.write("@echo off\r\n"
                    f'"{zig_exe}" {verb} %*\r\n'
                    "exit /b %ERRORLEVEL%\r\n")
    zigrc_impl = os.path.join(wrappers, "zigrc_impl.ps1")
    # These files embed absolute workspace/toolchain paths. Refresh them on
    # every provisioning attempt so moving the checkout or changing Zig never
    # leaves the RC compiler pointing at the previous location.
    with open(zigrc_impl, "w", newline="\r\n") as f:
        f.write(_ZIGRC_IMPL_PS1.format(zig_exe=zig_exe))
    common.ok(f"generated {zigrc_impl}")
    zigrc = os.path.join(wrappers, "zigrc.bat")
    with open(zigrc, "w", newline="\r\n") as f:
        f.write("@echo off\r\n"
                f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{zigrc_impl}" %*\r\n'
                "exit /b %ERRORLEVEL%\r\n")
    common.ok(f"generated {zigrc}")
    return wrappers


def _strip_ab(path):
    if path.startswith("a/") or path.startswith("b/"):
        return path[2:]
    return path


def _drop_noop_hunks(body_lines):
    """Drops hunks that contain no '-' and no '+' line. Several patches in this
    series close with a pure-context fragment that just SHOWS the surrounding
    code (e.g. 0002's `@@ -771,3 +782,3 @@`); it edits nothing, and its
    hand-written @@ line numbers have drifted, so keeping it only produces a
    bogus context mismatch. Dropping a no-op hunk cannot change the output."""
    out, cur = [], None
    for ln in body_lines:
        if ln.startswith("@@"):
            if cur and any(x[:1] in ("-", "+") for x in cur[1:]):
                out += cur
            cur = [ln]
        elif cur is not None:
            cur.append(ln)
    if cur and any(x[:1] in ("-", "+") for x in cur[1:]):
        out += cur
    return out


def split_patch_sections(patch_text):
    """Splits a unified diff into ONE section per target file:
    [{"old": rel-or-None, "new": rel-or-None, "body": "<the @@ hunks only>"}].

    A patch touching N files has N sections -- applying the whole diff to just
    the first one (as this helper used to) silently misapplies every multi-file
    patch in platform/gx/aurora-patches/ (0012/0013/0014/0016/0017/0018 all
    touch 2-11 files). The per-section body starts at the first `@@` so the
    `diff --git`/`index`/`new file mode` preamble never reaches the hunk
    parser, which would otherwise take those lines for context."""
    lines = patch_text.splitlines(keepends=True)
    starts = []
    seen_hunk = True  # the first header needs no preceding hunk
    for i, ln in enumerate(lines):
        if ln.startswith("diff --git "):
            if seen_hunk:
                starts.append(i)
                seen_hunk = False
        elif ln.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
            # git patches lead with `diff --git` (already recorded above); a
            # hand-written one starts right here.
            if seen_hunk:
                starts.append(i)
                seen_hunk = False
        elif ln.startswith("@@"):
            seen_hunk = True
    sections = []
    for n, begin in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        chunk = lines[begin:end]
        old = new = None
        first_hunk = None
        for j, ln in enumerate(chunk):
            if ln.startswith("@@"):
                first_hunk = j
                break
            if ln.startswith("--- ") and old is None:
                old = ln[4:].rstrip("\r\n").split("\t")[0]
            elif ln.startswith("+++ ") and new is None:
                new = ln[4:].rstrip("\r\n").split("\t")[0]
        if first_hunk is None:
            continue  # mode-only/rename-only stanza: nothing to apply
        body = chunk[first_hunk:]
        # Every patch in platform/gx/aurora-patches/ is a prose-wrapped diff:
        # a "Why/Verified:" narrative surrounds the hunks. The leading prose is
        # already excluded (the body starts at the first `@@`); trim the
        # TRAILING prose too -- a hunk body line can only start with ' ', '-',
        # '+' or '\', so anything after the last such line is narrative. Left
        # in, it is parsed as trailing context and the removed-block check then
        # fails against the real source (which is exactly what --build-aurora
        # did before this).
        while body and body[-1][:1] not in (" ", "-", "+", "\\"):
            body.pop()
        body = _drop_noop_hunks(body)
        sections.append({
            "old": None if old in (None, "/dev/null") else _strip_ab(old),
            "new": None if new in (None, "/dev/null") else _strip_ab(new),
            "body": "".join(body),
        })
    return sections


def _apply_patch(apply_patches_mod, patch_path, roots):
    """Applies EVERY file section of `patch_path` under whichever of `roots`
    holds the files it modifies. All-or-nothing: if any modified target is
    missing under a root, that root is rejected outright rather than leaving
    the patch half-applied. Returns True if applied, False if no candidate root
    has the targets yet (e.g. a FetchContent dep not fetched -- the caller
    retries after configure)."""
    with open(patch_path, "r", encoding="utf-8", errors="surrogateescape") as f:
        patch_text = f.read()
    sections = split_patch_sections(patch_text)
    name = os.path.basename(patch_path)
    if not sections:
        common.warn(f"{patch_path}: no file sections found, skipping")
        return True

    chosen = None
    for root in roots:
        if all(s["old"] is None or os.path.exists(os.path.join(root, *s["old"].split("/")))
               for s in sections):
            chosen = root
            break
    if chosen is None:
        return False

    # Stage every output first, then write -- so a hunk mismatch in file 5 of
    # 11 raises before files 1-4 have been rewritten.
    staged = []
    for s in sections:
        rel = s["new"] or s["old"]
        target = os.path.join(chosen, *rel.split("/"))
        if s["old"] is None:
            original = ""  # new file
        else:
            with open(os.path.join(chosen, *s["old"].split("/")), "r",
                      encoding="utf-8", errors="surrogateescape") as f:
                original = f.read()
        staged.append((target, apply_patches_mod.apply_unified_diff(
            original, s["body"], label=f"{name}:{rel}")))
    for target, text in staged:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(text)
    common.ok(f"applied {name} -> {len(staged)} file(s) under {chosen}")
    return True


def _configure_args(cmake, build_dir, wrappers, rmlui):
    """Shared argv for the automatic build and the copyable manual recipe."""
    cmake = _cmake_path(cmake)
    source_dir = _cmake_path(common.AURORA_DIR)
    build_dir_arg = _cmake_path(build_dir)
    compiler = _cmake_path(os.path.join(wrappers, "zigcc.bat"))
    cxx_compiler = _cmake_path(os.path.join(wrappers, "zigcxx.bat"))
    archiver = _cmake_path(os.path.join(wrappers, "zigar.bat"))
    ranlib = _cmake_path(os.path.join(wrappers, "zigranlib.bat"))
    rc_compiler = _cmake_path(os.path.join(wrappers, "zigrc.bat"))
    stub_libs = _cmake_path(os.path.join(wrappers, "stub-libs"))
    return [
        cmake, "-S", source_dir, "-B", build_dir_arg, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_C_COMPILER={compiler}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DCMAKE_AR={archiver}",
        f"-DCMAKE_RANLIB={ranlib}",
        f"-DCMAKE_RC_COMPILER={rc_compiler}",
        f"-DCMAKE_EXE_LINKER_FLAGS=-L{_cmake_flag_path(stub_libs)}",
        "-DAURORA_DAWN_PROVIDER=package", "-DAURORA_SDL3_PROVIDER=package",
        f"-DAURORA_ENABLE_RMLUI={'ON' if rmlui else 'OFF'}",
    ] + _carveout_cmake_args()


def _configure_and_build_tree(cmake, build_dir, wrappers, rmlui, apply_patches_mod, pending):
    """`pending` is the list of patch paths that did NOT apply to the Aurora
    tree itself -- i.e. the ones targeting a FetchContent dep. It is retried
    (not consumed) per tree on purpose: each tree gets its OWN _deps/ copy, so
    the dep patch has to go on twice. The patches that already applied to the
    shared Aurora tree are deliberately NOT retried here -- re-running them
    would either double-apply or (with this applier's content check) hard-fail
    on the second tree."""
    args = _configure_args(cmake, build_dir, wrappers, rmlui)
    env = {"MSYS2_ARG_CONV_EXCL": "*"}
    common.run(args, env=env)

    deps_dir = os.path.join(build_dir, "_deps")
    dep_roots = []
    if os.path.isdir(deps_dir):
        dep_roots = [os.path.join(deps_dir, d) for d in os.listdir(deps_dir) if d.endswith("-src")]
    still_missing = [os.path.basename(p) for p in pending
                     if not _apply_patch(apply_patches_mod, p, dep_roots)]
    if still_missing:
        raise common.SetupError(
            f"{build_dir}: no target found for {still_missing} under {deps_dir} or the aurora tree",
            hint="apply those by hand per docs/BUILDING.md, or check the patch's own header comment "
                 "for which dependency tree it belongs to")

    common.run([_cmake_path(cmake), "--build", _cmake_path(build_dir), "--target"] + BUILD_TARGETS)


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

    sys.path.insert(0, os.path.join(common.NATIVE_ROOT, "tools"))
    import apply_patches as apply_patches_mod  # tools/apply_patches.py -- reused, not reimplemented

    # Patches that target the Aurora tree itself can go on right away.
    pending = [p for p in _patch_files()
               if not _apply_patch(apply_patches_mod, p, [common.AURORA_DIR])]
    common.info(f"{len(pending)} patch(es) target FetchContent dependencies -- applied after configure")

    trees = []
    common.info("configuring + building the plain tree (build/) ...")
    tree = os.path.join(common.AURORA_DIR, "build")
    _configure_and_build_tree(cmake, tree, wrappers, False, apply_patches_mod, pending)
    trees.append("build")
    common.info("configuring + building the RmlUi-enabled tree (build-rmlui/) ...")
    tree = os.path.join(common.AURORA_DIR, "build-rmlui")
    _configure_and_build_tree(cmake, tree, wrappers, True, apply_patches_mod, pending)
    trees.append("build-rmlui")
    write_stamp(pin, trees)


def ensure_aurora(auto_build=False, url=None, assume_yes=False):
    url = url or os.environ.get("MP6_AURORA_URL") or DEFAULT_AURORA_URL
    ready, missing = is_ready()
    if ready:
        stamp = read_stamp()
        needs_adoption = stamp is None
        if (not needs_adoption
                and "android" not in (stamp.get("profiles") or {})):
            build = _load_build_module()
            needs_adoption = os.path.isdir(build.AURORA_BUILD_ANDROID_RMLUI)
        if needs_adoption:
            write_stamp(read_aurora_pin(), ["build", "build-rmlui"])
            common.ok(f"verified and adopted Aurora artifact profiles: {_stamp_path()}")
        common.ok(f"Aurora already built and ready: {common.AURORA_DIR}")
        return True

    # A tree that exists but is STALE (pin bumped / patch edited since it was
    # built) needs a rebuild, not a re-clone -- and --build-aurora's clone path
    # deliberately refuses to touch an existing checkout, so say so plainly
    # instead of failing later in the middle of the link.
    if os.path.isdir(common.AURORA_DIR) and os.listdir(common.AURORA_DIR):
        _print_manual_recipe(missing)
        raise common.SetupError(
            f"{common.AURORA_DIR} exists but its build is not current (see the problems above)",
            hint="re-apply the changed patch(es), reconfigure with every flag in the printed recipe, "
                 "re-run both builds, then delete the old stamp and re-run setup; the stamp-less "
                 "verifier will content-check and adopt the rebuilt tree")

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
            f"--build-aurora finished but the result is still not usable ({len(missing)} problem(s): "
            f"{'; '.join(str(m) for m in missing[:3])})",
            hint="finish the remaining steps manually per docs/BUILDING.md; the recipe above shows exactly "
                 "what this attempted",
        )
    common.ok(f"Aurora built and ready: {common.AURORA_DIR}")
    return True
