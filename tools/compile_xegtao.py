#!/usr/bin/env python3
"""Compile the pinned XeGTAO HLSL bridge to WebGPU WGSL.

This is a reproducible feasibility/toolchain probe, not a renderer build step.
The upstream XeGTAO math is kept in ``third_party/xegtao``.  The bridge only
provides resource bindings and the small renderer-independent noise/normal
adapter required by the upstream wrapper.

Slang currently lowers HLSL ``RWTexture2D`` to WGSL ``read_write`` storage
textures.  WebGPU's baseline storage-texture path needs write-only outputs for
these passes.  After compiling, this tool proves that every output UAV is only
used by ``textureStore`` (never ``textureLoad``), then changes only those
declarations to ``write`` access.  If that proof fails, compilation fails
without emitting an accepted artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "src" / "gx" / "xegtao" / "bridge.hlsl"
UPSTREAM_DIR = ROOT / "third_party" / "xegtao"
DEFAULT_OUTPUT_DIR = ROOT / "build" / "ao-upstream-toolchain" / "xegtao"
DEFAULT_TOOL_DIR = ROOT / "build" / "ao-upstream-toolchain"

UPSTREAM_COMMIT = "a5b1686c7ea37788eeb3576b5be47f7c03db532c"
SLANG_VERSION = "2026.19"
SLANG_URL = (
    "https://github.com/shader-slang/slang/releases/download/"
    f"v{SLANG_VERSION}/slang-{SLANG_VERSION}-windows-x86_64.zip"
)
SLANG_SHA256 = "fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33"

# The vendor files are intentionally checked after canonicalizing line endings
# and trimming terminal LF bytes. This matches the raw snapshot's text even
# when a checkout adds the conventional final newline.
UPSTREAM_FILES = {
    "XeGTAO.h": "c3a9ee2bcbd71b5e54a5438e8a803dc8118483e455bc920c15c30cb30a49eee1",
    "XeGTAO.hlsli": "30faf4f5013916c14694db20fe9ec7f0c786f2edac7610968f9391987115064e",
    "LICENSE": "d46617ae9890b74443e84a85bec3797664fe42712284a848a35bc4b14b77a2b6",
}

# SHA-256 values of the raw GitHub responses used when the pinned snapshot was
# recorded. GitHub served these files with CRLF; the vendored copies are
# normalized to LF, so ``UPSTREAM_FILES`` is the integrity check used locally.
UPSTREAM_RAW_FILES = {
    "XeGTAO.h": "c3a9ee2bcbd71b5e54a5438e8a803dc8118483e455bc920c15c30cb30a49eee1",
    "XeGTAO.hlsli": "30faf4f5013916c14694db20fe9ec7f0c786f2edac7610968f9391987115064e",
    "LICENSE": "1f3bfd6b628535f0f0e779e70c260cfdc1bb45bc8644d7108e65f83e24a05cba",
}

STAGES = (
    "CSPrefilterDepths16x16",
    "CSGenerateNormals",
    "CSGTAOLow",
    "CSGTAOMedium",
    "CSGTAOHigh",
    "CSGTAOUltra",
    "CSDenoisePass",
    "CSDenoiseLastPass",
)

DEPTH_STORAGE_FORMATS = ("r32float", "r16float")
DEFAULT_R16_OUTPUT_DIR = ROOT / "build" / "ao-upstream-toolchain" / "xegtao-r16"
R16_STORAGE_DEFINE = "MP6_XEGTAO_R16_STORAGE"
R32_DEPTH_BIAS = 0.99999
R16_DEPTH_BIAS = 0.99920
PREFILTER_DEPTH_OUTPUTS = tuple(f"g_outWorkingDepthMIP{i}" for i in range(5))
PREFILTER_DEPTH_BINDINGS = tuple(range(1, 6))
MAIN_DEPTH_STAGES = tuple(f"CSGTAO{name}" for name in ("Low", "Medium", "High", "Ultra"))

_STORAGE_DECL_RE = re.compile(
    r"@binding\(\s*(?P<binding>\d+)\s*\)\s*"
    r"@group\(\s*(?P<group>\d+)\s*\)\s*"
    r"var\s+(?P<name>[A-Za-z_]\w*)\s*:\s*"
    r"texture_storage_2d\s*<\s*(?P<format>[A-Za-z0-9_]+)\s*,\s*"
    r"(?P<access>[A-Za-z0-9_-]+)\s*>\s*;",
    re.MULTILINE,
)
_DEPTH_BIAS_RE = re.compile(r"(?<![A-Za-z0-9_])0\.999\d+(?:f)?")


class ToolchainError(RuntimeError):
    """A deterministic toolchain or generated-code validation failure."""


def normalized_sha256(path: Path) -> str:
    data = path.read_bytes().replace(b"\r\n", b"\n").rstrip(b"\n")
    return hashlib.sha256(data).hexdigest()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_upstream() -> None:
    if not BRIDGE.is_file():
        raise ToolchainError(f"missing bridge: {BRIDGE}")
    for name, expected in UPSTREAM_FILES.items():
        path = UPSTREAM_DIR / name
        if not path.is_file():
            raise ToolchainError(f"missing pinned upstream file: {path}")
        actual = normalized_sha256(path)
        if actual != expected:
            raise ToolchainError(
                f"pinned XeGTAO file changed: {path} ({actual} != {expected})"
            )


def verify_slangc_version(path: Path) -> Path:
    try:
        version = subprocess.run(
            [str(path), "-version"],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ToolchainError(f"unable to query Slang version from {path}: {exc}") from exc
    reported = (version.stdout or version.stderr).strip().splitlines()
    if version.returncode != 0 or not reported or reported[0].strip() != SLANG_VERSION:
        detail = reported[0].strip() if reported else "no version output"
        raise ToolchainError(
            f"slangc version mismatch for {path}: {detail!r} (expected {SLANG_VERSION})"
        )
    return path


def find_slangc(explicit: Path | None, tool_dir: Path) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit)
    candidates.extend(
        (
            tool_dir / f"slang-{SLANG_VERSION}" / "bin" / "slangc.exe",
            tool_dir / "slang" / "bin" / "slangc.exe",
        )
    )
    path_env = os.environ.get("SLANGC")
    if path_env:
        candidates.append(Path(path_env))
    for candidate in candidates:
        if candidate.is_file():
            return verify_slangc_version(candidate.resolve())
    command = shutil.which("slangc")
    if command:
        return verify_slangc_version(Path(command).resolve())
    raise ToolchainError("slangc.exe was not found")


def download_slang(tool_dir: Path, timeout: float) -> Path:
    tool_dir.mkdir(parents=True, exist_ok=True)
    archive = tool_dir / f"slang-{SLANG_VERSION}-windows-x86_64.zip"
    if archive.exists():
        actual = sha256(archive)
        if actual != SLANG_SHA256:
            raise ToolchainError(
                f"Slang archive digest mismatch: {archive} ({actual} != {SLANG_SHA256})"
            )
    else:
        fd, temporary_name = tempfile.mkstemp(
            prefix="slang-", suffix=".zip.part", dir=tool_dir
        )
        os.close(fd)
        temporary = Path(temporary_name)
        try:
            with urllib.request.urlopen(SLANG_URL, timeout=timeout) as response:
                with temporary.open("wb") as output:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
            actual = sha256(temporary)
            if actual != SLANG_SHA256:
                raise ToolchainError(
                    f"downloaded Slang digest mismatch ({actual} != {SLANG_SHA256})"
                )
            os.replace(temporary, archive)
        finally:
            temporary.unlink(missing_ok=True)

    install_dir = tool_dir / f"slang-{SLANG_VERSION}"
    slangc = install_dir / "bin" / "slangc.exe"
    if not slangc.is_file():
        temporary_dir = Path(tempfile.mkdtemp(prefix="slang-extract-", dir=tool_dir))
        try:
            with zipfile.ZipFile(archive) as package:
                root = temporary_dir.resolve()
                for member in package.infolist():
                    target = (temporary_dir / member.filename).resolve()
                    if os.path.commonpath((str(root), str(target))) != str(root):
                        raise ToolchainError(
                            f"Slang archive contains an unsafe path: {member.filename!r}"
                        )
                package.extractall(temporary_dir)
            if install_dir.exists():
                raise ToolchainError(
                    f"Slang install directory exists but is incomplete: {install_dir}"
                )
            os.replace(temporary_dir, install_dir)
        finally:
            if temporary_dir.exists():
                shutil.rmtree(temporary_dir, ignore_errors=True)
    if not slangc.is_file():
        raise ToolchainError(f"Slang archive did not contain {slangc}")
    return verify_slangc_version(slangc.resolve())


def validate_depth_storage(depth_storage: str) -> str:
    """Validate the accepted working-depth storage format selector."""

    if depth_storage not in DEPTH_STORAGE_FORMATS:
        choices = ", ".join(DEPTH_STORAGE_FORMATS)
        raise ToolchainError(f"depth storage must be one of {choices}; got {depth_storage!r}")
    return depth_storage


def _storage_declarations(source: str) -> list[dict[str, Any]]:
    declarations: list[dict[str, Any]] = []
    for match in _STORAGE_DECL_RE.finditer(source):
        declarations.append(
            {
                "name": match.group("name"),
                "baseName": re.sub(r"_\d+$", "", match.group("name")),
                "group": int(match.group("group")),
                "binding": int(match.group("binding")),
                "format": match.group("format"),
                "access": match.group("access"),
            }
        )
    return declarations


def _assert_store_only(source: str, variable: str) -> None:
    """Prove a generated output variable is only declaration/textureStore-used."""

    declaration_spans = [
        match.span(1)
        for match in re.finditer(rf"\bvar\s+({re.escape(variable)})\s*:", source)
    ]
    store_spans = [
        match.span(1)
        for match in re.finditer(
            rf"\btextureStore\s*\(\s*(?:\(\s*)?({re.escape(variable)})\b", source
        )
    ]
    if len(declaration_spans) != 1 or not store_spans:
        raise ToolchainError(
            f"{variable} must have one declaration and at least one textureStore; "
            f"declarations={len(declaration_spans)} stores={len(store_spans)}"
        )
    allowed = set(declaration_spans + store_spans)
    for match in re.finditer(rf"\b{re.escape(variable)}\b", source):
        if match.span() not in allowed:
            raise ToolchainError(f"{variable} escapes the declaration/textureStore proof")


def _prefilter_depth_contract(source: str, expected_format: str) -> dict[str, Any]:
    """Validate all five group-1 depth UAV declarations and their stores."""

    declarations = _storage_declarations(source)
    by_base = {item["baseName"]: item for item in declarations}
    if len(by_base) != len(declarations):
        raise ToolchainError("prefilter shader has duplicate storage declaration names")
    selected: list[dict[str, Any]] = []
    for name, binding in zip(PREFILTER_DEPTH_OUTPUTS, PREFILTER_DEPTH_BINDINGS):
        item = by_base.get(name)
        if item is None:
            raise ToolchainError(f"prefilter shader is missing {name}")
        expected = {"group": 1, "binding": binding, "format": expected_format, "access": "write"}
        actual = {key: item[key] for key in expected}
        if actual != expected:
            raise ToolchainError(f"unexpected {name} declaration: {actual}; expected {expected}")
        _assert_store_only(source, item["name"])
        selected.append(item)
    return {
        "stage": "CSPrefilterDepths16x16",
        "group": 1,
        "bindings": list(PREFILTER_DEPTH_BINDINGS),
        "format": expected_format,
        "declarations": [
            {
                "name": item["name"],
                "group": item["group"],
                "binding": item["binding"],
                "format": item["format"],
                "access": item["access"],
                "proof": "textureStore-only output",
            }
            for item in selected
        ],
    }


def adapt_prefilter_depth_storage(source: str) -> tuple[str, dict[str, Any]]:
    """Convert exactly the five prefilter depth UAVs from R32 to R16."""

    contract = _prefilter_depth_contract(source, "r32float")
    target_names = {item["name"] for item in contract["declarations"]}

    def replace(match: re.Match[str]) -> str:
        if match.group("name") not in target_names:
            return match.group(0)
        return match.group(0).replace(match.group("format"), "r16float", 1)

    transformed, _ = _STORAGE_DECL_RE.subn(replace, source)
    changed = [
        item
        for item in _storage_declarations(transformed)
        if item["baseName"] in PREFILTER_DEPTH_OUTPUTS and item["format"] == "r16float"
    ]
    if len(changed) != 5:
        raise ToolchainError(f"expected exactly five R16 prefilter declarations, found {len(changed)}")
    _prefilter_depth_contract(transformed, "r16float")
    if any(
        item["baseName"] not in PREFILTER_DEPTH_OUTPUTS and item["format"] == "r16float"
        for item in _storage_declarations(transformed)
    ):
        raise ToolchainError("R16 storage adaptation escaped the five prefilter depth UAVs")
    contract["baselineFormat"] = "r32float"
    contract["format"] = "r16float"
    contract["declarations"] = [
        {
            "name": item["name"],
            "group": item["group"],
            "binding": item["binding"],
            "baselineFormat": "r32float",
            "format": "r16float",
            "access": item["access"],
            "proof": "textureStore-only output; declaration-only format adaptation",
        }
        for item in changed
    ]
    return transformed, contract


def verify_depth_bias(output_dir: Path, expected: float) -> None:
    """Require the upstream format-specific center-depth bias in all main stages."""

    for stage in MAIN_DEPTH_STAGES:
        path = output_dir / f"{stage}.wgsl"
        if not path.is_file():
            raise ToolchainError(f"missing generated main stage for depth-bias proof: {path}")
        values = [float(value.rstrip("f")) for value in _DEPTH_BIAS_RE.findall(path.read_text(encoding="utf-8"))]
        if not any(abs(value - expected) <= 0.00002 for value in values):
            raise ToolchainError(
                f"{stage} does not contain expected upstream depth bias {expected:.5f}; found {values}"
            )


def run_compiler(
    slangc: Path,
    stage: str,
    output_dir: Path,
    timeout: float,
    depth_storage: str = "r32float",
) -> tuple[Path, Path, str]:
    depth_storage = validate_depth_storage(depth_storage)
    raw_wgsl = output_dir / f"{stage}.raw.wgsl"
    reflection = output_dir / f"{stage}.reflection.json"
    log_path = output_dir / f"{stage}.log"
    command = [
        str(slangc),
        str(BRIDGE),
        *([f"-D{R16_STORAGE_DEFINE}=1"] if depth_storage == "r16float" else []),
        "-target",
        "wgsl",
        "-entry",
        stage,
        "-stage",
        "compute",
        "-O2",
        "-I",
        str(UPSTREAM_DIR),
        "-o",
        str(raw_wgsl),
        "-reflection-json",
        str(reflection),
    ]
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        log_path.write_text(
            f"timeout after {timeout:.1f}s\ncommand: {command}\n{error}\n",
            encoding="utf-8",
        )
        raise ToolchainError(f"Slang timed out compiling {stage}") from error

    log_path.write_text(completed.stdout or "", encoding="utf-8")
    if completed.returncode != 0:
        raise ToolchainError(
            f"Slang failed compiling {stage} (exit {completed.returncode}); see {log_path}"
        )
    if not raw_wgsl.is_file() or not reflection.is_file():
        raise ToolchainError(f"Slang did not emit both outputs for {stage}")
    return raw_wgsl, reflection, " ".join(command)


def binding_group(binding: dict[str, Any]) -> int:
    return int(binding.get("space", 0))


def binding_index(binding: dict[str, Any]) -> int:
    return int(binding["index"])


def used_bindings(reflection: dict[str, Any]) -> list[dict[str, Any]]:
    entries = reflection.get("entryPoints", [])
    if len(entries) != 1:
        raise ToolchainError("expected exactly one reflected entry point")
    return [
        item
        for item in entries[0].get("bindings", [])
        if int(item.get("binding", {}).get("used", 0))
    ]


def wgsl_binding_var(source: str, name: str) -> str:
    match = re.search(rf"\bvar\s+({re.escape(name)}(?:_\d+)?)\s*:", source)
    if not match:
        raise ToolchainError(f"reflection binding {name} has no WGSL declaration")
    return match.group(1)


def make_write_only(source: str, reflection: dict[str, Any]) -> tuple[str, list[dict[str, Any]]]:
    outputs: list[dict[str, Any]] = []
    for item in used_bindings(reflection):
        name = str(item["name"])
        if not name.startswith("g_out"):
            continue
        variable = wgsl_binding_var(source, name)
        variable_pattern = re.escape(variable)
        token_pattern = re.compile(rf"\b{variable_pattern}\b")
        declaration_tokens = [
            match.span(1)
            for match in re.finditer(
                rf"\bvar\s+({variable_pattern})\s*:\s*[^;]+;", source
            )
        ]
        store_tokens = [
            match.span(1)
            for match in re.finditer(
                rf"\btextureStore\s*\(\s*(?:\(\s*)?({variable_pattern})\b",
                source,
            )
        ]
        if len(declaration_tokens) != 1:
            raise ToolchainError(
                f"expected one declaration token for {name}, found {len(declaration_tokens)}"
            )
        for token in token_pattern.finditer(source):
            span = token.span()
            if span in declaration_tokens or span in store_tokens:
                continue
            raise ToolchainError(
                f"{name} token escapes declaration/textureStore whitelist in generated WGSL"
            )

        declaration = re.compile(
            rf"(\bvar\s+{variable_pattern}\s*:\s*texture_storage_2d<[^,>]+,\s*)read_write(\s*>)"
        )
        source, count = declaration.subn(r"\1write\2", source, count=1)
        if count != 1:
            raise ToolchainError(f"expected one read_write storage declaration for {name}")
        output = dict(item)
        output["wgslVariable"] = variable
        output["storageAccess"] = "write"
        outputs.append(output)
    return source, outputs


def clamp_scalar_outputs(source: str, stage: str) -> tuple[str, bool]:
    """Preserve upstream R8_UINT saturation when the bridge uses R32_UINT.

    XeGTAO's reference integration stores scalar AO in R8_UINT, whose
    conversion saturates values above 255. The WebGPU bridge uses R32_UINT so
    the packed normal path remains lossless; integer stores do not saturate,
    so apply the equivalent clamp to every scalar AO store. This is
    intentionally a narrow, checked transformation rather than a change to
    the upstream AO math. (D3D11.3 functional spec, §3.2.3.13.)
    """
    scalar_names = {
        "g_outWorkingAOTerm_": "working",
        "g_outFinalAOTerm_": "final",
    }
    lines = source.splitlines(keepends=True)
    changed = False
    for index, line in enumerate(lines):
        if "textureStore" not in line:
            continue
        name = next((item for item in scalar_names if item in line), None)
        if name is None:
            continue
        needle = "vec4<u32>((u32("
        if line.count(needle) != 1:
            raise ToolchainError(f"unexpected {scalar_names[name]} scalar AO store shape in {stage}")
        prefix, rest = line.split(needle, 1)
        end = rest.find(")),")
        if end < 0:
            raise ToolchainError(f"cannot locate scalar AO value in {stage}")
        expression = rest[:end]
        lines[index] = (
            prefix
            + "vec4<u32>((min(255u, u32("
            + expression
            + "))),"
            + rest[end + 3 :]
        )
        changed = True
    if stage in {"CSGTAOLow", "CSGTAOMedium", "CSGTAOHigh", "CSGTAOUltra", "CSDenoisePass", "CSDenoiseLastPass"} and not changed:
        raise ToolchainError(f"expected a scalar AO store in {stage}")
    return "".join(lines), changed


def compact_binding(item: dict[str, Any]) -> dict[str, Any]:
    binding = item.get("binding", {})
    result: dict[str, Any] = {
        "name": item["name"],
        "group": binding_group(binding),
        "binding": binding_index(binding),
        "used": bool(binding.get("used", 0)),
    }
    if "format" in item:
        result["format"] = item["format"]
    if "storageAccess" in item:
        result["storageAccess"] = item["storageAccess"]
    return result


def compile_all(
    slangc: Path,
    output_dir: Path,
    timeout: float,
    depth_storage: str = "r32float",
) -> dict[str, Any]:
    depth_storage = validate_depth_storage(depth_storage)
    output_dir.mkdir(parents=True, exist_ok=True)
    entries: list[dict[str, Any]] = []
    for stage in STAGES:
        raw_wgsl, reflection_path, command = run_compiler(
            slangc, stage, output_dir, timeout, depth_storage
        )
        reflection = json.loads(reflection_path.read_text(encoding="utf-8"))
        source = raw_wgsl.read_text(encoding="utf-8")
        source, outputs = make_write_only(source, reflection)
        source, scalar_clamp = clamp_scalar_outputs(source, stage)
        prefilter_storage = None
        if stage == "CSPrefilterDepths16x16":
            if depth_storage == "r16float":
                source, prefilter_storage = adapt_prefilter_depth_storage(source)
            else:
                prefilter_storage = _prefilter_depth_contract(source, "r32float")
        wgsl_path = output_dir / f"{stage}.wgsl"
        wgsl_path.write_text(
            "// Generated from pinned XeGTAO HLSL via Slang; do not edit.\n" + source,
            encoding="utf-8",
        )

        entry_point = reflection["entryPoints"][0]
        bindings = [compact_binding(item) for item in entry_point.get("bindings", [])]
        for output in outputs:
            for binding in bindings:
                if binding["name"] == output["name"]:
                    binding["storageAccess"] = "write"
        if prefilter_storage is not None:
            for binding in bindings:
                if binding["name"] in PREFILTER_DEPTH_OUTPUTS:
                    binding["format"] = depth_storage
        entries.append(
            {
                "name": stage,
                "wgsl": wgsl_path.name,
                "reflection": reflection_path.name,
                "wgslSha256": sha256(wgsl_path),
                "threadGroupSize": entry_point.get("threadGroupSize"),
                "bindings": bindings,
                "scalarOutputClamp": scalar_clamp,
                "compilerCommand": command,
            }
        )

    expected_bias = R16_DEPTH_BIAS if depth_storage == "r16float" else R32_DEPTH_BIAS
    verify_depth_bias(output_dir, expected_bias)
    prefilter_entry = next(
        entry for entry in entries if entry["name"] == "CSPrefilterDepths16x16"
    )
    prefilter_storage = {
        "format": depth_storage,
        "group": 1,
        "bindings": list(PREFILTER_DEPTH_BINDINGS),
        "declarations": [
            {
                "name": binding["name"],
                "group": binding["group"],
                "binding": binding["binding"],
                "format": depth_storage,
                "access": binding.get("storageAccess", "write"),
                "proof": "textureStore-only output",
            }
            for binding in prefilter_entry["bindings"]
            if binding["name"] in PREFILTER_DEPTH_OUTPUTS
        ],
    }
    if len(prefilter_storage["declarations"]) != 5:
        raise ToolchainError("manifest prefilter storage metadata must contain exactly five depth UAVs")
    manifest = {
        "tool": "tools/compile_xegtao.py",
        "upstream": {
            "repository": "https://github.com/GameTechDev/XeGTAO",
            "commit": UPSTREAM_COMMIT,
            "files": UPSTREAM_FILES,
            "rawResponseSha256": UPSTREAM_RAW_FILES,
        },
        "compiler": {
            "name": "Slang",
            "version": SLANG_VERSION,
            "archiveSha256": SLANG_SHA256,
            "target": "wgsl",
        },
        "bridge": "src/gx/xegtao/bridge.hlsl",
        "depthStorageFormat": depth_storage,
        "depthStorageBias": expected_bias,
        "depthStorage": {
            "format": depth_storage,
            "arithmetic": "FP32",
            "arithmeticDefine": "#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0",
            "compileDefine": f"{R16_STORAGE_DEFINE}=1" if depth_storage == "r16float" else None,
            "depthBias": expected_bias,
            "baselineBias": R32_DEPTH_BIAS,
            "r16Bias": R16_DEPTH_BIAS,
            "prefilter": prefilter_storage,
            "canonicalRawDepthFormat": "r32float",
            "normalSourceFormat": "r32float",
            "edgeStorageFormat": "r32float",
        },
        "constants": {
            "name": "GTAOConstants",
            "reflectedSizeBytes": 96,
            "noiseIndex": 0,
            "temporalAccumulation": False,
        },
        "storageTexturePolicy": {
            "compilerOutput": "read_write",
            "finalOutput": "write",
            "workingDepthFormat": depth_storage,
            "workingDepthBias": expected_bias,
            "prefilterDepthBindings": list(PREFILTER_DEPTH_BINDINGS),
            "proof": "Every reflected g_out UAV is rejected if generated WGSL reads or samples it before the declaration is rewritten.",
            "scalarR8Compatibility": "Denoise scalar AO stores are clamped to 255 to match the upstream R8_UINT conversion while the bridge uses R32_UINT.",
        },
        "entries": entries,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def cpp_raw_literal(source: str) -> str:
    delimiter = "MP6_XEGTAO"
    while f"){delimiter}\"" in source:
        delimiter += "_"
    return f'R"{delimiter}(\n{source}){delimiter}"'


def emit_header(
    output_dir: Path,
    header_path: Path,
    manifest: dict[str, Any],
    namespace: str = "mp6::xegtao::shaders",
) -> None:
    """Embed accepted post-processed WGSL stages in a C++17 header."""
    if not namespace or any(not part.isidentifier() for part in namespace.split("::")):
        raise ToolchainError(f"invalid C++ shader namespace: {namespace!r}")
    entries = {str(entry["name"]): entry for entry in manifest.get("entries", [])}
    missing = [stage for stage in STAGES if stage not in entries]
    if missing:
        raise ToolchainError(f"manifest is missing WGSL stages for header: {', '.join(missing)}")

    lines = [
        "// Generated by tools/compile_xegtao.py; do not edit.",
        f"// Upstream XeGTAO commit: {UPSTREAM_COMMIT}",
        f"// Slang {SLANG_VERSION} -> WGSL; accepted write-only/saturation adaptations applied.",
        "#pragma once",
        "",
        f"namespace {namespace} {{",
        "",
    ]
    for stage in STAGES:
        entry = entries[stage]
        wgsl_path = output_dir / str(entry["wgsl"])
        if not wgsl_path.is_file():
            raise ToolchainError(f"missing accepted WGSL artifact for header: {wgsl_path}")
        expected_hash = str(entry.get("wgslSha256", ""))
        actual_hash = sha256(wgsl_path)
        if not expected_hash or actual_hash != expected_hash:
            raise ToolchainError(
                f"accepted WGSL digest changed for {stage}: {actual_hash} != {expected_hash}"
            )
        source = wgsl_path.read_text(encoding="utf-8")
        lines.append(f"inline constexpr char {stage}[] = {cpp_raw_literal(source)};")
        lines.append("")
    lines.append(f"}} // namespace {namespace}")
    lines.append("")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("\n".join(lines), encoding="utf-8")


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="artifact directory (defaults to xegtao or xegtao-r16 by storage mode)",
    )
    parser.add_argument("--slangc", type=Path, default=None)
    parser.add_argument(
        "--depth-storage",
        choices=DEPTH_STORAGE_FORMATS,
        default="r32float",
        help="working-depth storage format; r32float preserves the baseline",
    )
    parser.add_argument(
        "--download-slang",
        action="store_true",
        help="download the pinned Slang archive into the output toolchain directory",
    )
    parser.add_argument(
        "--header",
        type=Path,
        default=None,
        help="also emit accepted WGSL as inline constexpr C++ strings at this path",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="per-stage Slang timeout in seconds (maximum 30)",
    )
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        depth_storage = validate_depth_storage(args.depth_storage)
        if args.timeout <= 0 or args.timeout > 30.0:
            raise ToolchainError("--timeout must be in (0, 30] seconds")
        verify_upstream()
        output_dir = (
            args.output_dir
            if args.output_dir is not None
            else (DEFAULT_R16_OUTPUT_DIR if depth_storage == "r16float" else DEFAULT_OUTPUT_DIR)
        ).resolve()
        tool_dir = output_dir.parent
        if args.download_slang:
            slangc = download_slang(tool_dir, args.timeout)
        else:
            try:
                slangc = find_slangc(args.slangc, tool_dir)
            except ToolchainError:
                if args.slangc is not None or tool_dir == DEFAULT_TOOL_DIR:
                    raise
                # Private artifact directories may live anywhere below build;
                # reuse the pinned offline compiler installed beside the
                # canonical toolchain rather than searching the artifact tree.
                slangc = find_slangc(None, DEFAULT_TOOL_DIR)
        manifest = compile_all(slangc, output_dir, args.timeout, depth_storage)
        header_path = args.header
        namespace = "mp6::xegtao::shaders"
        if depth_storage == "r16float":
            namespace = "mp6::xegtao::r16_shaders"
            if header_path is None:
                header_path = ROOT / "src" / "gx" / "xegtao" / "generated_r16_shaders.hpp"
        if header_path is not None:
            header_path = header_path.resolve()
            emit_header(output_dir, header_path, manifest, namespace)
    except (OSError, ValueError, json.JSONDecodeError, ToolchainError) as error:
        print(f"compile_xegtao: error: {error}", file=sys.stderr)
        return 1
    print(f"compiled {len(manifest['entries'])} XeGTAO stages")
    print(f"outputs: {output_dir}")
    if header_path is not None:
        print(f"header: {header_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
