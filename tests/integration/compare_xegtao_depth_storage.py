"""Bounded offline R16 working-depth XeGTAO comparison.

This is an opt-in diagnostic for the native MP6 AO path.  It consumes the
production pinned-toolchain ``r16float`` generation mode, which compiles the
production bridge with its conditional depth selector and adapts only the
five prefilter working-depth storage declarations.  The generated variant is
compared with the existing FP32/R32 baseline through :mod:`test_xegtao_gpu`.

Importing this module is deliberately CPU-only: NumPy, wgpu and the optional
renderer are imported only from :func:`run_gpu_comparison` after the command
line has selected a backend.  The report is a quality/format probe, not a
performance or mobile-suitability claim.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = (ROOT / "build").resolve()
PRODUCTION_BRIDGE = ROOT / "src" / "gx" / "xegtao" / "bridge.hlsl"
DEFAULT_SHADER_DIR = ROOT / "build" / "ao-upstream-toolchain" / "xegtao"
DEFAULT_TOOL_DIR = ROOT / "build" / "ao-upstream-toolchain"

R16_FEATURE = "texture-adapter-specific-format-features"
COMPILER_TIMEOUT_SECONDS = 30.0
R16_BIAS = 0.99920
R32_BIAS = 0.99999
R16_STORAGE_DEFINE = "MP6_XEGTAO_R16_STORAGE"
DEPTH_STAGE = "CSPrefilterDepths16x16"
DEPTH_OUTPUTS = tuple(f"g_outWorkingDepthMIP{i}" for i in range(5))
DEPTH_BINDINGS = tuple(range(1, 6))
MAIN_STAGES = tuple(f"CSGTAO{name}" for name in ("Low", "Medium", "High", "Ultra"))

_FP32_DEFINE = "#define XE_GTAO_FP32_DEPTHS 1"
_HALF_DEFINE = "#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0"
_INCLUDE_LINES = {
    '#include "../../../third_party/xegtao/XeGTAO.h"': ROOT
    / "third_party"
    / "xegtao"
    / "XeGTAO.h",
    '#include "../../../third_party/xegtao/XeGTAO.hlsli"': ROOT
    / "third_party"
    / "xegtao"
    / "XeGTAO.hlsli",
}

_STORAGE_DECL_RE = re.compile(
    r"@binding\(\s*(?P<binding>\d+)\s*\)\s*"
    r"@group\(\s*(?P<group>\d+)\s*\)\s*"
    r"var\s+(?P<name>[A-Za-z_]\w*)\s*:\s*"
    r"texture_storage_2d\s*<\s*(?P<format>[A-Za-z0-9_]+)\s*,\s*"
    r"(?P<access>[A-Za-z0-9_-]+)\s*>\s*;",
    re.MULTILINE,
)
_FLOAT_LITERAL_RE = re.compile(r"(?<![A-Za-z0-9_])0\.999\d+(?:f)?")


class ProbeError(RuntimeError):
    """A deterministic probe setup or source-contract failure."""


class ToolchainUnavailable(ProbeError):
    """The pinned offline compiler or generated baseline is unavailable."""


class GpuUnavailable(ProbeError):
    """Optional GPU dependencies or the requested backend are unavailable."""


class R16StorageUnsupported(GpuUnavailable):
    """The adapter does not expose the required native R16 storage feature."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _normalise_feature_name(value: object) -> str:
    return str(value).strip().lower().replace("_", "-")


def _feature_supported(features: Iterable[object]) -> bool:
    return R16_FEATURE in {_normalise_feature_name(item) for item in features}


def validate_output_path(output: Path) -> Path:
    """Require a not-yet-created report path below this checkout's build tree."""

    resolved = output.resolve()
    try:
        resolved.relative_to(BUILD_ROOT)
    except ValueError as exc:
        raise ProbeError(f"--output must be a new path within {BUILD_ROOT}") from exc
    if resolved.exists():
        raise ProbeError(f"--output already exists; choose a new path: {resolved}")
    return resolved


def _variant_artifact_dir(output: Path) -> Path:
    artifact = output.parent / f"{output.stem}.r16-depth-storage"
    try:
        artifact.resolve().relative_to(BUILD_ROOT)
    except ValueError as exc:
        raise ProbeError(f"variant artifact path escapes {BUILD_ROOT}: {artifact}") from exc
    if artifact.exists():
        raise ProbeError(f"variant artifact directory already exists: {artifact}")
    return artifact


