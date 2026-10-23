"""Independent GPU verification for the native XeGTAO direct composite.

The direct path consumes the upstream ``r32uint`` AO result and composites it
straight into the existing continuation.  The frozen adapter path is kept as
the comparison oracle: ``fs_legacy_signal`` first writes an RGBA16Float
visibility signal and the exact production ``ambient_occlusion_shader``
(``preparedDepth=True``) composites that signal.

This module deliberately extracts and compiles the production WGSL strings;
it does not duplicate either production shader.  The CPU reference below only
models the observable source/blend contract and is used to catch an incorrect
binding, mask, water, or premultiplied-colour policy even when an optional GPU
adapter is unavailable.

Install the optional GPU dependencies with::

    pip install --target build/ao-gpu-test-deps wgpu==0.32.0 numpy

Run directly with ``python tests/integration/test_xegtao_composite.py -v``.
Missing optional dependencies or a missing adapter skip only the GPU class;
malformed production WGSL, layout, and pipeline errors remain failures.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
OPTIONAL_DEPS = ROOT / "build" / "ao-gpu-test-deps"
if OPTIONAL_DEPS.is_dir() and str(OPTIONAL_DEPS) not in sys.path:
    sys.path.insert(0, str(OPTIONAL_DEPS))

try:
    import numpy as np
except ImportError:  # pragma: no cover - optional GPU environment only
    np = None

try:
    import wgpu
except ImportError:  # pragma: no cover - optional GPU environment only
    wgpu = None


sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT))
from ao_shader_source import shader_source as load_legacy_shader  # noqa: E402
from tools import build  # noqa: E402


COMPOSITE_HEADER = ROOT / "src" / "gx" / "xegtao" / "composite_shader.hpp"
ADAPTER_HEADER = ROOT / "src" / "gx" / "xegtao" / "adapter_shader.hpp"
HALF_COMPOSITE_HEADER = ROOT / "src" / "gx" / "xegtao" / "half_composite_shader.hpp"
_RAW_RE = re.compile(
    r'R"(?P<tag>[A-Za-z_]\w*)\((?P<code>.*?)\)(?P=tag)"',
    re.DOTALL,
)
_DECL_RE = re.compile(
    r"(?P<attrs>(?:@\s*[A-Za-z_]\w*\s*\(\s*[^)]*\)\s*)+)"
    r"var(?P<storage>\s*<[^>]+>)?\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*:\s*(?P<type>[^;]+);",
    re.MULTILINE,
)


class CompositeShaderError(RuntimeError):
    """The production direct-composite module cannot satisfy its contract."""


@dataclass(frozen=True)
class Resource:
    binding: int
    name: str
    kind: str


def _raw_composite_source() -> str:
    if not COMPOSITE_HEADER.is_file():
        raise CompositeShaderError(f"missing native composite shader header: {COMPOSITE_HEADER}")
    header = COMPOSITE_HEADER.read_text(encoding="utf-8")
    modules = [match.group("code") for match in _RAW_RE.finditer(header)]
    if len(modules) != 1:
        raise CompositeShaderError(
            f"expected exactly one raw WGSL module in {COMPOSITE_HEADER}, found {len(modules)}"
        )
    source = modules[0]
    if not source.strip():
        raise CompositeShaderError(f"raw WGSL module in {COMPOSITE_HEADER} is empty")
    return source


def _raw_shader_source(header: Path) -> str:
    if not header.is_file():
        raise CompositeShaderError(f"missing native shader header: {header}")
    modules = [match.group("code") for match in _RAW_RE.finditer(header.read_text(encoding="utf-8"))]
    if len(modules) != 1:
        raise CompositeShaderError(f"expected exactly one raw WGSL module in {header}, found {len(modules)}")
    source = modules[0]
    if not source.strip():
        raise CompositeShaderError(f"raw WGSL module in {header} is empty")
    return source


def _composite_source(*, canonical_depth: bool = False) -> str:
    """Compile the production composite factory and return its selected WGSL.

    The baseline helper is equivalent to the historical raw module, but both
    variants are extracted through the C++ selector once that selector exists.
    This keeps the canonical predicate under test coupled to native source;
    the only supported capability skip is an older header with no helper.
    """
    if not COMPOSITE_HEADER.is_file():
        raise CompositeShaderError(f"missing native composite shader header: {COMPOSITE_HEADER}")
    header = COMPOSITE_HEADER.read_text(encoding="utf-8")
    supports_helper = bool(re.search(
        r"inline\s+std::string\s+composite_shader\s*\(\s*bool\s+canonicalDepth\b",
        header,
        re.S,
    ))
    if not supports_helper:
        if canonical_depth:
            raise CompositeShaderError(
                "canonical composite helper unavailable in this source checkpoint"
            )
        return _raw_composite_source()
    with tempfile.TemporaryDirectory(prefix="xegtao-composite-", dir=ROOT / "build") as temp:
        folder = Path(temp)
        cpp = folder / "dump.cpp"
        exe = folder / "dump.exe"
        cpp.write_text(
            '#include <cstdio>\n'
            f'#include "{COMPOSITE_HEADER.as_posix()}"\n'
            "int main() { auto s = mp6::xegtao::composite_shader("
            + ("true" if canonical_depth else "false")
            + "); std::fputs(s.c_str(), stdout); }\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [build.ZIG, "c++", "-std=c++20", "-O0", str(cpp), "-o", str(exe)],
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode:
            raise CompositeShaderError(
                "production composite helper failed to compile: " + result.stderr
            )
        output = subprocess.run(
            [str(exe)], capture_output=True, text=True, check=True, timeout=10
        ).stdout
    if not output.strip():
        raise CompositeShaderError(
            f"production composite helper returned an empty module: {COMPOSITE_HEADER}"
        )
    return output


def _parse_attrs(attrs: str) -> dict[str, int]:
    return {
        key: int(value)
        for key, value in re.findall(r"@\s*(group|binding)\s*\(\s*(\d+)\s*\)", attrs)
    }


def _resources(source: str) -> dict[int, Resource]:
    result: dict[int, Resource] = {}
    for match in _DECL_RE.finditer(source):
        attrs = _parse_attrs(match.group("attrs"))
        if attrs.get("group") != 0 or "binding" not in attrs:
            raise CompositeShaderError(
                f"bindable declaration {match.group('name')!r} must use group 0 and a binding"
            )
        binding = attrs["binding"]
        storage = (match.group("storage") or "").strip("<> ")
        type_text = match.group("type").strip()
        if storage == "uniform":
            kind = "uniform"
        elif re.fullmatch(r"texture_2d\s*<\s*f32\s*>", type_text):
            kind = "sampled-f32"
        elif re.fullmatch(r"texture_2d\s*<\s*u32\s*>", type_text):
            kind = "sampled-u32"
        else:
            raise CompositeShaderError(
                f"binding {binding} ({match.group('name')}) has unsupported type {type_text!r}"
            )
        if binding in result:
            raise CompositeShaderError(f"duplicate direct-composite binding {binding}")
        result[binding] = Resource(binding, match.group("name"), kind)
    return result


def _inside(width: int, height: int, x: int, y: int, viewport, scissor) -> bool:
    """Model a fragment center, matching the shader's half-open UV bounds."""
    uv_x = (x + 0.5) / width
    uv_y = (y + 0.5) / height
    lo_x = max(viewport[0], scissor[0], 0.0)
    lo_y = max(viewport[1], scissor[1], 0.0)
    hi_x = min(viewport[0] + viewport[2], scissor[0] + scissor[2], 1.0)
    hi_y = min(viewport[1] + viewport[3], scissor[1] + scissor[3], 1.0)
    return lo_x <= uv_x < hi_x and lo_y <= uv_y < hi_y


