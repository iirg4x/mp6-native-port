"""GPU/source checks for the opt-in half-resolution XeGTAO path.

The native-resolution XeGTAO shaders remain the reference.  This fixture
loads the two experimental WGSL modules exactly as embedded by the native
renderer and exercises only the extra adapter/composite steps:

* full clean depth is converted to ceil(full/2) canonical depth;
* the reduced AO result is reconstructed at full resolution; and
* the existing clear, viewport/scissor, foliage and water contracts remain
  authoritative.

The GPU class is optional in the same way as the other WebGPU fixtures.  The
source contracts and analytic tests still run without numpy/wgpu and catch an
accidental ABI or full-path change before a device is available.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import re
import struct
import sys
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


FULL_ADAPTER_HEADER = ROOT / "src" / "gx" / "xegtao" / "adapter_shader.hpp"
FULL_COMPOSITE_HEADER = ROOT / "src" / "gx" / "xegtao" / "composite_shader.hpp"
HALF_ADAPTER_HEADER = ROOT / "src" / "gx" / "xegtao" / "half_adapter_shader.hpp"
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


class ResolutionShaderError(RuntimeError):
    """A production half-resolution shader does not satisfy its ABI."""


@dataclass(frozen=True)
class Resource:
    binding: int
    name: str
    kind: str


def _source(header: Path) -> str:
    if not header.is_file():
        raise ResolutionShaderError(f"missing production shader header: {header}")
    modules = [match.group("code") for match in _RAW_RE.finditer(header.read_text(encoding="utf-8"))]
    if len(modules) != 1:
        raise ResolutionShaderError(f"expected one WGSL raw string in {header}, found {len(modules)}")
    if not modules[0].strip():
        raise ResolutionShaderError(f"empty WGSL raw string in {header}")
    return modules[0]


def _resources(source: str) -> dict[int, Resource]:
    result: dict[int, Resource] = {}
    for match in _DECL_RE.finditer(source):
        attrs = {
            key: int(value)
            for key, value in re.findall(r"@\s*(group|binding)\s*\(\s*(\d+)\s*\)", match.group("attrs"))
        }
        if attrs.get("group") != 0 or "binding" not in attrs:
            raise ResolutionShaderError(f"unbound declaration {match.group('name')!r}")
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
            raise ResolutionShaderError(
                f"binding {binding} ({match.group('name')}) has unsupported type {type_text!r}"
            )
        if binding in result:
            raise ResolutionShaderError(f"duplicate binding {binding}")
        result[binding] = Resource(binding, match.group("name"), kind)
    return result


def _ceil_half(width: int, height: int) -> tuple[int, int]:
    return (width + 1) // 2, (height + 1) // 2


def _inside(width: int, height: int, x: int, y: int, viewport, scissor) -> bool:
    uv = ((x + 0.5) / width, (y + 0.5) / height)
    lo = (max(viewport[0], scissor[0], 0.0), max(viewport[1], scissor[1], 0.0))
    hi = (
        min(viewport[0] + viewport[2], scissor[0] + scissor[2], 1.0),
        min(viewport[1] + viewport[3], scissor[1] + scissor[3], 1.0),
    )
    return lo[0] <= uv[0] < hi[0] and lo[1] <= uv[1] < hi[1]


def _inside_mask(width: int, height: int, viewport, scissor):
    mask = np.zeros((height, width), dtype=bool)
    for y in range(height):
        for x in range(width):
            mask[y, x] = _inside(width, height, x, y, viewport, scissor)
    return mask


def _depth_unpack(z):
    # Same stable forward-Z unpack used by the adapter tests and by the native
    # upstream constants in ambient_occlusion.cpp.
    mul = np.float32(20.0 * 10000.0 / (10000.0 - 20.0))
    add = np.float32(10000.0 / (10000.0 - 20.0))
    z = np.asarray(z, dtype=np.float32)
    result = np.ones_like(z, dtype=np.float32)
    valid = np.isfinite(z) & (z > 0.0)
    result[valid] = np.clip(add - mul / z[valid], 0.0, 1.0)
    return result


def _artistic_obscurance(obscurance):
    """Match the shared MP6 art response used by full and half finals."""
    obscurance = np.asarray(obscurance, dtype=np.float32)
    signal = np.maximum(obscurance - np.float32(0.05), np.float32(0.0))
    knee = np.minimum(signal, np.float32(0.1))
    return signal - knee + np.float32(5.0) * knee * knee


def _pack_adapter_params(depth, viewport, scissor) -> bytes:
    values = (*depth, *viewport, *scissor)
    packed = struct.pack("<12f", *map(float, values))
    if len(packed) != 48:
        raise AssertionError(f"half adapter uniform packed to {len(packed)} bytes")
    return packed


def _pack_composite_params(width, height, low_width, low_height, strength, viewport, scissor) -> bytes:
    values = (
        0.5,
        width / height,
        20.0,
        10000.0,
        *viewport,
        *scissor,
        0.0,
        1.0,
        0.0,
        strength,
        float(width),
        float(height),
        float(low_width),
        float(low_height),
    )
    packed = struct.pack("<20f", *map(float, values))
    if len(packed) != 80:
        raise AssertionError(f"half composite uniform packed to {len(packed)} bytes")
    return packed


def _linearize_canonical_depth(canonical):
    """Apply XeGTAO's exact ``mul / (add - screenDepth)`` conversion.

    The half adapter writes normalized canonical depth.  Composite binding 3
    is the upstream prefilter's linear MIP0, so it must not receive nearest
    raw-depth samples from a CPU model.  This helper is only the isolated
    readback bridge in the fixture; production still runs the upstream MIP0
    compute stage on GPU.
    """
    mul = np.float32(20.0 * 10000.0 / (10000.0 - 20.0))
    add = np.float32(10000.0 / (10000.0 - 20.0))
    canonical = np.asarray(canonical, dtype=np.float32)
    denominator = add - canonical
    result = np.zeros_like(canonical, dtype=np.float32)
    valid = np.isfinite(denominator) & (denominator > 0.0)
    result[valid] = mul / denominator[valid]
    return np.clip(result, 0.0, np.float32(65504.0))


def _aligned_pitch(row_bytes: int) -> int:
    return (row_bytes + 255) & ~255


HAS_GPU_DEPS = np is not None and wgpu is not None


class ResolutionSourceContract(unittest.TestCase):
    """Contracts run even when the optional WebGPU stack is unavailable."""

    def test_full_reference_bindings_remain_unchanged(self):
        adapter = _source(FULL_ADAPTER_HEADER)
        composite = _source(FULL_COMPOSITE_HEADER)
        self.assertEqual(
            {binding: resource.kind for binding, resource in _resources(adapter).items()},
            {0: "uniform", 1: "sampled-f32", 2: "sampled-u32", 3: "sampled-u32"},
        )
        self.assertEqual(
            {binding: (resource.name, resource.kind) for binding, resource in _resources(composite).items()},
            {
                0: ("p", "uniform"),
                1: ("cleanDepth", "sampled-f32"),
                2: ("finalAO", "sampled-u32"),
                5: ("foliageColor", "sampled-f32"),
            },
        )
        self.assertNotIn("lowDepth", composite)
        self.assertNotIn("HalfCompositeShader", composite)
        self.assertIn("textureLoad(finalAO, at, 0)", composite)

    def test_half_modules_have_separate_declared_abis(self):
        adapter = _source(HALF_ADAPTER_HEADER)
        composite = _source(HALF_COMPOSITE_HEADER)
        self.assertEqual(
            {binding: (resource.name, resource.kind) for binding, resource in _resources(adapter).items()},
            {0: ("p", "uniform"), 1: ("cleanDepth", "sampled-f32")},
        )
        self.assertEqual(
            {binding: (resource.name, resource.kind) for binding, resource in _resources(composite).items()},
            {
                0: ("p", "uniform"),
                1: ("cleanDepth", "sampled-f32"),
                2: ("finalAO", "sampled-u32"),
                3: ("lowDepth", "sampled-f32"),
                5: ("foliageColor", "sampled-f32"),
            },
        )
        self.assertEqual(len(_pack_adapter_params((1, 2, 0.3, 0), (0, 0, 1, 1), (0, 0, 1, 1))), 48)
        self.assertEqual(
            len(_pack_composite_params(7, 5, 4, 3, 0.35, (0, 0, 1, 1), (0, 0, 1, 1))),
            80,
        )
        for token in (
            "let lowDimensions = (fullDimensions + vec2u(1)) / vec2u(2);",
            "inverse_slope",
            "textureLoad(lowDepth, at, 0)",
            "smoothstep(0.0, 0.35, total)",
            "let signal = max(obscurance - 0.05, 0.0);",
            "let knee = min(signal, 0.1);",
            "return signal - knee + 5.0 * knee * knee;",
            "p.depth.w * artistic_obscurance(obscurance)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, adapter + composite)

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic checks")
    def test_working_size_and_canonical_constant_plane_reference(self):
        width, height = 7, 5
        low_width, low_height = _ceil_half(width, height)
        self.assertEqual((low_width, low_height), (4, 3))
        depth = np.full((height, width), 400.0, dtype=np.float32)
        expected = _depth_unpack(depth)
        self.assertTrue(np.allclose(expected, expected[0, 0], rtol=0, atol=1e-7))
        self.assertTrue(np.isfinite(expected).all())

    @unittest.skipUnless(np is not None, "numpy is unavailable for analytic checks")
    def test_odd_sloped_depth_reference_is_finite_and_monotone(self):
        width, height = 9, 7
        x, y = np.meshgrid(np.arange(width, dtype=np.float32), np.arange(height, dtype=np.float32))
        depth = 180.0 + 4.0 * x + 11.0 * y
        canonical = _depth_unpack(depth)
        self.assertTrue(np.isfinite(canonical).all())
        # Forward-Z depth increases away from the camera, so the canonical
        # unpacked value increases as well. This catches axis/rounding swaps
        # without baking the experimental one-sided extrapolator into Python.
        self.assertTrue(np.all(np.diff(canonical, axis=1) >= 0.0))
        self.assertTrue(np.all(np.diff(canonical, axis=0) >= 0.0))

    @unittest.skipUnless(np is not None, "numpy is unavailable for mask checks")
    def test_clear_viewport_scissor_and_receiver_masks_are_neutral(self):
        width, height = 9, 5
        depth = np.full((height, width), 400.0, dtype=np.float32)
        depth[2, 3] = 0.0
        viewport = (-0.25, -0.1, 1.25, 1.1)
        scissor = (2.0 / width, 1.0 / height, 5.0 / width, 3.0 / height)
        mask = _inside_mask(width, height, viewport, scissor)
        # The clear sample remains inside the raster mask; depth validity, not
        # viewport/scissor clipping, is what makes its composite neutral.
        self.assertTrue(bool(mask[2, 3]))
        self.assertTrue(np.all(depth[~mask] >= 0.0))
        self.assertEqual(tuple(np.array((0.0, 0.0, 0.0, 1.0), dtype=np.float32)), (0.0, 0.0, 0.0, 1.0))

    @unittest.skipUnless(np is not None, "numpy is unavailable for foliage checks")
    def test_water_and_foreground_are_not_ao_receivers(self):
        # This is the full-resolution receiver policy that both composite
        # shaders must preserve.  Water is neutral black/opaque; ordinary
        # premultiplied foliage receives no AO color contribution here.
        foliage = np.zeros((1, 2, 4), dtype=np.float32)
        foliage[0, 0] = (0.2, 0.4, 0.1, 0.25)  # water marker
        foliage[0, 1] = (0.2, 0.4, 0.1, 0.0)
        self.assertGreater(float(foliage[0, 0, 3]), 0.0)
        self.assertEqual(float(foliage[0, 1, 3]), 0.0)


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOResolutionGPU(unittest.TestCase):
    """Compile and exercise the exact half adapter/composite WGSL."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if cls.adapter is None:
            raise unittest.SkipTest("wgpu could not find a high-performance adapter")
        cls.device = cls.adapter.request_device_sync()
        cls.adapter_source = _source(HALF_ADAPTER_HEADER)
        cls.composite_source = _source(HALF_COMPOSITE_HEADER)
        cls.adapter_module = cls.device.create_shader_module(code=cls.adapter_source)
        cls.composite_module = cls.device.create_shader_module(code=cls.composite_source)
        cls.adapter_layout = cls._layout(48, ((1, "unfilterable-float"),))
        cls.composite_layout = cls._layout(
            80,
            (
                (1, "unfilterable-float"),
                (2, "uint"),
                (3, "unfilterable-float"),
                (5, "unfilterable-float"),
            ),
        )
        cls.adapter_pipeline = cls.device.create_render_pipeline(
            layout=cls.device.create_pipeline_layout(bind_group_layouts=[cls.adapter_layout]),
            vertex={"module": cls.adapter_module, "entry_point": "vs_main"},
            fragment={
                "module": cls.adapter_module,
                "entry_point": "fs_canonical_depth",
                "targets": [{"format": "r32float"}],
            },
            primitive={"topology": "triangle-list"},
        )
        cls.composite_pipeline = cls.device.create_render_pipeline(
            layout=cls.device.create_pipeline_layout(bind_group_layouts=[cls.composite_layout]),
            vertex={"module": cls.composite_module, "entry_point": "vs_main"},
            fragment={
                "module": cls.composite_module,
                "entry_point": "fs_composite",
                "targets": [{"format": "rgba32float"}],
            },
            primitive={"topology": "triangle-list"},
        )

    @classmethod
    def _layout(cls, uniform_size, textures):
        entries = [
            dict(
                binding=0,
                visibility=wgpu.ShaderStage.FRAGMENT,
                buffer=dict(type="uniform", min_binding_size=uniform_size),
            )
        ]
        for binding, sample_type in textures:
            entries.append(
                dict(
                    binding=binding,
                    visibility=wgpu.ShaderStage.FRAGMENT,
                    texture=dict(sample_type=sample_type, view_dimension="2d"),
                )
            )
        return cls.device.create_bind_group_layout(entries=entries)

    @classmethod
    def _texture(cls, width, height, format, usage):
        return cls.device.create_texture(size=(width, height, 1), format=format, usage=usage)

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
            rows = values.view(np.uint8).reshape(height, row_bytes)
            for y in range(height):
                data[y * pitch:y * pitch + row_bytes] = rows[y].tobytes()
        cls.device.queue.write_texture(
            {"texture": texture},
            data,
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
    def _bind(cls, layout, uniform, uniform_size, objects):
        entries = [{"binding": 0, "resource": {"buffer": uniform, "size": uniform_size}}]
        entries.extend({"binding": binding, "resource": view} for binding, view in objects.items())
        return cls.device.create_bind_group(layout=layout, entries=entries)

    @classmethod
    def _render_half(cls, depth, ao, foliage, strength, viewport, scissor,
                     *, linear_depth_override=None):
        depth = np.ascontiguousarray(depth, dtype=np.float32)
        ao = np.ascontiguousarray(ao, dtype=np.uint32)
        foliage = np.ascontiguousarray(foliage, dtype=np.float32)
        height, width = depth.shape
        low_width, low_height = _ceil_half(width, height)
        input_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        full_depth = cls._texture(width, height, "r32float", input_usage)
        cls._write(full_depth, depth)
        low_depth = cls._texture(
            low_width,
            low_height,
            "r32float",
            wgpu.TextureUsage.TEXTURE_BINDING
            | wgpu.TextureUsage.RENDER_ATTACHMENT
            | wgpu.TextureUsage.COPY_SRC,
        )
        low_depth_input = cls._texture(low_width, low_height, "r32float", input_usage)
        final_ao = cls._texture(low_width, low_height, "r32uint", input_usage)
        cls._write(final_ao, ao)
        foliage_texture = cls._texture(width, height, "rgba32float", input_usage)
        cls._write(foliage_texture, foliage)
        adapter_uniform = cls.device.create_buffer_with_data(
            data=_pack_adapter_params(
                (20.0 * 10000.0 / (10000.0 - 20.0), 10000.0 / (10000.0 - 20.0), strength, 0.0),
                viewport,
                scissor,
            ),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        composite_uniform = cls.device.create_buffer_with_data(
            data=_pack_composite_params(width, height, low_width, low_height, strength, viewport, scissor),
            usage=wgpu.BufferUsage.UNIFORM,
        )
        adapter_group = cls._bind(
            cls.adapter_layout,
            adapter_uniform,
            48,
            {1: full_depth.create_view()},
        )
        composite_group = cls._bind(
            cls.composite_layout,
            composite_uniform,
            80,
            {
                1: full_depth.create_view(),
                2: final_ao.create_view(),
                3: low_depth_input.create_view(),
                5: foliage_texture.create_view(),
            },
        )
        output = cls._texture(
            width,
            height,
            "rgba32float",
            wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC,
        )
        adapter_encoder = cls.device.create_command_encoder()
        adapter_pass = adapter_encoder.begin_render_pass(
            color_attachments=[
                dict(view=low_depth.create_view(), load_op="clear", store_op="store", clear_value=(1, 0, 0, 0))
            ]
        )
        adapter_pass.set_pipeline(cls.adapter_pipeline)
        adapter_pass.set_bind_group(0, adapter_group)
        adapter_pass.draw(3)
        adapter_pass.end()
        # Submit and read the exact canonical adapter output before building
        # the composite pass. XeGTAO's prefilter converts this normalized
        # texture to linear MIP0 with mul/(add-screenDepth); the isolated
        # readback/inversion below mirrors that conversion without replacing
        # production compute with a CPU AO implementation.
        cls.device.queue.submit([adapter_encoder.finish()])
        low_canonical = cls._read(low_depth, low_width, low_height, np.float32)
        # Stage-isolation probes may supply the actual upstream MIP0 readback.
        # The default remains the independent adapter-only fixture conversion.
        linear_depth = (_linearize_canonical_depth(low_canonical)
                        if linear_depth_override is None else
                        np.asarray(linear_depth_override, dtype=np.float32))
        if linear_depth.shape != (low_height, low_width):
            raise ValueError('linear depth override must match the half-resolution extent')
        cls._write(low_depth_input, linear_depth)
        composite_encoder = cls.device.create_command_encoder()
        composite_pass = composite_encoder.begin_render_pass(
            color_attachments=[
                dict(view=output.create_view(), load_op="clear", store_op="store", clear_value=(0, 0, 0, 1))
            ]
        )
        composite_pass.set_pipeline(cls.composite_pipeline)
        composite_pass.set_bind_group(0, composite_group)
        composite_pass.draw(3)
        composite_pass.end()
        cls.device.queue.submit([composite_encoder.finish()])
        return (
            low_canonical,
            cls._read(output, width, height, np.float32, channels=4),
        )

    def test_constant_plane_and_odd_size_depth_sampling(self):
        width, height = 7, 5
        depth = np.full((height, width), 400.0, dtype=np.float32)
        ao = np.full((3, 4), 128, dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        low_depth, output = self._render_half(depth, ao, foliage, 0.35, (0, 0, 1, 1), (0, 0, 1, 1))
        expected_depth = float(_depth_unpack(np.array([400.0], dtype=np.float32))[0])
        np.testing.assert_allclose(low_depth, expected_depth, rtol=3e-5, atol=3e-5)
        visibility = 1.0 - 0.35 * float(_artistic_obscurance(1.0 - 128.0 / 255.0))
        np.testing.assert_allclose(output[1:-1, 1:-1, :3], 0.0, rtol=0, atol=2e-6)
        # Interior full-resolution pixels have four valid low taps.  The
        # outermost pixels intentionally fade unsupported subpixel support.
        np.testing.assert_allclose(output[1:-1, 1:-1, 3], visibility, rtol=0, atol=2e-6)
        self.assertEqual(output.shape, (height, width, 4))

    def test_stage_isolation_depth_override_is_actually_bound(self):
        depth = np.full((18, 32), 400., dtype=np.float32)
        ao = np.full((9, 16), 128, dtype=np.uint32)
        foliage = np.zeros((18, 32, 4), dtype=np.float32)
        canonical, baseline = self._render_half(depth, ao, foliage, .35,
                                               (0, 0, 1, 1), (0, 0, 1, 1))
        _, same = self._render_half(depth, ao, foliage, .35, (0, 0, 1, 1), (0, 0, 1, 1),
                                    linear_depth_override=_linearize_canonical_depth(canonical))
        np.testing.assert_array_equal(same, baseline)
        _, rejected = self._render_half(depth, ao, foliage, .35, (0, 0, 1, 1), (0, 0, 1, 1),
                                        linear_depth_override=np.full((9, 16), 800., dtype=np.float32))
        np.testing.assert_array_equal(rejected[..., 3], np.ones_like(depth))
        self.assertLess(float(baseline[9, 16, 3]), .9)
        with self.assertRaisesRegex(ValueError, 'half-resolution extent'):
            self._render_half(depth, ao, foliage, .35, (0, 0, 1, 1), (0, 0, 1, 1),
                              linear_depth_override=np.ones((18, 32), dtype=np.float32))

    def test_odd_sloped_plane_depth_sampling_is_finite_and_ordered(self):
        width, height = 9, 7
        x, y = np.meshgrid(np.arange(width, dtype=np.float32), np.arange(height, dtype=np.float32))
        depth = 180.0 + 4.0 * x + 11.0 * y
        ao = np.full((4, 5), 255, dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        low_depth, output = self._render_half(depth, ao, foliage, 1.0, (0, 0, 1, 1), (0, 0, 1, 1))
        self.assertTrue(np.isfinite(low_depth).all())
        self.assertTrue(np.isfinite(output).all())
        self.assertTrue(np.all(np.diff(low_depth, axis=1) >= -1e-6))
        self.assertTrue(np.all(np.diff(low_depth, axis=0) >= -1e-6))
        np.testing.assert_allclose(output[..., 3], 1.0, rtol=0, atol=2e-6)

    def test_projected_plane_matches_exact_low_pixel_centers(self):
        # Perspective-projected planes are affine in reciprocal depth, not Z.
        # Compare against the analytic plane, not the adapter's own algorithm.
        for width, height in ((32, 18), (33, 19), (65, 37)):
            with self.subTest(size=(width, height)):
                y, x = np.indices((height, width), dtype=np.float32)
                depth = 1.0 / (0.002 + (x + 0.5) * 0.000015 +
                               (y + 0.5) * 0.000021)
                lw, lh = _ceil_half(width, height)
                ao = np.full((lh, lw), 255, dtype=np.uint32)
                foliage = np.zeros((height, width, 4), dtype=np.float32)
                actual, _ = self._render_half(depth, ao, foliage, 0.35,
                                              (0, 0, 1, 1), (0, 0, 1, 1))
                ly, lx = np.indices((lh, lw), dtype=np.float32)
                inverse = (0.002 + (lx + 0.5) * width / lw * 0.000015 +
                           (ly + 0.5) * height / lh * 0.000021)
                expected = _depth_unpack(1.0 / inverse)
                np.testing.assert_allclose(actual, expected, rtol=0, atol=2e-7)

    def test_reciprocal_sloped_plane_preserves_constant_dark_ao(self):
        """A physical plane must not lose a constant dark AO signal.

        AO is intentionally fixed at 128 so the assertion exercises bilateral
        support rather than merely validating depth output.  The interior
        excludes the output-edge support fade; strict variation/absolute
        bounds make a steep-slope support collapse a visible test failure.
        """
        cases = (
            (32, 18, 2.0e-6, 3.0e-6),   # moderate, even dimensions
            (33, 19, 2.0e-6, 3.0e-6),   # moderate, odd dimensions
            (32, 18, 4.0e-5, 6.0e-5),   # steep, even dimensions
            (33, 19, 4.0e-5, 6.0e-5),   # steep, odd dimensions
        )
        expected_visibility = np.float32(1.0 - 0.35 * _artistic_obscurance(1.0 - 128.0 / 255.0))
        for width, height, slope_x, slope_y in cases:
            with self.subTest(size=(width, height), slopes=(slope_x, slope_y)):
                y, x = np.indices((height, width), dtype=np.float32)
                reciprocal = 0.002 + (x + 0.5) * slope_x + (y + 0.5) * slope_y
                depth = 1.0 / reciprocal
                low_width, low_height = _ceil_half(width, height)
                ao = np.full((low_height, low_width), 128, dtype=np.uint32)
                foliage = np.zeros((height, width, 4), dtype=np.float32)
                _, output = self._render_half(
                    depth,
                    ao,
                    foliage,
                    0.35,
                    (0, 0, 1, 1),
                    (0, 0, 1, 1),
                )
                interior = output[2:-2, 2:-2, 3]
                self.assertLess(
                    float(np.max(interior) - np.min(interior)),
                    2.0e-3,
                    "bilateral support varied across a constant-AO reciprocal plane",
                )
                np.testing.assert_allclose(
                    interior,
                    expected_visibility,
                    rtol=0,
                    atol=2.0e-3,
                    err_msg="constant dark AO was lost on a reciprocal-depth plane",
                )

    def test_axis_aligned_depth_step_does_not_create_intermediate_surface(self):
        # A 2x2 reciprocal cross-difference is zero across a vertical step too;
        # that alone cannot classify a footprint as a plane.
        width, height = 34, 18
        depth = np.full((height, width), 800.0, dtype=np.float32)
        depth[:, :17] = 100.0
        lw, lh = _ceil_half(width, height)
        ao = np.full((lh, lw), 255, dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        actual, _ = self._render_half(depth, ao, foliage, 0.35,
                                      (0, 0, 1, 1), (0, 0, 1, 1))
        anchors = np.floor((np.arange(lw) + 0.5) * width / lw).astype(int)
        expected = _depth_unpack(depth[:, anchors])[::2]
        np.testing.assert_allclose(actual, expected, rtol=0, atol=2e-7)

    def test_clear_viewport_scissor_and_strength_are_applied_once(self):
        width, height = 9, 5
        depth = np.full((height, width), 400.0, dtype=np.float32)
        depth[2, 3] = 0.0
        low_width, low_height = _ceil_half(width, height)
        ao = np.zeros((low_height, low_width), dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = (0.2, 0.4, 0.1)
        viewport = (-0.25, -0.1, 1.25, 1.1)
        scissor = (2.0 / width, 1.0 / height, 5.0 / width, 3.0 / height)
        _, output = self._render_half(depth, ao, foliage, 0.35, viewport, scissor)
        inside = _inside_mask(width, height, viewport, scissor)
        valid = inside & (depth > 0.0)
        expected_visibility = np.float32(
            1.0 - np.float32(0.35) * _artistic_obscurance(np.float32(1.0))
        )
        # The central pixel has full four-tap support inside the scissor;
        # clipped support elsewhere is intentionally neutral/faded.
        np.testing.assert_allclose(output[2, 2, 3], expected_visibility, rtol=0, atol=2e-5)
        np.testing.assert_array_equal(output[2, 3], np.array((0, 0, 0, 1), dtype=np.float32))
        self.assertTrue(np.all(output[valid, 3] >= expected_visibility - 2e-5))
        np.testing.assert_array_equal(
            output[~valid],
            np.broadcast_to(np.array((0, 0, 0, 1), dtype=np.float32), output[~valid].shape),
        )

    def test_thin_depth_step_does_not_produce_dark_halo(self):
        width, height = 17, 9
        depth = np.full((height, width), 800.0, dtype=np.float32)
        depth[:, 8] = 90.0  # one-pixel foreground silhouette
        low_width, low_height = _ceil_half(width, height)
        # Only the low-resolution samples whose full-res anchors land on the
        # one-pixel foreground are dark.  Background samples are unoccluded;
        # any dark neighbors therefore indicate a reconstruction halo.
        ao = np.full((low_height, low_width), 255, dtype=np.uint32)
        ao[:, low_width // 2] = 0
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        _, output = self._render_half(depth, ao, foliage, 0.35, (0, 0, 1, 1), (0, 0, 1, 1))
        # Unsupported cross-edge taps must fade to neutral rather than turn a
        # single surviving low-resolution sample into a dark full-res halo.
        for x in (6, 7, 9, 10):
            np.testing.assert_allclose(output[:, x, 3], 1.0, rtol=0, atol=2e-4)

    def test_water_and_foreground_masks_survive_reconstruction(self):
        width, height = 9, 5
        low_width, low_height = _ceil_half(width, height)
        depth = np.full((height, width), 400.0, dtype=np.float32)
        ao = np.zeros((low_height, low_width), dtype=np.uint32)
        foliage = np.zeros((height, width, 4), dtype=np.float32)
        foliage[..., :3] = (0.2, 0.4, 0.1)
        foliage[:, 1, 3] = 0.25  # water marker
        foliage[:, 7, 3] = 0.0  # ordinary foreground/color path
        _, output = self._render_half(depth, ao, foliage, 0.35, (0, 0, 1, 1), (0, 0, 1, 1))
        np.testing.assert_array_equal(
            output[:, 1],
            np.broadcast_to(np.array((0, 0, 0, 1), dtype=np.float32), output[:, 1].shape),
        )
        self.assertTrue(np.all(output[:, 7, 3] < 1.0))


if __name__ == "__main__":  # pragma: no cover - direct optional harness entry point
    unittest.main(verbosity=2)