def variant_bridge_text(production_source: str, vendor_root: Path | None = None) -> str:
    """Return the historical private-bridge transform for CPU proof tests.

    R16 working storage needs the upstream FP16-depth bias while preserving
    FP32 arithmetic.  Removing the selector is therefore intentional; the
    half-float arithmetic define remains explicitly set to zero.  The
    production probe path does *not* call this helper: it uses the checked-in
    conditional bridge and ``compile_all(..., "r16float")``.  The vendor source
    itself is never copied or edited by this historical proof transformation.
    """

    if production_source.count(_FP32_DEFINE) != 1:
        raise ProbeError("production bridge must contain exactly one FP32-depth define")
    if production_source.count(_HALF_DEFINE) != 1:
        raise ProbeError("production bridge must retain exactly one FP32-arithmetic define")

    root = (vendor_root or (ROOT / "third_party" / "xegtao")).resolve()
    replacements = {
        old: f'#include "{(root / path.name).as_posix()}"'
        for old, path in _INCLUDE_LINES.items()
    }
    result: list[str] = []
    removed = 0
    include_counts = {old: 0 for old in replacements}
    for line in production_source.splitlines(keepends=True):
        newline = "\r\n" if line.endswith("\r\n") else "\n" if line.endswith("\n") else ""
        body = line[: -len(newline)] if newline else line
        if body == _FP32_DEFINE:
            removed += 1
            continue
        if body in replacements:
            include_counts[body] += 1
            result.append(replacements[body] + newline)
        else:
            result.append(line)
    transformed = "".join(result)
    if removed != 1 or _FP32_DEFINE in transformed:
        raise ProbeError("failed to remove the exact XE_GTAO_FP32_DEPTHS selector")
    if _HALF_DEFINE not in transformed:
        raise ProbeError("R16 bridge lost XE_GTAO_USE_HALF_FLOAT_PRECISION 0")
    if any(count != 1 for count in include_counts.values()):
        raise ProbeError(f"expected one replacement for each vendor include: {include_counts}")
    for old, replacement in replacements.items():
        if old in transformed or replacement not in transformed:
            raise ProbeError(f"absolute include replacement failed for {old}")
    return transformed


def _validate_production_bridge(source: str) -> None:
    """Prove the production bridge exposes the R16 selector contract.

    The old probe wrote a private bridge with the FP32 selector removed.  The
    production compiler now owns that source variation: the selector remains
    in the checked-in bridge and is disabled only when
    ``MP6_XE_GTAO_R16_STORAGE`` is defined.  Keep this source proof local so
    a stale bridge cannot silently produce an R16 artifact with R32 bias.
    """

    if source.count(_FP32_DEFINE) != 1:
        raise ProbeError("production bridge must contain exactly one FP32-depth define")
    if source.count(_HALF_DEFINE) != 1:
        raise ProbeError("production bridge must retain exactly one FP32-arithmetic define")
    guard = re.compile(
        r"#if\s*!defined\(\s*"
        + re.escape(R16_STORAGE_DEFINE)
        + r"\s*\)\s*\r?\n\s*"
        + re.escape(_FP32_DEFINE)
    )
    if guard.search(source) is None:
        raise ProbeError(
            "production bridge must conditionally define XE_GTAO_FP32_DEPTHS "
            f"when {R16_STORAGE_DEFINE} is absent"
        )
    for include in _INCLUDE_LINES:
        if include not in source:
            raise ProbeError(f"production bridge lost pinned vendor include: {include}")


def create_variant_bridge(artifact_dir: Path, production_bridge: Path = PRODUCTION_BRIDGE) -> dict[str, Any]:
    """Return provenance for the unchanged production bridge.

    ``artifact_dir`` is intentionally not created here.  The caller creates
    the fresh artifact directory immediately before compilation, while the
    compiler itself always reads ``src/gx/xegtao/bridge.hlsl``.  This keeps the
    report truthful and avoids the old private-bridge monkeypatch path.
    """

    if not production_bridge.is_file():
        raise ProbeError(f"missing production bridge: {production_bridge}")
    source = production_bridge.read_text(encoding="utf-8")
    _validate_production_bridge(source)
    resolved_bridge = production_bridge.resolve()
    source_hash = sha256(resolved_bridge)
    return {
        "path": str(resolved_bridge),
        "sourceSha256": source_hash,
        "productionSourceSha256": source_hash,
        # Preserve the prior report key as the selector identity.  It is not
        # removed from source; the explicit action below records that R16
        # disables it via the production compiler define.
        "removedDefine": _FP32_DEFINE,
        "removedDefineAction": f"retained; disabled by {R16_STORAGE_DEFINE}=1",
        "arithmeticDefine": _HALF_DEFINE,
        "compileDefine": f"{R16_STORAGE_DEFINE}=1",
        "sourceMode": "production-conditional-bridge",
        "includeRoot": str((ROOT / "third_party" / "xegtao").resolve()),
    }


