"""Optional GPU verification for the native XeGTAO adapter raster shaders.

The production adapter is embedded as a C++ raw WGSL string in
``src/gx/xegtao/adapter_shader.hpp``.  These tests extract and compile that
exact string, then exercise its two fragment entry points directly.  They do
not reuse the upstream XeGTAO renderer harness or copy its shader logic.

Install the optional dependencies with::

    pip install --target build/ao-gpu-test-deps wgpu==0.32.0 numpy

Run directly with ``python tests/integration/test_xegtao_adapter.py -v``.
Missing optional dependencies or a missing GPU adapter skip only the GPU
class.  Missing/malformed shader source and WebGPU layout or pipeline errors
remain failures.
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


ADAPTER_HEADER = ROOT / "src" / "gx" / "xegtao" / "adapter_shader.hpp"
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


class AdapterShaderError(RuntimeError):
    """The production adapter source cannot satisfy its declared contract."""


@dataclass(frozen=True)
class Resource:
    binding: int
    name: str
    kind: str


def _adapter_source() -> str:
    if not ADAPTER_HEADER.is_file():
        raise AdapterShaderError(f"missing native adapter shader header: {ADAPTER_HEADER}")
    header = ADAPTER_HEADER.read_text(encoding="utf-8")
    modules = [match.group("code") for match in _RAW_RE.finditer(header)]
    if len(modules) != 1:
        raise AdapterShaderError(
            f"expected exactly one raw WGSL module in {ADAPTER_HEADER}, found {len(modules)}"
        )
    source = modules[0]
    if not source.strip():
        raise AdapterShaderError(f"raw WGSL module in {ADAPTER_HEADER} is empty")
    return source


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
            raise AdapterShaderError(
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
            raise AdapterShaderError(
                f"binding {binding} ({match.group('name')}) has unsupported type {type_text!r}"
            )
        if binding in result:
            raise AdapterShaderError(f"duplicate adapter binding {binding}")
        result[binding] = Resource(binding, match.group("name"), kind)
    return result


def _pack_normal(normal) -> int:
    encoded = (np.asarray(normal, dtype=np.float32) * np.float32(0.5) + np.float32(0.5)).clip(0, 1)
    channels = [int(float(value) * scale + 0.5) for value, scale in zip(encoded, (2047, 2047, 1023))]
    return channels[0] | (channels[1] << 11) | (channels[2] << 22)


def _artistic_obscurance(obscurance):
    """Match the shared MP6 art response used by every final path."""
    obscurance = np.asarray(obscurance, dtype=np.float32)
    signal = np.maximum(obscurance - np.float32(0.05), np.float32(0.0))
    knee = np.minimum(signal, np.float32(0.1))
    return signal - knee + np.float32(5.0) * knee * knee


def _decode_normal(packed):
    packed = np.asarray(packed, dtype=np.uint32)
    encoded = np.stack(
        (
            (packed & np.uint32(0x7FF)).astype(np.float32) / np.float32(2047),
            ((packed >> np.uint32(11)) & np.uint32(0x7FF)).astype(np.float32) / np.float32(2047),
            ((packed >> np.uint32(22)) & np.uint32(0x3FF)).astype(np.float32) / np.float32(1023),
        ),
        axis=-1,
    )
    return encoded * np.float32(2) - np.float32(1)


def _pack_uniform(depth, viewport, scissor) -> bytes:
    values = (*depth, *viewport, *scissor)
    if len(values) != 12:
        raise AssertionError(f"adapter uniform requires 12 floats, got {len(values)}")
    packed = struct.pack("<12f", *map(float, values))
    if len(packed) != 48:
        raise AssertionError(f"adapter uniform packed to {len(packed)} bytes, expected 48")
    return packed


def _inside_mask(width: int, height: int, viewport, scissor):
    mask = np.zeros((height, width), dtype=bool)
    lo_x = max(viewport[0], scissor[0], 0.0)
    lo_y = max(viewport[1], scissor[1], 0.0)
    hi_x = min(viewport[0] + viewport[2], scissor[0] + scissor[2], 1.0)
    hi_y = min(viewport[1] + viewport[3], scissor[1] + scissor[3], 1.0)
    for y in range(height):
        for x in range(width):
            uv_x = (x + 0.5) / width
            uv_y = (y + 0.5) / height
            mask[y, x] = lo_x <= uv_x < hi_x and lo_y <= uv_y < hi_y
    return mask


HAS_GPU_DEPS = np is not None and wgpu is not None


class AdapterSourceContract(unittest.TestCase):
    """CPU-only source checks still run when the optional GPU stack is absent."""

    def test_raw_source_and_48_byte_layout(self):
        source = _adapter_source()
        self.assertIn("struct AdapterParams", source)
        self.assertEqual(len(re.findall(r"\bvec4f\b", source.split("}", 1)[0])), 3)
        resources = _resources(source)
        self.assertEqual(
            {binding: resource.kind for binding, resource in resources.items()},
            {0: "uniform", 1: "sampled-f32", 2: "sampled-u32", 3: "sampled-u32"},
        )
        self.assertIn("fs_canonical_depth", source)
        self.assertIn("fs_legacy_signal", source)
        self.assertEqual(len(_pack_uniform((1, 2, 0.3, 0), (0, 0, 1, 1), (0, 0, 1, 1))), 48)

    def test_raw_source_contains_explicit_adapter_contract(self):
        source = _adapter_source()
        self.assertIn("p.depth.y - p.depth.x / z", source)
        self.assertIn("return 1.0", source)
        self.assertIn("/ 255.0", source)
        self.assertIn("vec3f(1, 1, -1)", source)
        self.assertIn("textureLoad(finalAO", source)
        self.assertIn("textureLoad(packedNormals", source)
        self.assertIn("let signal = max(obscurance - 0.05, 0.0);", source)
        self.assertIn("let knee = min(signal, 0.1);", source)
        self.assertIn("return signal - knee + 5.0 * knee * knee;", source)
        self.assertEqual(source.count("artistic_obscurance("), 2)


@unittest.skipUnless(HAS_GPU_DEPS, "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable")
class XeGTAOAdapterGPU(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = _adapter_source()
        cls.resources = _resources(cls.source)
        cls.adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if cls.adapter is None:
            raise unittest.SkipTest("wgpu could not find a high-performance adapter")
        # Shader-module, layout, and pipeline errors intentionally propagate as
        # test failures. Only an absent optional adapter is a skip condition.
        cls.device = cls.adapter.request_device_sync()
        cls.module = cls.device.create_shader_module(code=cls.source)
        cls.depth_layout = cls._make_layout((0, 1))
        cls.signal_layout = cls._make_layout((0, 1, 2, 3))
        cls.depth_pipeline = cls.device.create_render_pipeline(
            layout=cls.device.create_pipeline_layout(bind_group_layouts=[cls.depth_layout]),
            vertex={"module": cls.module, "entry_point": "vs_main"},
            fragment={"module": cls.module, "entry_point": "fs_canonical_depth", "targets": [{"format": "r32float"}]},
            primitive={"topology": "triangle-list"},
        )
        cls.signal_pipeline = cls.device.create_render_pipeline(
            layout=cls.device.create_pipeline_layout(bind_group_layouts=[cls.signal_layout]),
            vertex={"module": cls.module, "entry_point": "vs_main"},
            fragment={"module": cls.module, "entry_point": "fs_legacy_signal", "targets": [{"format": "rgba16float"}]},
            primitive={"topology": "triangle-list"},
        )

    @classmethod
    def _make_layout(cls, bindings):
        entries = []
        for binding in bindings:
            resource = cls.resources.get(binding)
            if resource is None:
                raise AdapterShaderError(f"adapter layout requires missing binding {binding}")
            entry = {"binding": binding, "visibility": wgpu.ShaderStage.FRAGMENT}
            if resource.kind == "uniform":
                entry["buffer"] = {"type": "uniform", "min_binding_size": 48}
            elif resource.kind == "sampled-f32":
                entry["texture"] = {"sample_type": "unfilterable-float", "view_dimension": "2d"}
            elif resource.kind == "sampled-u32":
                entry["texture"] = {"sample_type": "uint", "view_dimension": "2d"}
            else:  # pragma: no cover - source parser rejects other kinds
                raise AdapterShaderError(f"unsupported adapter resource {resource}")
            entries.append(entry)
        return cls.device.create_bind_group_layout(entries=entries)

    @classmethod
    def _texture(cls, width, height, format, usage):
        return cls.device.create_texture(size=(width, height, 1), format=format, usage=usage)

    @classmethod
    def _write(cls, texture, values, width, height):
        values = np.ascontiguousarray(values)
        channels = values.shape[-1] if values.ndim == 3 else 1
        cls.device.queue.write_texture(
            {"texture": texture},
            values,
            {"bytes_per_row": width * values.dtype.itemsize * channels, "rows_per_image": height},
            (width, height, 1),
        )

    @classmethod
    def _read(cls, texture, width, height, dtype, channels=1):
        raw = cls.device.queue.read_texture(
            {"texture": texture},
            {"bytes_per_row": width * np.dtype(dtype).itemsize * channels, "rows_per_image": height},
            (width, height, 1),
        )
        shape = (height, width, channels) if channels != 1 else (height, width)
        return np.frombuffer(raw, dtype=dtype).reshape(shape).copy()

    @classmethod
    def _render(cls, pipeline, layout, width, height, uniform_values, clean_depth, final_ao=None, packed_normals=None, output_format="r32float"):
        input_usage = wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST
        depth_texture = cls._texture(width, height, "r32float", input_usage)
        cls._write(depth_texture, clean_depth, width, height)
        objects = {1: depth_texture.create_view()}
        if final_ao is not None:
            ao_texture = cls._texture(width, height, "r32uint", input_usage)
            cls._write(ao_texture, final_ao, width, height)
            objects[2] = ao_texture.create_view()
        if packed_normals is not None:
            normal_texture = cls._texture(width, height, "r32uint", input_usage)
            cls._write(normal_texture, packed_normals, width, height)
            objects[3] = normal_texture.create_view()
        uniform = cls.device.create_buffer_with_data(
            data=_pack_uniform(*uniform_values), usage=wgpu.BufferUsage.UNIFORM
        )
        entries = [{"binding": 0, "resource": {"buffer": uniform, "size": 48}}]
        entries.extend({"binding": binding, "resource": objects[binding]} for binding in sorted(objects))
        bindings = cls.device.create_bind_group(layout=layout, entries=entries)
        output_usage = wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC
        output = cls._texture(width, height, output_format, output_usage)
        encoder = cls.device.create_command_encoder()
        render = encoder.begin_render_pass(
            color_attachments=[
                dict(
                    view=output.create_view(),
                    clear_value=(1, 0, 0, 0),
                    load_op="clear",
                    store_op="store",
                )
            ]
        )
        render.set_pipeline(pipeline)
        render.set_bind_group(0, bindings)
        render.draw(3)
        render.end()
        cls.device.queue.submit([encoder.finish()])
        return output

    def _depth(self, z, viewport=(0, 0, 1, 1), scissor=(0, 0, 1, 1)):
        z = np.ascontiguousarray(z, dtype=np.float32)
        height, width = z.shape
        return self._render(
            self.depth_pipeline,
            self.depth_layout,
            width,
            height,
            ((20.0 * 10000.0 / (10000.0 - 20.0), 10000.0 / (10000.0 - 20.0), 0.4, 0.0), viewport, scissor),
            z,
        )

    def _signal(self, z, ao, normals, strength, viewport=(0, 0, 1, 1), scissor=(0, 0, 1, 1)):
        z = np.ascontiguousarray(z, dtype=np.float32)
        ao = np.ascontiguousarray(ao, dtype=np.uint32)
        normals = np.ascontiguousarray(normals, dtype=np.uint32)
        height, width = z.shape
        return self._render(
            self.signal_pipeline,
            self.signal_layout,
            width,
            height,
            ((0.0, 0.0, strength, 0.0), viewport, scissor),
            z,
            ao,
            normals,
            output_format="rgba16float",
        )

    def test_depth_roundtrip_and_fullres_shape_on_odd_size(self):
        width, height = 7, 5
        z = np.array(
            [
                [20.0, 50.0, 100.0, 400.0, 1000.0, 5000.0, 0.0],
                [30.0, 60.0, 120.0, 500.0, 1500.0, 9000.0, -1.0],
                [25.0, 75.0, 200.0, 800.0, 2000.0, 7000.0, 0.0],
                [40.0, 80.0, 300.0, 600.0, 3000.0, 8000.0, 45.0],
                [22.0, 90.0, 250.0, 750.0, 2500.0, 6000.0, 55.0],
            ],
            dtype=np.float32,
        )
        output = self._depth(z)
        actual = self._read(output, width, height, np.float32)
        x = np.float32(20.0 * 10000.0 / (10000.0 - 20.0))
        y = np.float32(10000.0 / (10000.0 - 20.0))
        expected = np.full_like(z, 1.0)
        valid = z > 0.0
        expected[valid] = np.clip(y - x / z[valid], 0.0, 1.0)
        np.testing.assert_allclose(actual, expected, rtol=3e-5, atol=3e-5)
        self.assertEqual(actual.shape, (height, width))

    def test_depth_clear_scissor_and_offset_viewport_clamp_out(self):
        width, height = 9, 5
        z = np.full((height, width), 400.0, dtype=np.float32)
        z[2, 3] = 0.0
        viewport = (-0.4, -0.2, 1.5, 1.4)
        scissor = (2.0 / width, 1.0 / height, 5.0 / width, 3.0 / height)
        output = self._depth(z, viewport=viewport, scissor=scissor)
        actual = self._read(output, width, height, np.float32)
        inside = _inside_mask(width, height, viewport, scissor)
        x = np.float32(20.0 * 10000.0 / (10000.0 - 20.0))
        y = np.float32(10000.0 / (10000.0 - 20.0))
        expected = np.full((height, width), 1.0, dtype=np.float32)
        valid = inside & (z > 0.0)
        expected[valid] = np.clip(y - x / z[valid], 0.0, 1.0)
        np.testing.assert_allclose(actual, expected, rtol=3e-5, atol=3e-5)
        self.assertTrue(np.all(actual[~inside] == np.float32(1.0)))

    def test_normal_encoding_and_visibility_strength_decode(self):
        width, height = 5, 3
        z = np.full((height, width), 400.0, dtype=np.float32)
        ao = np.array(
            [[0, 64, 128, 192, 255], [255, 192, 128, 64, 0], [80, 100, 120, 140, 160]],
            dtype=np.uint32,
        )
        normal = np.array([0.31, -0.47, math.sqrt(1.0 - 0.31**2 - 0.47**2)], dtype=np.float32)
        packed = np.full((height, width), _pack_normal(normal), dtype=np.uint32)
        viewport = (1.0 / width, 0.0, 3.0 / width, 1.0)
        scissor = (0.0, 1.0 / height, 1.0, 1.0 - 2.0 / height)
        strength = 0.6
        output = self._signal(z, ao, packed, strength, viewport=viewport, scissor=scissor)
        actual = self._read(output, width, height, np.float16, channels=4).astype(np.float32)
        decoded = _decode_normal(np.array(_pack_normal(normal), dtype=np.uint32))
        decoded = decoded / np.linalg.norm(decoded)
        expected = np.empty((height, width, 4), dtype=np.float32)
        obscurance = 1.0 - ao.astype(np.float32) / 255.0
        expected[..., 0] = 1.0 - strength * _artistic_obscurance(obscurance)
        expected[..., 1] = decoded[0]
        expected[..., 2] = decoded[1]
        expected[..., 3] = -decoded[2]
        valid = _inside_mask(width, height, viewport, scissor)
        expected[~valid] = (1.0, 0.0, 0.0, 0.0)
        np.testing.assert_allclose(actual, expected, rtol=2e-3, atol=2e-3)
        np.testing.assert_array_equal(actual[~valid], np.broadcast_to(np.array((1, 0, 0, 0), dtype=np.float32), actual[~valid].shape))

    def test_fullres_output_shape_on_nonmultiple_size(self):
        width, height = 11, 7
        z = np.full((height, width), 400.0, dtype=np.float32)
        ao = np.full((height, width), 128, dtype=np.uint32)
        packed = np.full((height, width), _pack_normal((0.0, 0.0, 1.0)), dtype=np.uint32)
        output = self._signal(z, ao, packed, 0.35)
        actual = self._read(output, width, height, np.float16, channels=4)
        self.assertEqual(actual.shape, (height, width, 4))
        self.assertTrue(np.isfinite(actual.astype(np.float32)).all())


if __name__ == "__main__":  # pragma: no cover - direct optional harness entry point
    unittest.main(verbosity=2)
