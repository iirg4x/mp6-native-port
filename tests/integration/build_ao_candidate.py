"""Build a private Windows Release executable for the XeGTAO AO candidate.

This is an opt-in desktop candidate builder, not a release switch.  The
normal Release executable and its object cache remain the source of truth;
all candidate objects and the linked executable live below ``build/`` in a
separate tree.  The production AO selector is not changed by this script.

Importing this module is intentionally side-effect free.  Call :func:`main`
explicitly (or run the file as a script) to invoke ``tools/build.py``.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any, Iterable, Mapping


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "build" / "ao-xegtao-candidate"
CANDIDATE_EXE = OUTPUT / "release" / "mp6native.exe"
PROVENANCE = OUTPUT / "provenance.json"
PROTECTED_EXE = ROOT / "build" / "release" / "mp6native.exe"


def sha256(path: Path) -> str:
    """Return the SHA-256 digest of *path* without invoking any build tools."""

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_selector_policy() -> dict[str, Any]:
    """Describe the runtime selector without mutating the process environment."""

    return {
        "default": "xegtao",
        "android_default": "legacy",
        "default_policy": {"quality": "high", "denoise": 2,
                           "resolution": "full", "depth_storage": "r32float"},
        "opt_in": {
            "implementation": "xegtao",
            "quality": "ultra",
            "denoise": 3,
            "resolution": ["full", "half"],
            "depth_storage": {"default": "r32float", "experimental": "r16float",
                              "fallback": "r32float when the enabled device lacks TextureFormatsTier1"},
            "environment": {
                "MP6_AO_XEGTAO": "1",
                "MP6_AO_XEGTAO_QUALITY": "ultra",
                "MP6_AO_XEGTAO_DENOISE": "3",
                "MP6_AO_XEGTAO_RESOLUTION": ["full", "half"],
                "MP6_AO_XEGTAO_DEPTH_STORAGE": ["r32float", "r16float"],
            },
        },
    }


def _replace_path(value: str, replacements: Iterable[tuple[str, str]]) -> str:
    """Replace path-bearing command/depfile tokens in both slash styles."""

    for old, new in replacements:
        if not old:
            continue
        variants = (old, old.replace("\\", "/"), old.replace("/", "\\"))
        for variant in variants:
            value = value.replace(variant, new)
    return value


def _rewrite_command_record(
    record: Mapping[str, Any],
    source_object: Path,
    candidate_object: Path,
) -> dict[str, Any]:
    """Retarget a release compile record while preserving its freshness data.

    ``build.compile_one`` derives the object destination from its module-level
    ``OBJ_DIR``.  A copied release record therefore needs only its generated
    output paths retargeted.  Generated-source path changes are rejected by
    the caller, so source/dependency identity is never manufactured here.  The
    normal build driver's command/dependency comparison still decides whether
    the copied object is fresh.
    """

    result = json.loads(json.dumps(record))
    old_object = str(source_object)
    new_object = str(candidate_object)
    old_depfile = old_object + ".d"
    new_depfile = new_object + ".d"

    command = result.get("command")
    if not isinstance(command, list):
        return result
    # The caller rejects a generated-source path change before reaching this
    # helper.  Do not rewrite it here: leaving source/dependency identity
    # untouched is what makes a changed generated file force recompilation.
    replacements = ((old_depfile, new_depfile), (old_object, new_object))
    result["command"] = [
        _replace_path(str(value), replacements) if isinstance(value, str) else value
        for value in command
    ]

    dependencies = result.get("dependencies")
    if isinstance(dependencies, list):
        for dependency in dependencies:
            if isinstance(dependency, dict) and isinstance(dependency.get("path"), str):
                dependency["path"] = _replace_path(dependency["path"], replacements)

    # build._command_record hashes only this command-authentication payload;
    # dependencies are checked independently by needs_rebuild().
    encoded = json.dumps(
        {key: result[key] for key in
         ("contract", "command", "compiler", "compat_headers") if key in result},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    result["fingerprint"] = hashlib.sha256(encoded).hexdigest()
    return result


def _seed_cached_object(
    unit: tuple[Any, ...], original_objects: Path, candidate_objects: Path
) -> bool:
    """Copy one complete release cache entry into the private object tree.

    The copy is deliberately conservative: an existing candidate entry is
    never overwritten, and incomplete/malformed release entries are left for
    ``build.py`` to compile normally.  The build driver's own freshness check
    remains authoritative after the copy.
    """

    if len(unit) < 3:
        return False
    source, _flags, name = unit[:3]
    source_object = original_objects / str(name)
    candidate_object = candidate_objects / str(name)
    source_depfile = Path(str(source_object) + ".d")
    source_record = Path(str(source_object) + ".cmd.json")
    candidate_depfile = Path(str(candidate_object) + ".d")
    candidate_record = Path(str(candidate_object) + ".cmd.json")

    if candidate_object.exists() or candidate_depfile.exists() or candidate_record.exists():
        return False
    if not (source_object.is_file() and source_depfile.is_file() and source_record.is_file()):
        return False
    if source_object.resolve() == candidate_object.resolve():
        raise RuntimeError("candidate object directory aliases the Release cache")

    try:
        record = json.loads(source_record.read_text(encoding="utf-8"))
        command = record.get("command")
        if not isinstance(command, list) or "-c" not in command:
            return False
        old_source = Path(command[command.index("-c") + 1]).resolve()
        current_source = Path(source).resolve()
        # Generated sources (notably mdmini_data.c) move with BUILD_DIR.  A
        # copied record for a different path could look fresh while referring
        # to a different generated file, so conservatively compile it there.
        if old_source != current_source:
            return False
        rewritten = _rewrite_command_record(
            record, source_object, candidate_object
        )
    except (OSError, UnicodeDecodeError, TypeError, ValueError, json.JSONDecodeError):
        # Do not invent metadata for a cache entry the normal driver cannot
        # authenticate; a subsequent compile will repair it in the candidate.
        return False

    candidate_objects.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_object, candidate_object)
    depfile_text = source_depfile.read_text(encoding="utf-8", errors="surrogateescape")
    depfile_replacements = ((str(source_object) + ".d", str(candidate_object) + ".d"),
                            (str(source_object), str(candidate_object)))
    candidate_depfile.write_text(
        _replace_path(depfile_text, depfile_replacements),
        encoding="utf-8",
        errors="surrogateescape",
        newline="",
    )
    candidate_record.write_text(
        json.dumps(rewritten, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return True


def _seed_cached_objects(
    units: Iterable[tuple[Any, ...]], original_objects: Path, candidate_objects: Path
) -> int:
    """Seed safe release entries and return the number copied."""

    return sum(
        _seed_cached_object(unit, original_objects, candidate_objects)
        for unit in units
    )


def _import_build() -> Any:
    """Import the build driver lazily so importing this module stays inert."""

    tools_root = str(ROOT / "tools")
    if tools_root not in sys.path:
        sys.path.insert(0, tools_root)
    import build  # type: ignore[import-not-found]

    return build


def _write_provenance(output: Path, executable: Path, builder: Path) -> Path:
    """Write candidate provenance after the private executable is complete."""

    executable_sha256 = sha256(executable)
    payload = {
        "release_asset": False,
        "candidate": "desktop-ao-xegtao",
        "configuration": "release",
        "executable": str(executable),
        "exe_sha256": executable_sha256,
        "sha256": executable_sha256,
        "source_selector_policy": source_selector_policy(),
        "source_transforms": False,
        "qa_probes": False,
        "builder_sha256": sha256(builder),
    }
    path = output / "provenance.json"
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")
    return path


def main(*, build_module: Any | None = None, output: Path | None = None) -> int:
    """Build the isolated Release candidate and write its provenance.

    ``build_module`` and ``output`` are test seams; normal callers should use
    the script entry point, which uses the repository build driver and the
    fixed ``build/ao-xegtao-candidate`` destination.
    """

    output_dir = Path(output) if output is not None else OUTPUT
    output_dir = output_dir.resolve()
    protected_root = PROTECTED_EXE.parent.resolve()
    if (output_dir == protected_root or protected_root in output_dir.parents or
            (output_dir / "release").resolve() == protected_root):
        raise ValueError("AO candidate output must stay outside build/release")
    # A failed explicit build must not leave a prior success receipt that a
    # later consumer could mistake for the new candidate.
    (output_dir / "provenance.json").unlink(missing_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    build = build_module if build_module is not None else _import_build()

    original_configure = build.configure_windows
    original_collect = build.collect_units
    original_build_dir = build.BUILD_DIR
    original_obj_dir = build.OBJ_DIR
    original_objects: Path | None = None

    def configure(configuration: str) -> None:
        nonlocal original_objects
        if configuration != "release":
            raise ValueError("the AO candidate requires the Release configuration")
        # Resolve the Release Aurora backend while BUILD_DIR still points at
        # the canonical tree.  Only then redirect executable/object output.
        original_configure(configuration)
        original_objects = Path(build.OBJ_DIR).resolve()
        build.BUILD_DIR = str(output_dir)
        build.OBJ_DIR = str(output_dir / "obj")

    def collect(*args: Any, **kwargs: Any) -> list[tuple[Any, ...]]:
        units = list(original_collect(*args, **kwargs))
        if original_objects is None:
            raise RuntimeError("Release configuration did not establish the object cache")
        _seed_cached_objects(units, original_objects, Path(build.OBJ_DIR))
        return units

    previous_argv = sys.argv
    previous_configure = build.configure_windows
    previous_collect = build.collect_units
    protected_before = sha256(PROTECTED_EXE) if PROTECTED_EXE.is_file() else None
    result: Any = 1
    try:
        build.configure_windows = configure
        build.collect_units = collect
        sys.argv = [str(Path(__file__).resolve()), "--configuration", "release"]
        result = build.main()
    finally:
        sys.argv = previous_argv
        build.configure_windows = previous_configure
        build.collect_units = previous_collect
        build.BUILD_DIR = original_build_dir
        build.OBJ_DIR = original_obj_dir

    return_code = 0 if result is None else int(result)
    if return_code != 0:
        return return_code
    executable = output_dir / "release" / "mp6native.exe"
    if not executable.is_file():
        raise RuntimeError("Release build completed without a candidate executable")
    protected_after = sha256(PROTECTED_EXE) if PROTECTED_EXE.is_file() else None
    if protected_after != protected_before:
        raise RuntimeError("candidate build modified build/release/mp6native.exe")
    _write_provenance(output_dir, executable, Path(__file__).resolve())
    print(f"Built isolated AO candidate (not a release asset): {executable}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