def _load_compile_tool():
    tool_path = ROOT / "tools" / "compile_xegtao.py"
    spec = importlib.util.spec_from_file_location("mp6_probe_compile_xegtao", tool_path)
    if spec is None or spec.loader is None:
        raise ToolchainUnavailable(f"cannot import compiler tool: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _assert_store_only(source: str, variable: str) -> None:
    """Prove an adapted output is declaration/textureStore-only."""

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
        raise ProbeError(
            f"{variable} must have one declaration and at least one textureStore; "
            f"declarations={len(declaration_spans)} stores={len(store_spans)}"
        )
    allowed = set(declaration_spans + store_spans)
    for match in re.finditer(rf"\b{re.escape(variable)}\b", source):
        if match.span() not in allowed:
            raise ProbeError(f"{variable} escapes the declaration/textureStore proof")


def _prefilter_declarations(source: str) -> list[dict[str, Any]]:
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


def rewrite_prefilter_storage(source: str) -> tuple[str, dict[str, Any]]:
    """Historical source-transform proof retained for CPU-only unit tests.

    Production generation now performs this adaptation inside
    ``tools/compile_xegtao.py``.  The comparison runner only validates the
    resulting declarations, so this helper is never applied to generated
    artifacts and cannot double-transform them.
    """

    declarations = _prefilter_declarations(source)
    by_base = {item["baseName"]: item for item in declarations}
    if len(by_base) != len(declarations):
        raise ProbeError("prefilter WGSL has duplicate storage declaration names")
    targets: list[dict[str, Any]] = []
    for index, (name, binding) in enumerate(zip(DEPTH_OUTPUTS, DEPTH_BINDINGS)):
        item = by_base.get(name)
        if item is None:
            raise ProbeError(f"prefilter WGSL is missing {name}")
        expected = {"group": 1, "binding": binding, "format": "r32float", "access": "write"}
        actual = {key: item[key] for key in expected}
        if actual != expected:
            raise ProbeError(f"unexpected {name} declaration: {actual}; expected {expected}")
        _assert_store_only(source, item["name"])
        targets.append(item)

    target_names = {item["name"] for item in targets}

    def replace(match: re.Match[str]) -> str:
        if match.group("name") not in target_names:
            return match.group(0)
        return match.group(0).replace(match.group("format"), "r16float", 1)

    transformed, count = _STORAGE_DECL_RE.subn(replace, source)
    if count < len(declarations):
        # ``count`` is every storage declaration, not every target.  It is
        # still useful as a proof that the regex saw all source declarations.
        raise ProbeError("storage declaration scan changed during R16 adaptation")
    target_after = _prefilter_declarations(transformed)
    changed = [
        item
        for item in target_after
        if item["baseName"] in DEPTH_OUTPUTS and item["format"] == "r16float"
    ]
    if len(changed) != 5:
        raise ProbeError(f"expected exactly five R16 prefilter declarations, found {len(changed)}")
    if any(
        item["baseName"] in DEPTH_OUTPUTS
        and item["format"] != "r16float"
        for item in target_after
    ):
        raise ProbeError("one or more prefilter depth outputs remained non-R16")
    if any(
        item["baseName"] not in DEPTH_OUTPUTS and item["format"] == "r16float"
        for item in target_after
    ):
        raise ProbeError("R16 adaptation escaped the five prefilter outputs")
    metadata = {
        "stage": DEPTH_STAGE,
        "group": 1,
        "bindings": list(DEPTH_BINDINGS),
        "declarations": [
            {
                "name": item["name"],
                "group": item["group"],
                "binding": item["binding"],
                "baselineFormat": "r32float",
                "variantFormat": "r16float",
                "access": item["access"],
                "proof": "textureStore-only output; declaration-only format adaptation",
            }
            for item in changed
        ],
    }
    return transformed, metadata


def _bias_values(source: str) -> list[float]:
    return [float(value.rstrip("f")) for value in _FLOAT_LITERAL_RE.findall(source)]


def assert_bias(shader_dir: Path, expected: float) -> None:
    """Check the format-specific upstream center-depth bias in main stages."""

    tolerance = 0.00002
    for stage in MAIN_STAGES:
        path = shader_dir / f"{stage}.wgsl"
        if not path.is_file():
            raise ToolchainUnavailable(f"missing generated main shader: {path}")
        values = _bias_values(path.read_text(encoding="utf-8"))
        matches = [value for value in values if abs(value - expected) <= tolerance]
        if not matches:
            raise ProbeError(f"{stage} does not contain expected depth bias {expected:.5f}: {values}")


def _validate_production_prefilter(
    shader_dir: Path, compile_manifest: dict[str, Any]
) -> dict[str, Any]:
    """Validate production ``compile_all(..., "r16float")`` output in place.

    This intentionally never rewrites WGSL.  The production compiler already
    performed the exact five-declaration adaptation and its write-only proof;
    this probe only rechecks those contracts before recording its legacy
    ``depthStorageProbe`` report section.
    """

    prefilter = shader_dir / f"{DEPTH_STAGE}.wgsl"
    if not prefilter.is_file():
        raise ToolchainUnavailable(f"missing generated prefilter shader: {prefilter}")
    source = prefilter.read_text(encoding="utf-8")
    declarations = _prefilter_declarations(source)
    by_base = {item["baseName"]: item for item in declarations}
    if len(by_base) != len(declarations):
        raise ProbeError("production prefilter WGSL has duplicate storage declaration names")

    targets: list[dict[str, Any]] = []
    for name, binding in zip(DEPTH_OUTPUTS, DEPTH_BINDINGS):
        item = by_base.get(name)
        if item is None:
            raise ProbeError(f"production prefilter WGSL is missing {name}")
        expected = {
            "group": 1,
            "binding": binding,
            "format": "r16float",
            "access": "write",
        }
        actual = {key: item[key] for key in expected}
        if actual != expected:
            raise ProbeError(
                f"production R16 prefilter declaration changed for {name}: "
                f"{actual}; expected {expected}"
            )
        _assert_store_only(source, item["name"])
        targets.append(item)
    target_bases = set(DEPTH_OUTPUTS)
    if any(
        item["baseName"] not in target_bases and item["format"] == "r16float"
        for item in declarations
    ):
        raise ProbeError("production R16 adaptation escaped the five prefilter outputs")

    storage = compile_manifest.get("depthStorage")
    if not isinstance(storage, dict):
        raise ToolchainUnavailable("production manifest is missing depthStorage metadata")
    if storage.get("format") != "r16float":
        raise ProbeError(f"production manifest depthStorage format is {storage.get('format')!r}")
    if storage.get("arithmetic") != "FP32":
        raise ProbeError("R16 storage probe must retain FP32 arithmetic")
    if storage.get("arithmeticDefine") != _HALF_DEFINE:
        raise ProbeError("production R16 manifest lost the FP32 arithmetic define")
    if storage.get("compileDefine") != f"{R16_STORAGE_DEFINE}=1":
        raise ProbeError("production R16 manifest lost its explicit storage compile define")
    if abs(float(storage.get("depthBias", -1.0)) - R16_BIAS) > 0.00002:
        raise ProbeError("production R16 manifest does not record the .99920 depth bias")
    if abs(float(storage.get("baselineBias", -1.0)) - R32_BIAS) > 0.00002:
        raise ProbeError("production R16 manifest does not record the .99999 baseline bias")
    if abs(float(storage.get("r16Bias", -1.0)) - R16_BIAS) > 0.00002:
        raise ProbeError("production R16 manifest has an inconsistent R16 bias")
    for field in ("canonicalRawDepthFormat", "normalSourceFormat", "edgeStorageFormat"):
        if storage.get(field) != "r32float":
            raise ProbeError(f"production R16 manifest changed canonical {field} from r32float")
    policy = compile_manifest.get("storageTexturePolicy")
    if not isinstance(policy, dict):
        raise ToolchainUnavailable("production manifest is missing storageTexturePolicy metadata")
    if policy.get("workingDepthFormat") != "r16float":
        raise ProbeError("production storage policy does not record r16float working depth")
    if abs(float(policy.get("workingDepthBias", -1.0)) - R16_BIAS) > 0.00002:
        raise ProbeError("production storage policy does not record the .99920 working-depth bias")
    try:
        policy_bindings = list(policy.get("prefilterDepthBindings", ()))
    except TypeError as exc:
        raise ProbeError("production storage policy prefilter bindings are malformed") from exc
    if policy_bindings != list(DEPTH_BINDINGS):
        raise ProbeError("production storage policy prefilter bindings are not 1..5")

    manifest_prefilter = storage.get("prefilter")
    if not isinstance(manifest_prefilter, dict):
        raise ToolchainUnavailable("production manifest is missing prefilter storage metadata")
    if manifest_prefilter.get("format") != "r16float":
        raise ProbeError("production manifest prefilter format is not r16float")
    try:
        manifest_bindings = list(manifest_prefilter.get("bindings", ()))
    except TypeError as exc:
        raise ProbeError("production manifest prefilter bindings are malformed") from exc
    if manifest_prefilter.get("group") != 1 or manifest_bindings != list(DEPTH_BINDINGS):
        raise ProbeError("production manifest prefilter bindings are not group 1 bindings 1..5")
    metadata_declarations = manifest_prefilter.get("declarations")
    if not isinstance(metadata_declarations, list) or len(metadata_declarations) != 5:
        raise ProbeError("production manifest prefilter metadata must contain exactly five declarations")
    try:
        metadata_bindings = [int(item["binding"]) for item in metadata_declarations]
    except (KeyError, TypeError, ValueError) as exc:
        raise ProbeError("production manifest prefilter declarations lack binding metadata") from exc
    if sorted(metadata_bindings) != list(DEPTH_BINDINGS):
        raise ProbeError("production manifest prefilter declarations are not bindings 1..5")
    metadata_names = {
        re.sub(r"_\d+$", "", str(item.get("name", "")))
        for item in metadata_declarations
        if isinstance(item, dict)
    }
    if metadata_names != set(DEPTH_OUTPUTS):
        raise ProbeError("production manifest prefilter declarations are not the five depth MIPs")
    if any(
        item.get("format") != "r16float"
        or item.get("group") != 1
        or item.get("binding") not in DEPTH_BINDINGS
        or item.get("access") != "write"
        for item in metadata_declarations
        if isinstance(item, dict)
    ) or any(not isinstance(item, dict) for item in metadata_declarations):
        raise ProbeError("production manifest prefilter declarations have an invalid R16 contract")

    format_metadata = {
        "stage": DEPTH_STAGE,
        "group": 1,
        "bindings": list(DEPTH_BINDINGS),
        "declarations": [
            {
                "name": item["name"],
                "group": item["group"],
                "binding": item["binding"],
                "baselineFormat": "r32float",
                "variantFormat": "r16float",
                "access": item["access"],
                "proof": "production compile_all declaration/textureStore-only output proof rechecked",
            }
            for item in targets
        ],
    }
    assert_bias(shader_dir, R16_BIAS)
    return format_metadata


def _rewrite_variant_artifacts(
    shader_dir: Path, bridge_metadata: dict[str, Any], compile_manifest: dict[str, Any]
) -> dict[str, Any]:
    """Finalize report metadata without changing production-generated WGSL.

    The function name is retained for the earlier probe's artifact contract;
    it is now a validator/finalizer rather than a source transformer.
    """

    format_metadata = _validate_production_prefilter(shader_dir, compile_manifest)
    hashes: dict[str, str] = {}
    for path in sorted(shader_dir.glob("*.wgsl")):
        hashes[path.name] = sha256(path)
    if not hashes:
        raise ToolchainUnavailable(f"production compiler emitted no WGSL files in {shader_dir}")
    for entry in compile_manifest.get("entries", []):
        wgsl_name = str(entry.get("wgsl", ""))
        expected = str(entry.get("wgslSha256", ""))
        actual = hashes.get(wgsl_name)
        if actual is None or expected != actual:
            raise ProbeError(
                f"production manifest hash mismatch for {wgsl_name or '<missing>'}"
            )

    compile_manifest["bridge"] = bridge_metadata["path"]
    compile_manifest["bridgeSourceSha256"] = bridge_metadata["sourceSha256"]
    compile_manifest["bridgeSourceMode"] = bridge_metadata.get(
        "sourceMode", "production-conditional-bridge"
    )
    compile_manifest["depthStorageProbe"] = {
        "arithmetic": "FP32",
        "arithmeticDefine": _HALF_DEFINE,
        "removedDepthDefine": _FP32_DEFINE,
        "removedDepthDefineAction": bridge_metadata.get(
            "removedDefineAction", f"retained; disabled by {R16_STORAGE_DEFINE}=1"
        ),
        "compileDefine": bridge_metadata.get("compileDefine", f"{R16_STORAGE_DEFINE}=1"),
        "bias": {"baselineR32": R32_BIAS, "variantR16": R16_BIAS},
        "formats": {
            "rawDepth": "r32float",
            "depthMipsBaseline": "r32float",
            "depthMipsVariant": "r16float",
            "packedNormals": "r32uint",
            "edges": "r32float",
        },
        "prefilter": format_metadata,
        "storageProof": (
            "production compile_all make_write_only plus "
            "declaration/textureStore-only recheck; no post-compile WGSL transform"
        ),
    }
    compile_manifest["shaderHashes"] = hashes
    manifest_path = shader_dir / "manifest.json"
    manifest_path.write_text(json.dumps(compile_manifest, indent=2) + "\n", encoding="utf-8")
    return compile_manifest


def compile_variant(
    artifact_dir: Path,
    bridge_metadata: dict[str, Any] | None = None,
    timeout: float = COMPILER_TIMEOUT_SECONDS,
) -> tuple[Path, dict[str, Any]]:
    """Compile the R16 variant through the production generator.

    ``bridge_metadata`` remains an input for report provenance and backwards
    compatibility with the first probe revision.  It is never installed into
    the compiler module: ``tools/compile_xegtao.py`` must use the checked-in
    production bridge and its explicit ``r16float`` mode.
    """

    if timeout <= 0 or timeout > COMPILER_TIMEOUT_SECONDS:
        raise ProbeError(f"compiler timeout must be in (0, {COMPILER_TIMEOUT_SECONDS:g}] seconds")
    if bridge_metadata is not None:
        bridge_path = Path(str(bridge_metadata.get("path", ""))).resolve()
        if bridge_path != PRODUCTION_BRIDGE.resolve():
            raise ProbeError(
                "R16 generation requires the checked-in production bridge; "
                "private bridge overrides are no longer supported"
            )
    artifact_dir = artifact_dir.resolve()
    try:
        artifact_dir.relative_to(BUILD_ROOT)
    except ValueError as exc:
        raise ProbeError(f"variant artifact path escapes {BUILD_ROOT}: {artifact_dir}") from exc
    if bridge_metadata is None:
        bridge_metadata = create_variant_bridge(artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=False)
    shader_dir = artifact_dir / "shaders"
    shader_dir.mkdir(parents=False, exist_ok=False)
    tool = _load_compile_tool()
    try:
        tool.verify_upstream()
        slangc = tool.find_slangc(None, DEFAULT_TOOL_DIR)
        manifest = tool.compile_all(slangc, shader_dir, timeout, "r16float")
    except (OSError, ValueError, json.JSONDecodeError, tool.ToolchainError) as exc:
        raise ToolchainUnavailable(str(exc)) from exc
    manifest = _rewrite_variant_artifacts(shader_dir, bridge_metadata, manifest)
    return shader_dir, manifest


class _FeatureAdapterProxy:
    """Narrow adapter wrapper that augments only request_device_sync kwargs."""

    def __init__(self, adapter: object):
        self._adapter = adapter

    def __getattr__(self, name: str):
        return getattr(self._adapter, name)

    def request_device_sync(self, *args, **kwargs):
        features = getattr(self._adapter, "features", ())
        if not _feature_supported(features):
            raise R16StorageUnsupported(
                f"adapter does not expose required native feature {R16_FEATURE!r}; "
                "R16 storage probe is explicitly skipped"
            )
        requested = list(kwargs.get("required_features") or ())
        if R16_FEATURE not in {_normalise_feature_name(item) for item in requested}:
            requested.append(R16_FEATURE)
        kwargs["required_features"] = requested
        return self._adapter.request_device_sync(*args, **kwargs)


class _FeatureGpuProxy:
    def __init__(self, gpu: object):
        self._gpu = gpu

    def __getattr__(self, name: str):
        return getattr(self._gpu, name)

    def request_adapter_sync(self, *args, **kwargs):
        adapter = self._gpu.request_adapter_sync(*args, **kwargs)
        if adapter is None:
            return None
        return _FeatureAdapterProxy(adapter)


@contextmanager
def feature_request_context(wgpu_module):
    """Temporarily wrap only ``wgpu.gpu``; restore the module on exit."""

    original = wgpu_module.gpu
    wgpu_module.gpu = _FeatureGpuProxy(original)
    try:
        yield
    finally:
        wgpu_module.gpu = original


def make_r16_renderer(base_module, shader_dir: Path):
    """Create a local Renderer subclass with only depth-MIP storage changed."""

    np = base_module.np
    wgpu = base_module.wgpu
    if np is None or wgpu is None:
        raise GpuUnavailable("numpy and wgpu==0.32.0 are required for the GPU probe")

    class R16Renderer(base_module.Renderer):
        def __init__(self, *args, **kwargs):
            # Keep the texture objects themselves alive while the renderer is
            # in use.  A permanent id set could misclassify a later fresh
            # texture after Python reuses an old wrapper's integer id.
            self._r16_texture_refs: list[object] = []
            with feature_request_context(wgpu):
                super().__init__(*args, **kwargs)

        def _is_r16_texture(self, texture) -> bool:
            return any(texture is candidate for candidate in self._r16_texture_refs)

        def _texture(self, size, format, usage, mip_levels=1):
            if int(mip_levels) == 5:
                if format != "r32float":
                    raise ProbeError(f"unexpected mip-chain format request: {format!r}")
                texture = super()._texture(size, "r16float", usage, mip_levels=mip_levels)
                self._r16_texture_refs.append(texture)
                return texture
            return super()._texture(size, format, usage, mip_levels=mip_levels)

        def _write(self, texture, values, size, mip_level=0):
            if self._is_r16_texture(texture):
                values = np.ascontiguousarray(np.asarray(values, dtype=np.float16))
            return super()._write(texture, values, size, mip_level=mip_level)

        def _read(self, texture, size, dtype, mip_level=0):
            if self._is_r16_texture(texture):
                values = super()._read(texture, size, np.float16, mip_level=mip_level)
                return np.ascontiguousarray(values.astype(np.float32))
            return super()._read(texture, size, dtype, mip_level=mip_level)

    return R16Renderer(shader_dir=shader_dir)


def _stats(np, values: Any) -> dict[str, float | int]:
    array = np.asarray(values, dtype=np.float32)
    finite = np.isfinite(array)
    if not finite.all():
        raise AssertionError("comparison produced non-finite values")
    return {
        "count": int(array.size),
        "mean": float(array.mean()),
        "p50": float(np.quantile(array, 0.50)),
        "p95": float(np.quantile(array, 0.95)),
        "max": float(array.max()),
    }


def _difference(np, baseline: Any, variant: Any) -> dict[str, float | int]:
    first = np.asarray(baseline, dtype=np.float32)
    second = np.asarray(variant, dtype=np.float32)
    if first.shape != second.shape:
        raise AssertionError(f"baseline/variant shape mismatch: {first.shape} != {second.shape}")
    return _stats(np, np.abs(second - first))


def _assert_result(np, result, label: str) -> None:
    for name in ("decoded_visibility", "packed_normals", "mip0", "raw_ao"):
        values = np.asarray(getattr(result, name))
        if not np.isfinite(values.astype(np.float32)).all():
            raise AssertionError(f"{label} has non-finite {name}")
    for level, mip in enumerate(result.depth_mips):
        if not np.isfinite(np.asarray(mip, dtype=np.float32)).all():
            raise AssertionError(f"{label} has non-finite depth mip {level}")
    visibility = np.asarray(result.decoded_visibility, dtype=np.float32)
    if float(visibility.min()) < 0.0 or float(visibility.max()) > 1.0:
        raise AssertionError(f"{label} visibility escaped [0,1]")


def _assert_deterministic(np, renderer, scene, label: str):
    fresh = renderer.render(scene, "Ultra", 3, reuse_resources=False)
    first = renderer.render(scene, "Ultra", 3, reuse_resources=True)
    repeated = renderer.render(scene, "Ultra", 3, reuse_resources=True)
    for name in ("decoded_visibility", "packed_normals", "raw_ao"):
        np.testing.assert_array_equal(
            np.asarray(getattr(fresh, name)).view(np.uint32),
            np.asarray(getattr(first, name)).view(np.uint32),
            err_msg=f"{label} fresh/reused {name}",
        )
        np.testing.assert_array_equal(
            np.asarray(getattr(first, name)).view(np.uint32),
            np.asarray(getattr(repeated, name)).view(np.uint32),
            err_msg=f"{label} reused/repeated {name}",
        )
    for level, (before, after) in enumerate(zip(fresh.depth_mips, first.depth_mips)):
        np.testing.assert_array_equal(
            np.asarray(before).view(np.uint32),
            np.asarray(after).view(np.uint32),
            err_msg=f"{label} fresh/reused depth mip {level}",
        )
    return fresh


def _scene_set(np, width: int, height: int):
    scalar_values = (
        ("near20", 20.0),
        ("flat400", 400.0),
        ("far10000", 10000.0),
        ("clear0", 0.0),
        ("fp16_max65504", 65504.0),
        ("over65504_70000", 70000.0),
        ("infinite", math.inf),
    )
    for name, value in scalar_values:
        yield name, np.full((height, width), value, dtype=np.float32)
    tilted, _ = _GPU_FIXTURES["plane"](width, height, distance=400.0)
    feet, _ = _GPU_FIXTURES["feet_scene"](width, height)
    yield "tilted_plane", np.asarray(tilted, dtype=np.float32)
    yield "feet", np.asarray(feet, dtype=np.float32)


def _world_statistics(np, values, common):
    stacked = 1.0 - np.stack(values)
    selected = stacked[:, common]
    region = common[2:-2, 2:-2].copy()
    coherent = np.zeros_like(stacked[:, 2:-2, 2:-2])
    for dy in range(5):
        for dx in range(5):
            region &= common[dy : dy + region.shape[0], dx : dx + region.shape[1]]
            coherent += stacked[:, dy : dy + region.shape[0], dx : dx + region.shape[1]] / 25.0
    return {
        "mean_darkening": float(selected.mean()),
        "std_mean": float(np.std(selected, axis=0).mean()),
        "step_max": float(np.abs(np.diff(selected, axis=0)).max()) if len(values) > 1 else 0.0,
        "coherent_std_mean": float(np.std(coherent[:, region], axis=0).mean()),
        "coherent_step_max": float(np.abs(np.diff(coherent[:, region], axis=0)).max()) if len(values) > 1 else 0.0,
    }


def _run_leaf_registration(np, baseline, variant, shifts: int):
    from test_ao_leaf_motion import leaf_scene, world_samples

    count = 8 if shifts == 0 else max(1, min(8, int(shifts)))
    base_fields: list[Any] = []
    variant_fields: list[Any] = []
    validities: list[Any] = []
    for shift in np.linspace(-0.7, 0.7, count):
        depth, _ground, coords, mask = leaf_scene(np, 640, 360, float(shift), 10.0)
        base_result = baseline.render(depth, "Ultra", 3, reuse_resources=True)
        variant_result = variant.render(depth, "Ultra", 3, reuse_resources=True)
        _assert_result(np, base_result, f"leaf baseline shift {shift:g}")
        _assert_result(np, variant_result, f"leaf variant shift {shift:g}")
        np.testing.assert_array_equal(base_result.packed_normals, variant_result.packed_normals)
        # Match analyze_ao_half_stages.py's MP6 final mapping: the upstream
        # visibility receives strength exactly once before world registration.
        base_visibility = 1.0 - 0.35 * (1.0 - base_result.visibility)
        variant_visibility = 1.0 - 0.35 * (1.0 - variant_result.visibility)
        base_samples, valid = world_samples(np, base_visibility, ~mask, coords)
        variant_samples, variant_valid = world_samples(np, variant_visibility, ~mask, coords)
        validities.extend((valid, variant_valid))
        base_fields.append(base_samples)
        variant_fields.append(variant_samples)
    common = np.logical_and.reduce(validities)
    if int(common.sum()) <= 1000:
        raise AssertionError(f"insufficient common world-registration receivers: {int(common.sum())}")
    base_values = np.stack(base_fields)[:, common]
    variant_values = np.stack(variant_fields)[:, common]
    return {
        "frames": count,
        "cameraShifts": [-0.7, 0.7],
        "leafLift": 10.0,
        "worldReceivers": int(common.sum()),
        "baseline": _world_statistics(np, base_fields, common),
        "variant": _world_statistics(np, variant_fields, common),
        "visibilityError": _difference(np, base_values, variant_values),
    }


def _compare_renderers(np, baseline, variant, samples: int):
    results: dict[str, Any] = {"scenes": [], "qualitySmallFeet": []}
    for width, height in ((129, 73), (640, 360)):
        for name, scene in _scene_set(np, width, height):
            base_result = _assert_deterministic(np, baseline, scene, f"baseline {name} {width}x{height}")
            variant_result = _assert_deterministic(np, variant, scene, f"variant {name} {width}x{height}")
            _assert_result(np, base_result, f"baseline {name} {width}x{height}")
            _assert_result(np, variant_result, f"variant {name} {width}x{height}")
            # Normals read from the canonical raw R32 source must remain bit exact;
            # R16 is allowed to differ only after the prefilter/depth-MIP boundary.
            np.testing.assert_array_equal(base_result.packed_normals, variant_result.packed_normals)
            results["scenes"].append(
                {
                    "name": name,
                    "size": [width, height],
                    "depthMips": [
                        _difference(np, base, actual)
                        for base, actual in zip(base_result.depth_mips, variant_result.depth_mips)
                    ],
                    "visibility": _difference(np, base_result.visibility, variant_result.visibility),
                    "rawAO": _difference(np, base_result.raw_ao, variant_result.raw_ao),
                    "packedNormals": {"exact": True},
                }
            )

    small_feet, _ = _GPU_FIXTURES["feet_scene"](129, 73)
    for quality in ("Low", "Medium", "High", "Ultra"):
        base_result = baseline.render(small_feet, quality, 3, reuse_resources=False)
        variant_result = variant.render(small_feet, quality, 3, reuse_resources=False)
        _assert_result(np, base_result, f"baseline feet {quality}")
        _assert_result(np, variant_result, f"variant feet {quality}")
        np.testing.assert_array_equal(base_result.packed_normals, variant_result.packed_normals)
        results["qualitySmallFeet"].append(
            {
                "quality": quality,
                "size": [129, 73],
                "visibility": _difference(np, base_result.visibility, variant_result.visibility),
                "packedNormals": {"exact": True},
            }
        )
    results["leafWorldRegistration"] = _run_leaf_registration(np, baseline, variant, samples)
    return results


_GPU_FIXTURES: dict[str, Any] = {}


def run_gpu_comparison(backend: str, shader_dir: Path, samples: int = 0) -> dict[str, Any]:
    """Run the opt-in baseline/variant comparison after imports are safe."""

    os.environ["WGPU_BACKEND_TYPE"] = backend
    sys.path[:0] = [str(ROOT / "build" / "ao-gpu-test-deps"), str(ROOT / "tests" / "integration"), str(ROOT / "tests")]
    try:
        import numpy as np
        import wgpu
        from test_ao_gpu import feet_scene, plane
        import test_xegtao_gpu as base_module
    except ImportError as exc:
        raise GpuUnavailable(f"optional NumPy/wgpu dependencies unavailable: {exc}") from exc
    if base_module.np is None or base_module.wgpu is None:
        raise GpuUnavailable("test_xegtao_gpu did not load NumPy/wgpu")
    _GPU_FIXTURES["plane"] = plane
    _GPU_FIXTURES["feet_scene"] = feet_scene
    try:
        baseline = base_module.Renderer(base_module.DEFAULT_SHADER_DIR)
    except getattr(base_module, "GpuUnavailable", RuntimeError) as exc:
        raise GpuUnavailable(str(exc)) from exc
    variant = make_r16_renderer(base_module, shader_dir)
    comparisons = _compare_renderers(np, baseline, variant, samples)
    if not variant._r16_texture_refs:
        raise ProbeError("R16 renderer did not allocate a five-level depth-MIP texture")
    return {
        "adapter": dict(variant.adapter.info),
        "feature": R16_FEATURE,
        "featureScope": "native adapter-specific R16Float storage/sampling; no tier-1 feature requested",
        "backend": backend,
        "comparisons": comparisons,
    }


def _baseline_metadata(shader_dir: Path) -> dict[str, Any]:
    manifest_path = shader_dir / "manifest.json"
    if not manifest_path.is_file():
        raise ToolchainUnavailable(f"missing baseline manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ToolchainUnavailable(f"cannot read baseline manifest: {manifest_path}") from exc
    hashes: dict[str, str] = {}
    for entry in manifest.get("entries", []):
        name = str(entry.get("wgsl", ""))
        expected = str(entry.get("wgslSha256", ""))
        path = shader_dir / name
        if not name or not path.is_file() or not expected or sha256(path) != expected:
            raise ProbeError(f"baseline manifest hash mismatch for {name or '<missing>'}")
        hashes[name] = expected
    if not hashes:
        raise ToolchainUnavailable("baseline manifest has no shader hashes")
    assert_bias(shader_dir, R32_BIAS)
    metadata: dict[str, Any] = {
        "shaderDir": str(shader_dir),
        "manifestSha256": sha256(manifest_path),
        "shaderHashes": hashes,
        "bias": R32_BIAS,
        "storageFormat": "r32float",
    }
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("Vulkan", "D3D12"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--samples",
        type=int,
        default=0,
        help="leaf camera frames (0=all eight shifts; bounded to eight)",
    )
    parser.add_argument(
        "--compiler-timeout",
        type=float,
        default=COMPILER_TIMEOUT_SECONDS,
        help="per-stage Slang timeout in seconds (maximum 30)",
    )
    args = parser.parse_args(argv)
    if args.samples < 0:
        parser.error("--samples must be non-negative")
    try:
        output = validate_output_path(args.output)
        artifact_dir = _variant_artifact_dir(output)
        bridge_metadata = create_variant_bridge(artifact_dir)
        shader_dir, manifest = compile_variant(artifact_dir, bridge_metadata, args.compiler_timeout)
        report: dict[str, Any] = {
            "status": "compiled",
            "backend": args.backend,
            "scope": "offline XeGTAO R16 working-depth storage quality probe; not performance or mobile evidence",
            "baseline": _baseline_metadata(DEFAULT_SHADER_DIR),
            "variant": {
                "shaderDir": str(shader_dir),
                "manifest": str(shader_dir / "manifest.json"),
                "manifestSha256": sha256(shader_dir / "manifest.json"),
                "bridge": bridge_metadata,
                "storage": manifest["depthStorageProbe"],
            },
        }
        try:
            report["gpu"] = run_gpu_comparison(args.backend, shader_dir, args.samples)
            report["status"] = "completed"
        except (GpuUnavailable, R16StorageUnsupported) as exc:
            report["status"] = "skipped"
            report["skipReason"] = str(exc)
            report["featureScope"] = {
                "requested": R16_FEATURE,
                "result": "not available; no tier-1 fallback was requested",
            }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, default=str) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2, default=str), flush=True)
        return 0
    except (ProbeError, OSError, ValueError) as exc:
        print(f"compare_xegtao_depth_storage: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":  # pragma: no cover - opt-in compiler/GPU entry point
    raise SystemExit(main())
