"""Build an isolated XeGTAO ordered-buffer capture executable.

The normal Release, board-QA, and cadence-QA binaries are inputs only.  This
wrapper transforms ``build_board_qa.py`` in memory, writes private copies of
the two AO translation units below ``build/ao-buffer-qa``, and records enough
hashes in ``build-ready.json`` for the runner to reject stale output.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "build" / "ao-buffer-qa"
BACKEND_HEADER = ROOT / "tests" / "integration" / "ao_buffer_capture.hpp"
PRIVATE_NATIVE = OUTPUT / "xegtao" / "native.hpp"
PRIVATE_AMBIENT = OUTPUT / "ambient_occlusion.cpp"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"AO buffer builder seam expected once: {label} ({count})")
    return text.replace(old, new)


def _private_paths(output: Path) -> tuple[Path, Path]:
    return output / "xegtao" / "native.hpp", output / "ambient_occlusion.cpp"


def _assert_output_isolated(root: Path, output: Path) -> None:
    """Reject an output tree that aliases a protected executable tree."""

    resolved_output = output.resolve()
    protected_roots = (
        (root / "build" / "release").resolve(),
        (root / "build" / "board-qa").resolve(),
        (root / "build" / "board-cadence-qa").resolve(),
    )
    for protected in protected_roots:
        if resolved_output == protected or protected in resolved_output.parents:
            raise ValueError(f"AO buffer QA output aliases protected tree: {resolved_output}")


def transformed_builder() -> str:
    """Return the board builder with only the private AO seam substituted."""

    source = (ROOT / "tests" / "integration" / "build_board_qa.py").read_text(encoding="utf-8")
    source = _replace_once(
        source,
        "OUTPUT = ROOT / 'build/board-qa'",
        "OUTPUT = ROOT / 'build/ao-buffer-qa'",
        "board output",
    )

    # Keep the ambient probe's existing QA instrumentation and add the buffer
    # transform immediately before its generated source is written.  The
    # generated filename is private as well, so no board-QA source is reused.
    source = _replace_once(
        source,
        "            source = str(OUTPUT / 'ambient_occlusion_probe.cpp')",
        "            contents = instrument_ambient(contents, ao_buffer_native_path, ao_buffer_header_path)\n"
        "            source = str(OUTPUT / 'ambient_occlusion.cpp')",
        "ambient private source filename",
    )

    # Header-only backend calls need Aurora's WebGPU C++ header directory in
    # the private ambient compile.  This is deliberately limited to this QA
    # object's flags and does not change build.py or any production unit.
    flags_anchor = "            flags = [*flags, '-I'+str(ROOT/'src/gx'), '-DMP6_XEGTAO_QA_INVALID_SHADER']\n"
    flags_replacement = (
        "            flags = [*flags, '-I'+str(ROOT/'src/gx'), '-DMP6_XEGTAO_QA_INVALID_SHADER',\n"
        "                     '-I'+str(ROOT/'third_party/aurora/lib')]\n"
    )
    source = _replace_once(source, flags_anchor, flags_replacement, "ambient flags")
    return source


def _write_private_native(output: Path, instrument_native) -> Path:
    native_path, _ambient_path = _private_paths(output)
    native_path.parent.mkdir(parents=True, exist_ok=True)
    native_source = (ROOT / "src" / "gx" / "xegtao" / "native.hpp").read_text(encoding="utf-8")
    native_path.write_text(
        instrument_native(native_source, ROOT / "src" / "gx" / "xegtao"),
        encoding="utf-8",
        newline="\n",
    )
    return native_path


def _protected_paths(root: Path) -> tuple[Path, ...]:
    return (
        root / "build" / "release" / "mp6native.exe",
        root / "build" / "board-qa" / "release" / "mp6native.exe",
        root / "build" / "board-cadence-qa" / "release" / "mp6native.exe",
    )


def main() -> int:
    _assert_output_isolated(ROOT, OUTPUT)
    sys.path.insert(0, str(ROOT))
    from tests.integration.ao_buffer_capture_source import instrument_ambient, instrument_native

    native_path = _write_private_native(OUTPUT, instrument_native)
    code = transformed_builder()
    protected = {str(path): digest(path) for path in _protected_paths(ROOT) if path.is_file()}
    namespace = {
        "__file__": str(ROOT / "tests" / "integration" / "build_board_qa.py"),
        "__name__": "__main__",
        "instrument_ambient": instrument_ambient,
        "ao_buffer_native_path": str(native_path),
        "ao_buffer_header_path": str(BACKEND_HEADER),
    }
    result = 1
    try:
        exec(compile(code, namespace["__file__"], "exec"), namespace)
    except SystemExit as exc:
        result = int(exc.code or 0)
    finally:
        for name, expected in protected.items():
            path = Path(name)
            if not path.is_file() or digest(path) != expected:
                raise RuntimeError(f"protected executable changed: {name}")

    if result != 0:
        return result
    executable = OUTPUT / "release" / "mp6native.exe"
    ready_path = OUTPUT / "build-ready.json"
    if not executable.is_file() or not ready_path.is_file():
        raise RuntimeError("AO buffer QA build completed without executable/receipt")

    ready = json.loads(ready_path.read_text(encoding="utf-8"))
    generated = {
        str(native_path.relative_to(OUTPUT)).replace("\\", "/"): digest(native_path),
    }
    if (OUTPUT / "ambient_occlusion.cpp").is_file():
        generated["ambient_occlusion.cpp"] = digest(OUTPUT / "ambient_occlusion.cpp")
    protected_after = {name: digest(Path(name)) for name in protected}
    source_hashes = {
        "tests/integration/build_ao_buffer_qa.py": digest(Path(__file__).resolve()),
        "tests/integration/ao_buffer_capture_source.py": digest(Path(__file__).with_name("ao_buffer_capture_source.py")),
        "tests/integration/ao_buffer_capture.hpp": digest(BACKEND_HEADER),
    }
    ready["ao_buffers"] = {
        "executable": str(executable),
        "exe": str(executable),
        "exe_sha256": digest(executable),
        "sources": source_hashes,
        "source_hashes": source_hashes,
        "private_generated_sources": generated,
        "private_generated_hashes": generated,
        "protected_executables": protected_after,
        "protected_hashes": protected_after,
        "builder_sha256": digest(Path(__file__).resolve()),
        "stages": [[name, mip] for name, mip in __import__(
            "tests.integration.ao_buffer_capture_source", fromlist=["STAGES"]
        ).STAGES],
    }
    ready_path.write_text(json.dumps(ready, indent=2) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
