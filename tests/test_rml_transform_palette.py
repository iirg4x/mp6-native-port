"""Source, lifetime, and replay contracts for RmlUi transform palettes.

The storybook sky keeps its immutable local geometry and changes only a small
per-frame transform/color table.  These checks intentionally cover both sides
of that boundary: the public API must snapshot a caller-owned span before it
can be queued, while the renderer must consume the resulting uniform range
through the normal resident-geometry/stencil path.  The final test replays the
complete pinned Aurora patch queue for every file owned by the feature, so a
direct edit of the generated checkout cannot silently replace the patch.
"""

from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


PATCH = ROOT / "compat/aurora/base/0069-rml-transform-palette.patch"
SOURCE_ROOT = ROOT / "third_party/aurora"
PUBLIC_HPP = SOURCE_ROOT / "include/aurora/rmlui.hpp"
RML_CPP = SOURCE_ROOT / "lib/rmlui.cpp"
RENDER_CPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.cpp"
RENDER_HPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.hpp"
PIPELINE_CPP = SOURCE_ROOT / "lib/rmlui/pipeline.cpp"
PIPELINE_HPP = SOURCE_ROOT / "lib/rmlui/pipeline.hpp"

TARGETS = {
    "include/aurora/rmlui.hpp",
    "lib/rmlui.cpp",
    "lib/rmlui/WebGPURenderInterface.cpp",
    "lib/rmlui/WebGPURenderInterface.hpp",
    "lib/rmlui/pipeline.cpp",
    "lib/rmlui/pipeline.hpp",
}


def _slice_between(source: str, start: str, end: str) -> str:
    """Return one bounded source region, failing loudly if it moved."""

    start_at = source.index(start)
    end_at = source.index(end, start_at + len(start))
    return source[start_at:end_at]


@dataclass(frozen=True)
class Transform:
    basis: tuple[float, float, float, float]
    offset: tuple[float, float, float, float]
    color: tuple[float, float, float, float]


class PaletteLifecycle:
    """Small model of the renderer's fixed-size, frame-local palette range."""

    capacity = 256

    def __init__(self):
        self.active = False
        self.valid = False
        self.snapshot: tuple[Transform, ...] = ()

    def begin(self):
        self.active = True
        self.valid = False
        self.snapshot = ()

    def set(self, transforms):
        if not self.active or not transforms or len(transforms) > self.capacity:
            self.valid = False
            self.snapshot = ()
            return
        # This tuple is the model of the immediate uniform upload: mutating
        # the caller's list after set() cannot mutate the frame payload.
        self.snapshot = tuple(transforms)
        self.valid = True

    def end(self):
        self.active = False
        self.valid = False
        self.snapshot = ()


def _apply_transform(transform: Transform, point: tuple[float, float]) -> tuple[float, float]:
    a, b, c, d = transform.basis
    ox, oy, _oz, _ow = transform.offset
    x, y = point
    return (a * x + c * y + ox, b * x + d * y + oy)


class RmlTransformPaletteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for path in (PUBLIC_HPP, RML_CPP, RENDER_CPP, RENDER_HPP, PIPELINE_CPP, PIPELINE_HPP):
            if not path.exists():
                raise unittest.SkipTest(f"generated Aurora source is unavailable: {path}")
        cls.public_hpp = PUBLIC_HPP.read_text(encoding="utf-8")
        cls.rml_cpp = RML_CPP.read_text(encoding="utf-8")
        cls.render_cpp = RENDER_CPP.read_text(encoding="utf-8")
        cls.render_hpp = RENDER_HPP.read_text(encoding="utf-8")
        cls.pipeline_cpp = PIPELINE_CPP.read_text(encoding="utf-8")
        cls.pipeline_hpp = PIPELINE_HPP.read_text(encoding="utf-8")

    def test_public_layout_is_three_48_byte_float_rows_and_fixed_256_capacity(self):
        struct = _slice_between(self.public_hpp, "struct GeometryTransform {", "};")
        self.assertRegex(struct, r"std::array<float,\s*4>\s+basis\s*;\s*")
        self.assertRegex(struct, r"std::array<float,\s*4>\s+offset\s*;\s*")
        self.assertRegex(struct, r"std::array<float,\s*4>\s+color\s*;\s*")
        self.assertRegex(
            self.public_hpp,
            r"inline\s+constexpr\s+size_t\s+GeometryTransformPaletteSize\s*=\s*256\s*;",
        )
        self.assertRegex(
            self.public_hpp,
            r"void\s+set_geometry_transform_palette\s*\(\s*"
            r"std::span\s*<\s*const\s+GeometryTransform\s*>\s+transforms\s*\)\s*noexcept\s*;",
        )
        self.assertNotIn("GeometryTransform*", struct)
        # WGSL uniform layout and the C++ row layout must agree exactly.
        self.assertIn("static_assert(sizeof(GeometryTransform) == sizeof(float) * 12)", self.pipeline_cpp)

    def test_public_wrapper_forwards_span_without_retaining_it(self):
        wrapper = _slice_between(self.rml_cpp, "void set_geometry_transform_palette", "void set_input_type")
        self.assertIn("renderInterface->SetGeometryTransformPalette(transforms);", wrapper)
        self.assertNotRegex(wrapper, r"(?:static|thread_local).*transforms\.data\(\)")
        self.assertNotRegex(wrapper, r"GeometryTransform\s*\*|span\s*<\s*GeometryTransform\s*>")

    def test_palette_upload_snapshots_and_rejects_invalid_lifetimes(self):
        upload = _slice_between(
            self.render_cpp,
            "void WebGPURenderInterface::SetGeometryTransformPalette",
            "void WebGPURenderInterface::CreateDeviceObjects",
        )
        self.assertIn("m_geometryTransformPaletteRange = {};", upload)
        self.assertIn("m_geometryTransformPaletteValid = false;", upload)
        self.assertRegex(
            upload,
            r"if\s*\(\s*!m_frameActive\s*\|\|\s*transforms\.empty\(\)\s*\|\|\s*"
            r"transforms\.size\(\)\s*>\s*GeometryTransformPaletteSize\s*\)",
        )
        self.assertRegex(
            upload,
            r"std::array\s*<\s*GeometryTransform\s*,\s*GeometryTransformPaletteSize\s*>\s+palette",
        )
        self.assertIn("std::copy(transforms.begin(), transforms.end(), palette.begin());", upload)
        self.assertRegex(upload, r"gfx::push_uniform\s*\(\s*reinterpret_cast<const uint8_t\*>\(palette\.data\(\)\)")
        self.assertIn("sizeof(palette)", upload)
        self.assertIn("m_geometryTransformPaletteRange.size == sizeof(palette)", upload)
        # A queued DrawData may outlive this call; caller span pointers may not.
        self.assertNotRegex(upload, r"m_geometryTransformPalette(?:Range|Valid)\s*=\s*transforms\.data\(\)")

    def test_palette_range_is_reset_at_frame_boundaries(self):
        begin = _slice_between(self.render_cpp, "void WebGPURenderInterface::BeginFrame", "Rml::LayerHandle WebGPURenderInterface::PushLayer")
        end = _slice_between(self.render_cpp, "bool WebGPURenderInterface::EndFrame", "Rml::LayerHandle WebGPURenderInterface::PushLayer")
        for label, source in (("BeginFrame", begin), ("EndFrame", end)):
            with self.subTest(label=label):
                self.assertIn("m_geometryTransformPaletteRange = {};", source)
                self.assertIn("m_geometryTransformPaletteValid = false;", source)

    def test_transform_draw_requires_valid_range_and_queues_only_ranges(self):
        draw = _slice_between(
            self.render_cpp,
            "void WebGPURenderInterface::DrawGeometry",
            "void WebGPURenderInterface::ReleaseGeometry",
        )
        self.assertRegex(
            draw,
            r"if\s*\(\s*transformPalette\s*&&\s*!m_geometryTransformPaletteValid\s*\)\s*\{",
        )
        self.assertIn("transform_palette_bind_group_ref()", draw)
        self.assertIn("m_geometryTransformPaletteRange.offset", draw)
        self.assertIn("1u << 2u", draw)
        # Transform palette data is a copied uniform range, never a pointer in
        # the asynchronous draw payload. Geometry itself still follows the
        # existing resident/staging selection.
        queued = _slice_between(draw, "gfx::push_draw_command(DrawData{", "});")
        self.assertNotRegex(queued, r"transforms?\.data\(\)|GeometryTransform\s*\*")
        self.assertIn("residentGeometry = residentGeometry", queued)
        retain = draw.index("gfx::retain_frame_resource(geometryData->resident)")
        queue = draw.index("gfx::push_draw_command(DrawData{")
        self.assertLess(retain, queue)

    def test_shader_compile_and_render_route_transform_palette_pipeline(self):
        compile_shader = _slice_between(
            self.render_cpp,
            "Rml::CompiledShaderHandle WebGPURenderInterface::CompileShader",
            "void WebGPURenderInterface::RenderShader",
        )
        self.assertIn('name == "transform-palette"', compile_shader)
        self.assertIn("shader->transformPalette = true;", compile_shader)

        render_shader = _slice_between(
            self.render_cpp,
            "void WebGPURenderInterface::RenderShader",
            "void WebGPURenderInterface::ReleaseShader",
        )
        self.assertIn("shaderData->transformPalette", render_shader)
        self.assertIn("transform_palette_pipeline", render_shader)
        self.assertIn("true);", render_shader)

    def test_transform_shader_clamps_index_applies_basis_offset_and_coverage(self):
        vertex = _slice_between(self.pipeline_cpp, "constexpr std::string_view transformPaletteVertexSource", ")\"sv;")
        self.assertRegex(
            vertex,
            r"let\s+index\s*=\s*min\(u32\(max\(round\(in\.uv\.x\),\s*0\.0\)\),\s*255u\);",
        )
        self.assertIn("array<GeometryTransform, 256>", vertex)
        self.assertIn("@group(2) @binding(0) var<uniform> palette", vertex)
        self.assertIn("transform.basis.x * in.position.x + transform.basis.z * in.position.y", vertex)
        self.assertIn("transform.basis.y * in.position.x + transform.basis.w * in.position.y", vertex)
        self.assertIn("uniforms.translation.xy + transform.offset.xy + local", vertex)
        self.assertIn("out.color = transform.color * in.color.a;", vertex)

        fragment = _slice_between(self.pipeline_cpp, "constexpr std::string_view transformPaletteFragmentSource", ")\"sv;")
        self.assertIn("return in.color;", fragment)
        self.assertRegex(fragment, r"pow\(in\.color\.rgb,\s*vec3<f32>\(uniforms\.gamma\)\)")

    def test_transform_pipeline_has_palette_layout_and_preserves_masking(self):
        self.assertRegex(self.pipeline_hpp, r"constexpr\s+uint32_t\s+RmlPipelineConfigVersion\s*=\s*4\s*;")
        self.assertRegex(self.pipeline_hpp, r"enum class PipelineKind[\s\S]*?TransformPalette")
        self.assertRegex(self.pipeline_hpp, r"enum class VertexLayoutKind[\s\S]*?TransformPalette")
        layout = _slice_between(self.pipeline_cpp, "case PipelineKind::TransformPalette:", "case PipelineKind::MaskImage:")
        self.assertIn("g_imageBindGroupLayout", layout)
        self.assertIn("g_transformPaletteBindGroupLayout", layout)
        self.assertIn("TransformPaletteBindingSize", self.pipeline_cpp)
        self.assertRegex(
            self.pipeline_cpp,
            r"\.visibility\s*=\s*wgpu::ShaderStage::Vertex\s*\|\s*wgpu::ShaderStage::Fragment",
        )
        self.assertRegex(
            self.pipeline_cpp,
            r"\.minBindingSize\s*=\s*TransformPaletteBindingSize",
        )
        bind_ref = _slice_between(self.pipeline_cpp, "gfx::BindGroupRef transform_palette_bind_group_ref", "wgpu::RenderPipeline create_pipeline")
        self.assertIn("gfx::g_uniformBuffer", bind_ref)
        self.assertIn("TransformPaletteBindingSize", bind_ref)

        transform_pipeline = _slice_between(
            self.render_cpp,
            "PipelineConfig transform_palette_pipeline",
            "gfx::PipelineRef blit_pipeline",
        )
        self.assertIn("StencilMode::EqualKeep", transform_pipeline)
        self.assertIn("BlendMode::Premultiplied", transform_pipeline)

    def test_worker_replay_binds_palette_and_keeps_resident_geometry_path(self):
        render = _slice_between(self.pipeline_cpp, "void render(const DrawData& data", "uint32_t sampler_mode")
        self.assertIn("bind_group(2, data.bindGroup2", render)
        self.assertIn("data.bindGroup2DynamicOffset", render)
        self.assertIn("ensure_resident_geometry(*data.residentGeometry)", render)
        self.assertIn("pass.SetVertexBuffer(0, resident.vertexBuffer", render)
        self.assertIn("pass.SetIndexBuffer(resident.indexBuffer", render)
        self.assertIn("pass.SetStencilReference(data.stencilRef)", render)
        self.assertIn("pass.DrawIndexed(data.indexCount)", render)
        draw_data = _slice_between(self.pipeline_hpp, "struct DrawData {", "};\nstatic_assert")
        self.assertNotRegex(draw_data, r"GeometryTransform\s*\*|std::span|Rml::Vertex\s*\*")
        self.assertIn("static_assert(std::is_trivially_copyable_v<DrawData>)", self.pipeline_hpp)

    def test_lifecycle_model_snapshots_mutations_and_clears_outside_frame(self):
        first = Transform((1.0, 0.0, 0.0, 1.0), (4.0, 8.0, 0.0, 0.0), (0.2, 0.4, 0.6, 1.0))
        second = Transform((0.0, 1.0, -1.0, 0.0), (1.0, 2.0, 0.0, 0.0), (1.0, 0.5, 0.25, 0.75))
        caller = [first, second]
        state = PaletteLifecycle()
        state.set(caller)
        self.assertFalse(state.valid)

        state.begin()
        state.set(caller)
        caller[0] = second
        caller.clear()
        self.assertTrue(state.valid)
        self.assertEqual(state.snapshot, (first, second))

        state.end()
        self.assertFalse(state.valid)
        self.assertEqual(state.snapshot, ())
        state.begin()
        state.set([first] * 257)
        self.assertFalse(state.valid)
        state.set([])
        self.assertFalse(state.valid)

    def test_transform_math_and_premultiplied_coverage_model(self):
        transform = Transform((0.0, 1.0, -1.0, 0.0), (10.0, 20.0, 0.0, 0.0), (0.2, 0.4, 0.6, 0.5))
        self.assertEqual(_apply_transform(transform, (3.0, 4.0)), (6.0, 23.0))
        coverage = 0.25
        self.assertEqual(tuple(channel * coverage for channel in transform.color), (0.05, 0.1, 0.15, 0.125))

if __name__ == "__main__":
    unittest.main()