def _artistic_obscurance(obscurance):
    """MP6 art response applied once after XeGTAO/upsampling obscurance."""
    obscurance = np.asarray(obscurance, dtype=np.float32)
    signal = np.maximum(obscurance - np.float32(0.05), np.float32(0.0))
    knee = np.minimum(signal, np.float32(0.1))
    return signal - knee + np.float32(5.0) * knee * knee


def _reference_source(depth, ao, foliage, strength, viewport, scissor):
    """Independent FP32 direct-composite source, before target/blend rounding."""
    height, width = depth.shape
    result = np.zeros((height, width, 4), dtype=np.float32)
    for y in range(height):
        for x in range(width):
            if not _inside(width, height, x, y, viewport, scissor) or not depth[y, x] > 0:
                result[y, x] = (0.0, 0.0, 0.0, 1.0)
                continue
            obscurance = 1.0 - min(max(float(ao[y, x]) / 255.0, 0.0), 1.0)
            visibility = 1.0 - strength * float(_artistic_obscurance(obscurance))
            if foliage[y, x, 3] > 0.0:
                result[y, x] = (0.0, 0.0, 0.0, 1.0)
            else:
                result[y, x, :3] = foliage[y, x, :3] * (1.0 - visibility)
                result[y, x, 3] = visibility
    return result


def _reference_legacy_source(depth, ao, foliage, strength, viewport, scissor):
    """Model the adapter's RGBA16F signal before legacy composite."""
    result = _reference_source(depth, ao, foliage, strength, viewport, scissor)
    # fs_legacy_signal writes visibility through an RGBA16Float attachment.
    # The production composite then sees that half value in obscurance.r.
    valid = np.zeros(result.shape[:2], dtype=bool)
    height, width = depth.shape
    for y in range(height):
        for x in range(width):
            valid[y, x] = _inside(width, height, x, y, viewport, scissor) and depth[y, x] > 0
    half_visibility = result[..., 3].copy()
    half_visibility[valid] = np.float16(half_visibility[valid]).astype(np.float32)
    # Water/clear/outside pixels have alpha=1 in the adapter and are rejected
    # by the production prepared-depth composite, so retain the direct source.
    ordinary = valid & (foliage[..., 3] <= 0)
    result[ordinary, :3] = foliage[ordinary, :3] * (1.0 - half_visibility[ordinary][:, None])
    result[ordinary, 3] = half_visibility[ordinary]
    return result


def _valid_mask(depth, viewport, scissor):
    height, width = depth.shape
    result = np.zeros((height, width), dtype=bool)
    for y in range(height):
        for x in range(width):
            result[y, x] = _inside(width, height, x, y, viewport, scissor) and depth[y, x] > 0
    return result


def _analytic_visibility(depth, ao, strength, viewport, scissor):
    result = np.ones(depth.shape, dtype=np.float32)
    valid = _valid_mask(depth, viewport, scissor)
    obscurance = 1.0 - np.asarray(ao[valid], dtype=np.float32) / np.float32(255.0)
    result[valid] = 1.0 - np.float32(strength) * _artistic_obscurance(obscurance)
    return result


def _source_from_signal(depth, signal, foliage, viewport, scissor):
    """Model production fs_composite using the captured RGBA16F signal."""
    result = np.zeros((*depth.shape, 4), dtype=np.float32)
    result[..., 3] = 1.0
    valid = _valid_mask(depth, viewport, scissor)
    ordinary = valid & (foliage[..., 3] <= 0.0)
    visibility = np.asarray(signal[..., 0], dtype=np.float32)
    result[ordinary, :3] = foliage[ordinary, :3] * (1.0 - visibility[ordinary, None])
    result[ordinary, 3] = visibility[ordinary]
    return result


def _reference_blend(source, scene):
    """Exact observable RGB-only source-one/src-alpha continuation."""
    scene_half = np.asarray(scene, dtype=np.float16).astype(np.float32)
    result = np.empty_like(source, dtype=np.float32)
    result[..., :3] = source[..., :3] + scene_half[..., :3] * source[..., 3, None]
    result[..., 3] = scene_half[..., 3]  # RGB write mask preserves destination alpha.
    return result


def _pack_params(width, height, strength, viewport, scissor) -> bytes:
    values = (0.5, width / height, 20.0, 10000.0,
              *viewport, *scissor,
              0.0, 1.0, 0.0, strength,
              float(width), float(height), float(width), float(height))
    packed = struct.pack("<20f", *map(float, values))
    if len(packed) != 80:
        raise AssertionError(f"production Params packed to {len(packed)} bytes, expected 80")
    return packed


def _pack_adapter_params(strength, viewport, scissor) -> bytes:
    values = (0.0, 0.0, strength, 0.0, *viewport, *scissor)
    packed = struct.pack("<12f", *map(float, values))
    if len(packed) != 48:
        raise AssertionError(f"adapter Params packed to {len(packed)} bytes, expected 48")
    return packed


def _aligned_pitch(row_bytes: int) -> int:
    return (row_bytes + 255) & ~255


HAS_GPU_DEPS = np is not None and wgpu is not None


