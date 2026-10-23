"""Third-party sources for Aurora builds (FetchContent *-src trees).

Aurora itself is vendored in third_party/aurora with every port change in
place. The only remaining patches adapt sources Aurora's CMake fetches; they
live in compat/aurora/deps/ and are applied here, idempotently, to a copy of
those fetched trees before a build tree is configured against them.

    python tools/aurora_deps.py --check <deps-dir>   # are the patches applied?
    python tools/aurora_deps.py --apply <deps-dir>   # apply the missing ones
"""
import argparse
import os
import shutil
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.dirname(TOOLS)
sys.path[:0] = [TOOLS, NATIVE_ROOT]
import apply_patches  # noqa: E402
from setup.lib import step_aurora  # noqa: E402

PATCH_DIR = os.path.join(NATIVE_ROOT, "compat", "aurora", "deps")
# patch file -> the fetched tree it applies to
DEP_PATCHES = {
    "abseil-mingw-skip-windows-globalization.patch": "abseil-cpp-src",
    "rmlui-update-diagnostics.patch": "rmlui-src",
    "rmlui-transparent-text-paint.patch": "rmlui-src",
}
WEB_DEP_PATCHES = {
    "imgui-emdawnwebgpu.patch": "imgui-src",
}


def _sections(patch):
    with open(os.path.join(PATCH_DIR, patch), "r", encoding="utf-8", errors="surrogateescape") as f:
        return step_aurora.split_patch_sections(f.read())


def _applied(root, patch):
    for section in _sections(patch):
        target = os.path.join(root, *(section["new"] or section["old"]).split("/"))
        if not os.path.isfile(target):
            return False
        with open(target, "r", encoding="utf-8", errors="surrogateescape") as f:
            if not step_aurora._section_is_applied(apply_patches, f.read(), section["body"]):
                return False
    return True


def problems(deps_dir, web=False):
    """Patches that are not applied in deps_dir (empty when all are)."""
    table = dict(DEP_PATCHES, **(WEB_DEP_PATCHES if web else {}))
    out = []
    for patch, tree in table.items():
        root = os.path.join(deps_dir, tree)
        if not os.path.isdir(root):
            out.append(f"{tree} is missing from {deps_dir}")
        elif not _applied(root, patch):
            out.append(f"{patch} is not applied in {root}")
    return out


def apply(deps_dir, web=False):
    """Apply every dependency patch that is not applied yet."""
    table = dict(DEP_PATCHES, **(WEB_DEP_PATCHES if web else {}))
    for patch, tree in table.items():
        root = os.path.join(deps_dir, tree)
        if not os.path.isdir(root):
            raise RuntimeError(f"{tree} is missing from {deps_dir}")
        if _applied(root, patch):
            continue
        if not step_aurora._apply_patch(apply_patches, os.path.join(PATCH_DIR, patch), [root]):
            raise RuntimeError(f"{patch}: target files missing under {root}")


def copy_sources(source_deps, dest_deps, web=False):
    """Copy every fetched *-src tree from source_deps and apply the patches.
    Returns FETCHCONTENT_SOURCE_DIR_* definitions for CMake."""
    if not os.path.isdir(source_deps):
        raise RuntimeError(f"no fetched Aurora dependencies at {source_deps}")
    definitions = []
    for name in sorted(os.listdir(source_deps)):
        src = os.path.join(source_deps, name)
        if not name.endswith("-src") or not os.path.isdir(src):
            continue
        dst = os.path.join(dest_deps, name)
        if os.path.normcase(os.path.abspath(src)) != os.path.normcase(os.path.abspath(dst)):
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns(".git"), dirs_exist_ok=True)
        definitions.append(f"-DFETCHCONTENT_SOURCE_DIR_{name[:-4].upper()}={dst.replace(os.sep, '/')}")
    apply(dest_deps, web=web)
    return definitions


def find_cmake(*caches):
    """The native CMake a previous configure used, else the installed one.
    MSYS CMake must be avoided: it treats C:/ as a relative POSIX path."""
    for cache in caches:
        path = os.path.join(cache, "CMakeCache.txt")
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith("CMAKE_COMMAND:INTERNAL="):
                        found = line.split("=", 1)[1].strip()
                        if os.path.isfile(found):
                            return found
    for candidate in (os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "CMake", "bin",
                                   "cmake.exe"), shutil.which("cmake")):
        if candidate and os.path.isfile(candidate):
            return candidate
    raise RuntimeError("CMake is required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", metavar="DEPS_DIR")
    parser.add_argument("--apply", metavar="DEPS_DIR")
    parser.add_argument("--web", action="store_true")
    args = parser.parse_args()
    if args.apply:
        apply(args.apply, web=args.web)
        print("dependency patches applied in", args.apply)
        return 0
    if args.check:
        found = problems(args.check, web=args.web)
        for line in found:
            print(line)
        return 1 if found else 0
    parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