class CompositeSourceContract(unittest.TestCase):
    """CPU-only source and analytic contracts run without a GPU dependency."""

    def test_direct_module_exports_exact_bindings_and_no_signal_pass(self):
        source = _composite_source()
        resources = _resources(source)
        self.assertEqual(
            {binding: (resource.name, resource.kind) for binding, resource in resources.items()},
            {
                0: ("p", "uniform"),
                1: ("cleanDepth", "sampled-f32"),
                2: ("finalAO", "sampled-u32"),
                5: ("foliageColor", "sampled-f32"),
            },
        )
        self.assertIn("@vertex fn vs_main", source)
        self.assertIn("@fragment fn fs_composite", source)
        self.assertNotIn("fs_legacy_signal", source)
        self.assertNotIn("rgba16", source.lower())
        self.assertIn("textureLoad(finalAO", source)
        self.assertIn("p.depth.w", source)
        self.assertEqual(len(_pack_params(7, 5, 0.35, (0, 0, 1, 1), (0, 0, 1, 1))), 80)

    def test_artistic_obscurance_response_is_shared_by_all_final_paths(self):
        sources = {
            "full direct": _composite_source(),
            "full adapter signal": _raw_shader_source(ADAPTER_HEADER),
            "half composite": _raw_shader_source(HALF_COMPOSITE_HEADER),
        }
        formula = (
            "let signal = max(obscurance - 0.05, 0.0);\n"
            "    let knee = min(signal, 0.1);\n"
            "    return signal - knee + 5.0 * knee * knee;"
        )
        for name, source in sources.items():
            with self.subTest(path=name):
                self.assertEqual(source.count("fn artistic_obscurance"), 1)
                self.assertIn(formula, source)
                self.assertEqual(source.count("artistic_obscurance("), 2)
        # The legacy composite consumes the adapter's already-shaped
        # visibility signal; no second response is allowed downstream.
        self.assertIn("let obscurance = artistic_obscurance(1.0 - ao);", sources["full direct"])
        self.assertIn("let obscurance = artistic_obscurance(1.0 - clamp(ao, 0.0, 1.0));", sources["full adapter signal"])
        self.assertIn("artistic_obscurance(obscurance)", sources["half composite"])

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic response checks")
    def test_artistic_obscurance_is_zero_soft_knee_monotone_bounded_and_nonexpansive(self):
        values = np.linspace(0.0, 1.0, 1001, dtype=np.float32)
        shaped = _artistic_obscurance(values)
        self.assertEqual(float(shaped[0]), 0.0)
        above_knee = values >= np.float32(0.15)
        np.testing.assert_allclose(shaped[above_knee], values[above_knee] - np.float32(0.1),
                                   rtol=0, atol=2e-7)
        self.assertTrue(np.all(np.diff(shaped) >= 0.0))
        self.assertTrue(np.all(shaped >= 0.0))
        self.assertTrue(np.all(shaped <= 1.0))
        self.assertTrue(np.all(np.diff(shaped) <= np.diff(values) + np.float32(2e-7)))
        self.assertEqual(float(shaped[np.searchsorted(values, np.float32(0.05))]), 0.0)

        # Every packed uint AO step must remain nonexpansive, not only a dense
        # floating-point sample of the response curve.
        obscurance_steps = 1.0 - np.arange(256, dtype=np.float32) / np.float32(255.0)
        shaped_steps = _artistic_obscurance(obscurance_steps)
        self.assertTrue(np.all(np.abs(np.diff(shaped_steps)) <= np.abs(np.diff(obscurance_steps)) + np.float32(2e-7)))

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic response checks")
    def test_artistic_obscurance_deadzone_and_above_knee_endpoints(self):
        values = np.array((0.0, 0.05, 0.0501, 0.1499, 0.15, 0.1501), dtype=np.float32)
        expected = np.array((0.0, 0.0, 5.0e-8, 0.04990005, 0.05, 0.0501), dtype=np.float32)
        np.testing.assert_allclose(_artistic_obscurance(values), expected, rtol=0, atol=2e-7)
        self.assertEqual(float(_artistic_obscurance(np.float32(0.05))), 0.0)
        np.testing.assert_allclose(_artistic_obscurance(np.float32(0.15)), np.float32(0.05), rtol=0, atol=2e-7)

    def test_full_canonical_helper_is_selected_by_the_cpp_factory(self):
        """The Full helper must be the native predicate variant, not a clone."""
        baseline = _composite_source()
        try:
            canonical = _composite_source(canonical_depth=True)
        except CompositeShaderError as exc:
            if "canonical composite helper unavailable" not in str(exc):
                raise
            self.skipTest(str(exc))
        self.assertEqual(
            _raw_composite_source(),
            baseline,
            "baseline extraction must remain byte-identical to CompositeShader",
        )
        resources = _resources(canonical)
        self.assertEqual(
            {binding: (resource.name, resource.kind) for binding, resource in resources.items()},
            {
                0: ("p", "uniform"),
                1: ("cleanDepth", "sampled-f32"),
                2: ("finalAO", "sampled-u32"),
                5: ("foliageColor", "sampled-f32"),
            },
        )
        self.assertIn("@fragment fn fs_composite", canonical)
        self.assertIn("textureLoad(cleanDepth, at, 0).r < 1.0", canonical)
        self.assertIn("textureLoad(cleanDepth, at, 0).r > 0.0", baseline)
        self.assertNotIn("textureLoad(cleanDepth, at, 0).r > 0.0", canonical)
        self.assertNotIn("fs_legacy_signal", canonical)

    def test_source_and_foliage_policy_match_the_legacy_contract(self):
        direct = _composite_source()
        legacy = load_legacy_shader(prepared_depth=True, canonical_depth=False)
        for token in (
            "let lo = max(p.viewport.xy, p.scissor.xy);",
            "let hi = min(p.viewport.xy + p.viewport.zw, p.scissor.xy + p.scissor.zw);",
            "all(uv >= lo)",
            "textureLoad(foliageColor,at,0).a > 0.0",
            "foreground*(1.0-visibility)",
            "return vec4f(0,0,0,1);",
        ):
            with self.subTest(token=token):
                self.assertIn(token, direct)
                self.assertIn(token, legacy)
        # The direct pass must apply strength once from uint AO.  The legacy
        # production composite consumes the adapter's already-scaled signal.
        self.assertIn("f32(textureLoad(finalAO, at, 0).r) / 255.0", direct)
        self.assertIn("return foliage_visibility(pixel.xy,textureLoad(obscurance, vec2i(pixel.xy), 0).r);", legacy)

        def function_body(source, name):
            marker = f"fn {name}"
            start = source.index(marker)
            brace = source.index("{", start)
            depth = 0
            for index in range(brace, len(source)):
                if source[index] == "{":
                    depth += 1
                elif source[index] == "}":
                    depth -= 1
                    if depth == 0:
                        body = source[start:index + 1]
                        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", body, flags=re.S)
                        return re.sub(r"\s+", "", body)
            raise AssertionError(f"unterminated {name}() in shader")

        # The direct pass intentionally owns only AO decoding and strength;
        # receiver clipping and foliage/water policy must remain byte-for-byte
        # equivalent after comments/formatting are removed.
        self.assertEqual(function_body(direct, "inside"), function_body(legacy, "inside"))
        self.assertEqual(
            function_body(direct, "foliage_visibility"),
            function_body(legacy, "foliage_visibility"),
        )

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic reference checks")
    def test_independent_reference_handles_clear_masks_water_and_colored_foreground(self):
        depth = np.full((2, 5), 400.0, dtype=np.float32)
        depth[0, 0] = 0.0
        ao = np.array([[0, 64, 128, 192, 255], [255, 192, 128, 64, 0]], dtype=np.uint32)
        foliage = np.zeros((2, 5, 4), dtype=np.float32)
        foliage[..., :3] = (0.1, 0.3, 0.05)  # premultiplied foreground RGB
        foliage[1, 2, 3] = 0.25  # nonzero alpha marks water, regardless of RGB
        source = _reference_source(depth, ao, foliage, 0.35,
                                   viewport=(0.0, 0.0, 1.0, 1.0),
                                   scissor=(0.2, 0.0, 0.6, 1.0))
        self.assertEqual(tuple(source[0, 0]), (0.0, 0.0, 0.0, 1.0))
        self.assertEqual(tuple(source[0, 4]), (0.0, 0.0, 0.0, 1.0))
        self.assertGreater(float(source[0, 2, 3]), 0.0)
        self.assertGreater(float(source[0, 2, 0]), 0.0)
        self.assertEqual(tuple(source[1, 2]), (0.0, 0.0, 0.0, 1.0))
        self.assertTrue(np.isfinite(source).all())

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic reference checks")
    def test_reference_blend_preserves_destination_alpha(self):
        source = np.array([[[0.2, 0.1, 0.05, 0.75]]], dtype=np.float32)
        scene = np.array([[[0.4, 0.6, 0.8, 0.375]]], dtype=np.float32)
        result = _reference_blend(source, scene)
        expected_rgb = np.array((0.2, 0.1, 0.05), dtype=np.float32) + \
            np.float16((0.4, 0.6, 0.8)).astype(np.float32) * np.float32(0.75)
        np.testing.assert_array_equal(result[0, 0, :3], expected_rgb)
        self.assertEqual(float(result[0, 0, 3]), float(np.float16(0.375)))


def _half_close(actual, expected, label):
    """Check a RGBA16Float readback within two half-precision ULPs.

    Both expected and actual are target-format values.  Two ULPs allows the
    backend's final f32 arithmetic/round-to-half edge while remaining far
    below a visible 1/255 AO step; it does not hide a missing mask or a
    wrongly applied strength factor.
    """
    quantized = np.asarray(expected, dtype=np.float16).astype(np.float32)
    observed = np.asarray(actual, dtype=np.float32)
    # Compute spacing in the target's half format, not in the float32 readback
    # representation (which would make the bound spuriously 8192x tighter).
    spacing = np.abs(np.spacing(quantized.astype(np.float16)).astype(np.float32))
    bound = np.maximum(2.0 * spacing, np.float32(1e-6))
    error = np.abs(observed - quantized)
    if not np.all(error <= bound):
        index = np.unravel_index(int(np.argmax(error - bound)), error.shape)
        raise AssertionError(
            f"{label}: RGBA16Float readback exceeded two ULPs at {index}: "
            f"actual={observed[index]!r}, expected-half={quantized[index]!r}, "
            f"error={error[index]!r}, bound={bound[index]!r}"
        )


def _half_spacing(values):
    """ULP at each value in the RGBA16Float target format."""
    return np.abs(np.spacing(np.asarray(values, dtype=np.float16))).astype(np.float32)


def _assert_signal_near_analytic(signal, depth, ao, strength, viewport, scissor):
    """Require the captured half signal to be nearest/adjacent to FP32 ideal."""
    ideal = _analytic_visibility(depth, ao, strength, viewport, scissor)
    nearest = np.asarray(ideal, dtype=np.float16)
    lower = np.minimum(nearest, np.nextafter(nearest, np.float16(-np.inf)))
    upper = np.maximum(nearest, np.nextafter(nearest, np.float16(np.inf)))
    valid = _valid_mask(depth, viewport, scissor)
    actual = np.asarray(signal[..., 0], dtype=np.float32)
    if not np.all((actual[valid] >= lower[valid]) & (actual[valid] <= upper[valid])):
        bad = np.flatnonzero(valid & ((actual < lower) | (actual > upper)))[0]
        y, x = np.unravel_index(int(bad), valid.shape)
        raise AssertionError(
            "adapter RGBA16F visibility escaped nearest/adjacent half bounds "
            f"at {(y, x)}: actual={actual[y, x]!r}, ideal={ideal[y, x]!r}, "
            f"bounds=({float(lower[y, x])!r}, {float(upper[y, x])!r})"
        )


def _assert_direct_legacy_relation(
    direct, legacy, signal, depth, ao, foliage, strength, viewport, scissor, *, scene=None
):
    """Bound final direct-vs-legacy error from the observed half signal.

    The only intentional semantic difference is the adapter's half visibility
    conversion.  For source-only output its RGB impact is |foreground|*dv;
    after blending it is |scene-foreground|*dv.  Add one target-format ULP
    for each final half conversion, rather than applying a global tolerance.
    """
    valid = _valid_mask(depth, viewport, scissor)
    ordinary = valid & (foliage[..., 3] <= 0.0)
    ideal = _analytic_visibility(depth, ao, strength, viewport, scissor)
    dv = np.abs(np.asarray(signal[..., 0], dtype=np.float32) - ideal)
    if scene is None:
        coefficient = np.abs(foliage[..., :3])
    else:
        scene_half = np.asarray(scene, dtype=np.float16).astype(np.float32)
        coefficient = np.abs(scene_half[..., :3] - foliage[..., :3])
    derived = np.max(coefficient * dv[..., None], axis=-1)
    target_round = np.max(_half_spacing(direct[..., :3]) + _half_spacing(legacy[..., :3]), axis=-1)
    bound = derived + target_round + np.float32(1e-6)
    error = np.max(np.abs(np.asarray(direct)[..., :3] - np.asarray(legacy)[..., :3]), axis=-1)
    if not np.all(error[ordinary] <= bound[ordinary]):
        bad = np.flatnonzero(ordinary & (error > bound))[0]
        y, x = np.unravel_index(int(bad), ordinary.shape)
        raise AssertionError(
            f"direct-vs-legacy RGB exceeded propagated bound at {(y, x)}: "
            f"error={error[y, x]!r}, bound={bound[y, x]!r}, dv={dv[y, x]!r}, "
            f"coefficient={coefficient[y, x]!r}"
        )
    nonordinary = ~ordinary
    np.testing.assert_array_equal(np.asarray(direct)[nonordinary], np.asarray(legacy)[nonordinary])


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOCompositeGPU(unittest.TestCase):
    """Compare direct and frozen legacy paths on real WebGPU render passes."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if cls.adapter is None:
            raise unittest.SkipTest("wgpu could not find a high-performance adapter")
        cls.device = cls.adapter.request_device_sync()
        cls.direct_source = _composite_source()
        try:
            cls.canonical_source = _composite_source(canonical_depth=True)
        except CompositeShaderError as exc:
            if "canonical composite helper unavailable" not in str(exc):
                raise
            raise unittest.SkipTest(str(exc))
        # This invokes the C++ builder and compiles the exact selected
        # production shader; no test-local reconstruction is substituted.
        cls.legacy_source = load_legacy_shader(prepared_depth=True, canonical_depth=False)
        cls.adapter_source = __import__(
            "test_xegtao_adapter", fromlist=["_adapter_source"]
        )._adapter_source()
        cls.direct_module = cls.device.create_shader_module(code=cls.direct_source)
        cls.canonical_module = cls.device.create_shader_module(code=cls.canonical_source)
        cls.legacy_module = cls.device.create_shader_module(code=cls.legacy_source)
        cls.adapter_module = cls.device.create_shader_module(code=cls.adapter_source)
        cls.direct_layout = cls._layout(
            80,
            ((1, "unfilterable-float"), (2, "uint"), (5, "unfilterable-float")),
        )
        # The adapter's AO texture is uint; the legacy production composite
        # consumes the adapter's RGBA16Float signal as f32.
        cls.legacy_layout = cls._layout(
            80,
            ((1, "unfilterable-float"), (2, "unfilterable-float"), (5, "unfilterable-float")),
        )
        cls.adapter_layout = cls._layout(48, ((1, "unfilterable-float"), (2, "uint"), (3, "uint")))

    @classmethod
    def _layout(cls, uniform_size, textures):
        entries = [dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT,
                        buffer=dict(type="uniform", min_binding_size=uniform_size))]
        entries.extend(
            dict(binding=binding, visibility=wgpu.ShaderStage.FRAGMENT,
                 texture=dict(sample_type=sample_type, view_dimension="2d"))
            for binding, sample_type in textures
        )
        return cls.device.create_bind_group_layout(entries=entries)

    @classmethod
    def _texture(cls, width, height, format, usage, sample_count=1):
        return cls.device.create_texture(
            size=(width, height, 1), format=format, usage=usage, sample_count=sample_count
        )

    @classmethod
    def _write(cls, texture, values):
        values = np.ascontiguousarray(values)
        height, width = values.shape[:2]
        channels = values.shape[2] if values.ndim == 3 else 1
        row_bytes = width * channels * values.dtype.itemsize
        pitch = _aligned_pitch(row_bytes)
        if pitch == row_bytes:
            data = values
        else:
            data = bytearray(pitch * height)
            row = values.view(np.uint8).reshape(height, row_bytes)
            for y in range(height):
                data[y * pitch:y * pitch + row_bytes] = row[y].tobytes()
        cls.device.queue.write_texture(
            {"texture": texture}, data,
            {"bytes_per_row": pitch, "rows_per_image": height},
            (width, height, 1),
        )

    @classmethod
    def _read(cls, texture, width, height, dtype, channels=1):
        row_bytes = width * channels * np.dtype(dtype).itemsize
        pitch = _aligned_pitch(row_bytes)
        raw = cls.device.queue.read_texture(
            {"texture": texture},
            {"bytes_per_row": pitch, "rows_per_image": height},
            (width, height, 1),
        )
        rows = np.frombuffer(raw, dtype=np.uint8).reshape(height, pitch)
        compact = rows[:, :row_bytes].copy()
        shape = (height, width, channels) if channels != 1 else (height, width)
        return compact.view(dtype).reshape(shape)

    @classmethod
    def _pipeline(cls, module, layout, entry, sample_count, blend, *, depth=True,
                  target_format="rgba16float"):
        target = dict(format=target_format)
        if blend:
            target.update(
                blend=dict(
                    color=dict(src_factor="one", dst_factor="src-alpha", operation="add"),
                    alpha=dict(src_factor="zero", dst_factor="one", operation="add"),
                ),
                # Native production uses RGB-only writes to preserve EFB alpha.
                write_mask=wgpu.ColorWrite.RED | wgpu.ColorWrite.GREEN | wgpu.ColorWrite.BLUE,
            )
        else:
            target["write_mask"] = wgpu.ColorWrite.ALL
        descriptor = dict(
            layout=cls.device.create_pipeline_layout(bind_group_layouts=[layout]),
            vertex=dict(module=module, entry_point="vs_main"),
            fragment=dict(module=module, entry_point=entry, targets=[target]),
            primitive=dict(topology="triangle-list"),
            multisample=dict(count=sample_count),
        )
        if depth:
            descriptor["depth_stencil"] = dict(
                format="depth32float", depth_write_enabled=False, depth_compare="always"
            )
        return cls.device.create_render_pipeline(**descriptor)

    @classmethod
    def _bind(cls, layout, uniform, uniform_size, objects):
        entries = [dict(binding=0, resource=dict(buffer=uniform, size=uniform_size))]
        entries.extend(dict(binding=binding, resource=view) for binding, view in objects.items())
        return cls.device.create_bind_group(layout=layout, entries=entries)

    @classmethod
    def _pass(cls, encoder, pipeline, group, target, *, depth, load, clear, sample_count):
        if sample_count > 1:
            # The caller passes a single-sample resolve target through target in
            # this case; use the multisample view as the attachment separately.
            color = dict(
                view=target[0].create_view(), resolve_target=target[1].create_view(),
                load_op=load, store_op="store", clear_value=clear,
            )
        else:
            color = dict(view=target.create_view(), load_op=load, store_op="store", clear_value=clear)
        depth_attachment = dict(
            # Composite never writes depth, but a fresh test attachment still
            # needs a defined value before the production-equivalent load.
            view=depth.create_view(), depth_load_op="clear",
            depth_store_op="store", depth_clear_value=0.375,
        )
        render = encoder.begin_render_pass(
            color_attachments=[color], depth_stencil_attachment=depth_attachment
        )
        render.set_pipeline(pipeline)
        render.set_bind_group(0, group)
        render.draw(3)
        render.end()

    @classmethod
    def _render_pair(
        cls, depth, ao, foliage, strength, viewport, scissor, *, blend=False,
        empty_foliage=False, sample_count=1, scene=None, check_depth=False,
        output_format="rgba16float", canonical_clean=None,
    ):
        height, width = depth.shape
        input_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        clean = cls._texture(width, height, "r32float", input_usage)
        final_ao = cls._texture(width, height, "r32uint", input_usage)
        normals = cls._texture(width, height, "r32uint", input_usage)
        clean_values = depth if canonical_clean is None else canonical_clean
        cls._write(clean, np.ascontiguousarray(clean_values, dtype=np.float32))
        cls._write(final_ao, np.ascontiguousarray(ao, dtype=np.uint32))
        cls._write(normals, np.zeros((height, width), dtype=np.uint32))
        if empty_foliage:
            foliage_texture = cls._texture(
                1, 1, "rgba8unorm", input_usage
            )
            cls._write(foliage_texture, np.zeros((1, 1, 4), dtype=np.uint8))
        else:
            foliage_texture = cls._texture(width, height, "rgba32float", input_usage)
            cls._write(foliage_texture, np.ascontiguousarray(foliage, dtype=np.float32))

        params = cls.device.create_buffer_with_data(
            data=_pack_params(width, height, strength, viewport, scissor),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        adapter_params = cls.device.create_buffer_with_data(
            data=_pack_adapter_params(strength, viewport, scissor),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        direct_group = cls._bind(
            cls.direct_layout, params, 80,
            {1: clean.create_view(), 2: final_ao.create_view(), 5: foliage_texture.create_view()},
        )
        adapter_group = cls._bind(
            cls.adapter_layout, adapter_params, 48,
            {1: clean.create_view(), 2: final_ao.create_view(), 3: normals.create_view()},
        )
        composite_module = cls.direct_module if canonical_clean is None else cls.canonical_module
        direct_pipeline = cls._pipeline(
            composite_module, cls.direct_layout, "fs_composite", sample_count, blend,
            target_format=output_format,
        )
        adapter_pipeline = cls._pipeline(
            cls.adapter_module, cls.adapter_layout, "fs_legacy_signal", 1, False, depth=False
        )
        legacy_pipeline = cls._pipeline(
            cls.legacy_module, cls.legacy_layout, "fs_composite", sample_count, blend,
            target_format=output_format,
        )

        output_usage = wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC
        if sample_count == 1:
            direct_target = cls._texture(
                width, height, output_format, output_usage | wgpu.TextureUsage.COPY_DST
            )
            legacy_target = cls._texture(
                width, height, output_format, output_usage | wgpu.TextureUsage.COPY_DST
            )
            direct_color = direct_target
            legacy_color = legacy_target
        else:
            direct_resolve = cls._texture(width, height, output_format, output_usage)
            legacy_resolve = cls._texture(width, height, output_format, output_usage)
            direct_msaa = cls._texture(width, height, output_format, wgpu.TextureUsage.RENDER_ATTACHMENT,
                                       sample_count)
            legacy_msaa = cls._texture(width, height, output_format, wgpu.TextureUsage.RENDER_ATTACHMENT,
                                       sample_count)
            direct_color = (direct_msaa, direct_resolve)
            legacy_color = (legacy_msaa, legacy_resolve)
            direct_target = direct_resolve
            legacy_target = legacy_resolve
        depth_direct = cls._texture(
            width, height, "depth32float", wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING,
            sample_count,
        )
        depth_legacy = cls._texture(
            width, height, "depth32float", wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING,
            sample_count,
        )

        encoder = cls.device.create_command_encoder()
        direct_clear = legacy_clear = (0.0, 0.0, 0.0, 0.0)
        if blend:
            # RGBA16Float is the native continuation.  Single-sample targets
            # get the exact per-pixel destination; MSAA uses a constant clear
            # (the MSAA regression is intentionally a compact smoke case).
            if sample_count == 1:
                scene_dtype = np.float16 if output_format == "rgba16float" else np.float32
                scene_values = np.ascontiguousarray(scene, dtype=scene_dtype)
                cls._write(direct_target, scene_values)
                cls._write(legacy_target, scene_values)
            else:
                scene_values = np.asarray(scene[0, 0], dtype=np.float32)
                direct_clear = tuple(map(float, scene_values))
                legacy_clear = direct_clear
        else:
            direct_clear = legacy_clear = (0.0, 0.0, 0.0, 0.0)
        if sample_count == 1:
            clear = direct_clear if blend else (0.0, 0.0, 0.0, 0.0)
            cls._pass(encoder, direct_pipeline, direct_group, direct_target, depth=depth_direct,
                      load="load" if blend else "clear", clear=clear, sample_count=1)
            signal = cls._texture(
                width, height, "rgba16float",
                wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_SRC,
            )
            signal_pass = encoder.begin_render_pass(color_attachments=[dict(
                view=signal.create_view(), load_op="clear", store_op="store", clear_value=(1, 0, 0, 0)
            )])
            signal_pass.set_pipeline(adapter_pipeline)
            signal_pass.set_bind_group(0, adapter_group)
            signal_pass.draw(3)
            signal_pass.end()
            legacy_group = cls._bind(
                cls.legacy_layout, params, 80,
                {1: clean.create_view(), 2: signal.create_view(), 5: foliage_texture.create_view()},
            )
            clear = legacy_clear if blend else (0.0, 0.0, 0.0, 0.0)
            cls._pass(encoder, legacy_pipeline, legacy_group, legacy_target, depth=depth_legacy,
                      load="load" if blend else "clear", clear=clear, sample_count=1)
        else:
            clear = direct_clear if blend else (0.0, 0.0, 0.0, 0.0)
            cls._pass(encoder, direct_pipeline, direct_group, direct_color, depth=depth_direct,
                      load="clear", clear=clear, sample_count=sample_count)
            signal = cls._texture(
                width, height, "rgba16float",
                wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_SRC,
            )
            signal_pass = encoder.begin_render_pass(color_attachments=[dict(
                view=signal.create_view(), load_op="clear", store_op="store", clear_value=(1, 0, 0, 0)
            )])
            signal_pass.set_pipeline(adapter_pipeline)
            signal_pass.set_bind_group(0, adapter_group)
            signal_pass.draw(3)
            signal_pass.end()
            legacy_group = cls._bind(
                cls.legacy_layout, params, 80,
                {1: clean.create_view(), 2: signal.create_view(), 5: foliage_texture.create_view()},
            )
            cls._pass(encoder, legacy_pipeline, legacy_group, legacy_color, depth=depth_legacy,
                      load="clear", clear=clear, sample_count=sample_count)

        # Probe depth only for the single-sample pass. The composite pipeline
        # has depth writes disabled; sampling after the pass verifies that
        # native EFB depth is untouched, not just that alpha survived.
        depth_probe = None
        if check_depth:
            probe_source = """
                @group(0) @binding(0) var d: texture_depth_2d;
                @vertex fn vs_main(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
                    let p=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
                    return vec4f(p[i],0,1);
                }
                @fragment fn fs(@builtin(position) p:vec4f)->@location(0) f32 {
                    return textureLoad(d,vec2i(p.xy),0);
                }
            """
            probe = cls.device.create_shader_module(code=probe_source)
            probe_pipeline = cls.device.create_render_pipeline(
                layout="auto", vertex=dict(module=probe, entry_point="vs_main"),
                fragment=dict(module=probe, entry_point="fs", targets=[dict(format="r32float")]),
            )
            probe_target = cls._texture(
                width, height, "r32float", wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC
            )
            probe_group = cls.device.create_bind_group(
                layout=probe_pipeline.get_bind_group_layout(0),
                entries=[dict(binding=0, resource=depth_direct.create_view())],
            )
            probe_pass = encoder.begin_render_pass(color_attachments=[dict(
                view=probe_target.create_view(), load_op="clear", store_op="store", clear_value=(0, 0, 0, 0)
            )])
            probe_pass.set_pipeline(probe_pipeline)
            probe_pass.set_bind_group(0, probe_group)
            probe_pass.draw(3)
            probe_pass.end()
            depth_probe = probe_target
        cls.device.queue.submit([encoder.finish()])
        output_dtype = np.float16 if output_format == "rgba16float" else np.float32
        direct = cls._read(direct_target, width, height, output_dtype, channels=4).astype(np.float32)
        legacy = cls._read(legacy_target, width, height, output_dtype, channels=4).astype(np.float32)
        if depth_probe is not None:
            depth_probe = cls._read(depth_probe, width, height, np.float32)
        signal = cls._read(signal, width, height, np.float16, channels=4).astype(np.float32)
        return direct, legacy, depth_probe, signal

    def test_direct_matches_legacy_for_every_ao_value_and_strength(self):
        width, height = 257, 5  # odd dimensions and all 0..255 AO values
        depth = np.full((height, width), 400.0, dtype=np.float32)
        depth[0, 0] = 0.0
        depth[2, 17] = 0.0
        ao = (np.arange(width * height, dtype=np.uint32).reshape(height, width) % 256)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        y, x = np.indices((height, width))
        foliage[..., :3] = np.stack((0.12345 + (x % 5) * 0.01,
                                     0.54321 - (y % 3) * 0.03,
                                     0.24681 + (x % 7) * 0.005), axis=-1)
        water = (x % 19 == 0) & (y % 2 == 1)
        foliage[..., 3] = water.astype(np.float32) * 0.375
        viewport = (-0.12, -0.08, 1.08, 1.14)
        scissor = (0.17, 0.19, 0.64, 0.57)
        scene = np.zeros_like(foliage)
        scene[..., :3] = (0.25, 0.625, 0.875)
        scene[..., 3] = 0.375
        for strength in (0.0, 0.2, 0.35, 1.0):
            with self.subTest(strength=strength):
                direct, legacy, _, signal = self._render_pair(
                    depth, ao, foliage, strength, viewport, scissor,
                    blend=False, scene=scene,
                )
                expected = _reference_source(depth, ao, foliage, strength, viewport, scissor)
                expected_legacy = _source_from_signal(depth, signal, foliage, viewport, scissor)
                _assert_signal_near_analytic(signal, depth, ao, strength, viewport, scissor)
                _half_close(direct, expected, f"direct source strength={strength}")
                _half_close(legacy, expected_legacy, f"legacy source strength={strength}")
                _assert_direct_legacy_relation(
                    direct, legacy, signal, depth, ao, foliage, strength, viewport, scissor
                )
                np.testing.assert_array_equal(
                    direct[water],
                    np.broadcast_to(np.array((0, 0, 0, 1), dtype=np.float32), direct[water].shape),
                )
                self.assertTrue(np.isfinite(direct).all())

    def test_direct_blend_matches_legacy_and_preserves_colored_scene_alpha(self):
        width, height = 11, 7
        depth = np.full((height, width), 250.0, dtype=np.float32)
        depth[0, 0] = 0.0
        ao = ((np.arange(width * height, dtype=np.uint32).reshape(height, width) * 37) % 256)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = (0.12345, 0.54321, 0.24681)
        y, x = np.indices((height, width))
        water = (x == 9) | ((x + y) % 11 == 0)
        foliage[..., 3] = water.astype(np.float32) * 0.125
        scene = np.zeros_like(foliage)
        scene[..., :3] = (0.25, 0.625, 0.875)
        scene[..., 3] = 0.375
        viewport = (0.0, 0.0, 1.0, 1.0)
        scissor = (0.13, 0.11, 0.74, 0.77)
        direct, legacy, depth_probe, signal = self._render_pair(
            depth, ao, foliage, 0.35, viewport, scissor,
            blend=True, scene=scene, check_depth=True,
        )
        source = _reference_source(depth, ao, foliage, 0.35, viewport, scissor)
        legacy_source = _source_from_signal(depth, signal, foliage, viewport, scissor)
        expected = _reference_blend(source, scene)
        expected_legacy = _reference_blend(legacy_source, scene)
        _assert_signal_near_analytic(signal, depth, ao, 0.35, viewport, scissor)
        _half_close(direct, expected, "direct RGB-only blend")
        _half_close(legacy, expected_legacy, "legacy RGB-only blend")
        _assert_direct_legacy_relation(
            direct, legacy, signal, depth, ao, foliage, 0.35, viewport, scissor, scene=scene
        )
        np.testing.assert_array_equal(direct[..., 3], np.full((height, width), np.float16(0.375), dtype=np.float32))
        np.testing.assert_array_equal(legacy[..., 3], np.full((height, width), np.float16(0.375), dtype=np.float32))
        np.testing.assert_allclose(depth_probe, 0.375, rtol=0, atol=1e-7)

    def test_one_pixel_empty_foliage_and_resize(self):
        for width, height in ((1, 1), (7, 3), (13, 5)):
            depth = np.full((height, width), 100.0, dtype=np.float32)
            ao = (np.arange(width * height, dtype=np.uint32).reshape(height, width) % 256)
            foliage = np.zeros((height, width, 4), dtype=np.float32)
            direct, legacy, _, signal = self._render_pair(
                depth, ao, foliage, 0.2, (0, 0, 1, 1), (0, 0, 1, 1),
                empty_foliage=True, scene=foliage,
            )
            expected = _reference_source(depth, ao, foliage, 0.2, (0, 0, 1, 1), (0, 0, 1, 1))
            expected_legacy = _source_from_signal(depth, signal, foliage, (0, 0, 1, 1), (0, 0, 1, 1))
            _assert_signal_near_analytic(signal, depth, ao, 0.2, (0, 0, 1, 1), (0, 0, 1, 1))
            with self.subTest(size=(width, height)):
                _half_close(direct, expected, f"empty foliage direct size={width}x{height}")
                _half_close(legacy, expected_legacy, f"empty foliage legacy size={width}x{height}")
                _assert_direct_legacy_relation(
                    direct, legacy, signal, depth, ao, foliage, 0.2,
                    (0, 0, 1, 1), (0, 0, 1, 1),
                )
                self.assertEqual(direct.shape, (height, width, 4))

    def test_direct_fp32_diagnostic_retains_pre_target_precision(self):
        """Use an RGBA32F diagnostic target to isolate removed half conversion."""
        width, height = 257, 1
        depth = np.full((height, width), 400.0, dtype=np.float32)
        ao = np.arange(width * height, dtype=np.uint32).reshape(height, width) % 256
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = (0.12345, 0.54321, 0.24681)
        viewport = scissor = (0.0, 0.0, 1.0, 1.0)
        direct, legacy, _, signal = self._render_pair(
            depth, ao, foliage, 0.35, viewport, scissor,
            output_format="rgba32float",
        )
        expected = _reference_source(depth, ao, foliage, 0.35, viewport, scissor)
        expected_legacy = _source_from_signal(depth, signal, foliage, viewport, scissor)
        _assert_signal_near_analytic(signal, depth, ao, 0.35, viewport, scissor)
        np.testing.assert_allclose(direct, expected, rtol=0, atol=2e-6)
        np.testing.assert_allclose(legacy, expected_legacy, rtol=0, atol=2e-6)
        direct_error = np.max(np.abs(direct[..., :3] - expected[..., :3]), axis=-1)
        legacy_error = np.max(np.abs(legacy[..., :3] - expected[..., :3]), axis=-1)
        # The diagnostic target avoids final RGBA16F ties, so removal of the
        # intermediate half conversion must be observable against one common
        # unrounded analytic reference.
        self.assertGreater(int(np.count_nonzero(direct_error + 1e-6 < legacy_error)), 0)

    def test_full_canonical_composite_matches_viewz_mask_exactly(self):
        """Exercise the native Full predicate, including 0 and 1 sentinels.

        Canonical preparation represents a valid near-plane sample as zero and
        an uncovered pixel as one.  The source is selected by the C++ helper in
        ``setUpClass``; this test only supplies representable texture values
        and compares the real render outputs against the view-Z baseline.
        """
        width, height = 13, 7
        viewz = np.full((height, width), 400.0, dtype=np.float32)
        canonical = np.full((height, width), 0.5, dtype=np.float32)
        y, x = np.indices((height, width))
        uncovered = ((x + 2 * y) % 11 == 0)
        viewz[uncovered] = 0.0
        canonical[uncovered] = 1.0
        # This pixel is deliberately the canonical near-plane zero, not an
        # uncovered sentinel.  It must receive the AO value below.
        near_y, near_x = 1, 2
        uncovered[near_y, near_x] = False
        viewz[near_y, near_x] = 400.0
        canonical[near_y, near_x] = 0.0
        ao = ((x * 43 + y * 71) % 256).astype(np.uint32)
        ao[near_y, near_x] = 0
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = np.stack(
            (0.11 + (x % 3) * 0.02, 0.31 - (y % 2) * 0.04, 0.07 + (x % 4) * 0.01),
            axis=-1,
        )
        water = ((x * 3 + y) % 13 == 0) & ~uncovered
        foliage[..., 3] = water.astype(np.float32) * 0.25
        viewport = (0.0, 0.0, 1.0, 1.0)
        scissor = (0.13, 0.11, 0.74, 0.77)
        baseline, _, _, _ = self._render_pair(
            viewz,
            ao,
            foliage,
            0.35,
            viewport,
            scissor,
            output_format="rgba32float",
        )
        canonical_output, _, _, _ = self._render_pair(
            viewz,
            ao,
            foliage,
            0.35,
            viewport,
            scissor,
            output_format="rgba32float",
            canonical_clean=canonical,
        )
        np.testing.assert_array_equal(canonical_output, baseline)
        # Explicitly prove the two sentinel classes were not accidentally
        # folded together by a >0 predicate or a blanket clear.
        # AO=0 is fully occluded before MP6's 0.35 strength.  The bounded
        # soft-knee maps that raw obscurance to 0.95, so retain the exact
        # analytic response rather than a stale pre-knee 1 - 0.35 value.
        expected_visibility = np.float32(
            1.0 - np.float32(0.35) * _artistic_obscurance(np.float32(1.0))
        )
        self.assertEqual(float(canonical_output[near_y, near_x, 3]),
                         float(expected_visibility))
        np.testing.assert_array_equal(
            canonical_output[uncovered],
            np.broadcast_to(np.array((0, 0, 0, 1), dtype=np.float32),
                            canonical_output[uncovered].shape),
        )
        np.testing.assert_array_equal(
            canonical_output[water],
            np.broadcast_to(np.array((0, 0, 0, 1), dtype=np.float32),
                            canonical_output[water].shape),
        )
        valid = _valid_mask(viewz, viewport, scissor) & ~water
        self.assertGreater(int(np.count_nonzero(valid)), 0)
        self.assertTrue(np.isfinite(canonical_output).all())

    def test_msaa_four_composite_matches_legacy(self):
        # Keep the MSAA case deliberately small: the parent integration also
        # validates the full production depth/MSAA schedule.  This test proves
        # direct-vs-legacy composite pipeline sample-count compatibility.
        width, height = 5, 3
        depth = np.full((height, width), 300.0, dtype=np.float32)
        ao = np.array([[0, 64, 128, 192, 255], [255, 192, 128, 64, 0], [13, 37, 91, 173, 241]], dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = (0.11, 0.31, 0.07)
        direct, legacy, _, signal = self._render_pair(
            depth, ao, foliage, 1.0, (0, 0, 1, 1), (0, 0, 1, 1),
            blend=True, scene=np.broadcast_to(np.array((0.25, 0.625, 0.875, 0.375), dtype=np.float32), foliage.shape),
            sample_count=4,
        )
        scene = np.broadcast_to(np.array((0.25, 0.625, 0.875, 0.375), dtype=np.float32), foliage.shape)
        expected = _reference_blend(_reference_source(depth, ao, foliage, 1.0, (0, 0, 1, 1), (0, 0, 1, 1)), scene)
        expected_legacy = _reference_blend(
            _source_from_signal(depth, signal, foliage, (0, 0, 1, 1), (0, 0, 1, 1)), scene
        )
        _assert_signal_near_analytic(signal, depth, ao, 1.0, (0, 0, 1, 1), (0, 0, 1, 1))
        _half_close(direct, expected, "direct MSAA4 blend")
        _half_close(legacy, expected_legacy, "legacy MSAA4 blend")
        _assert_direct_legacy_relation(
            direct, legacy, signal, depth, ao, foliage, 1.0,
            (0, 0, 1, 1), (0, 0, 1, 1), scene=scene,
        )


if __name__ == "__main__":  # pragma: no cover - direct optional harness entry point
    unittest.main(verbosity=2)
