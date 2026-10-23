"""Contracts for bounded RmlUi frame-local pipeline and binding reuse."""

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


SOURCE_ROOT = ROOT / "third_party/aurora"
RENDER_CPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.cpp"
RENDER_HPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.hpp"
PIPELINE_CPP = SOURCE_ROOT / "lib/rmlui/pipeline.cpp"
PIPELINE_HPP = SOURCE_ROOT / "lib/rmlui/pipeline.hpp"
PATCH = ROOT / "compat/aurora/base/0070-rml-frame-state-reuse.patch"
TARGETS = {
    "lib/rmlui/WebGPURenderInterface.cpp",
    "lib/rmlui/WebGPURenderInterface.hpp",
    "lib/rmlui/pipeline.cpp",
    "lib/rmlui/pipeline.hpp",
}


def section(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first:source.index(end, first + len(start))]


class RmlFrameStateReuseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.render_cpp = RENDER_CPP.read_text(encoding="utf-8")
        cls.render_hpp = RENDER_HPP.read_text(encoding="utf-8")
        cls.pipeline_cpp = PIPELINE_CPP.read_text(encoding="utf-8")
        cls.pipeline_hpp = PIPELINE_HPP.read_text(encoding="utf-8")

    def test_pipeline_cache_is_exact_bounded_and_ref_created_only_on_miss(self):
        body = section(self.render_cpp, "gfx::PipelineRef WebGPURenderInterface::FramePipeline", "gfx::BindGroupRef WebGPURenderInterface::FrameTextureBindGroup")
        self.assertIn("std::memcpy(key.data(), &config, sizeof(config));", body)
        self.assertIn("entry.valid && entry.key == key", body)
        self.assertLess(body.index("entry.valid && entry.key == key"), body.index("gfx::pipeline_ref(config)"))
        self.assertIn("m_pipelineCacheNext++ % m_pipelineCache.size()", body)
        self.assertIn("PipelineCacheCapacity = 24", self.render_hpp)

    def test_geometry_gradient_and_transform_use_frame_pipeline_cache(self):
        self.assertIn("FramePipeline(geometry_pipeline", self.render_cpp)
        self.assertIn("FramePipeline(gradient_pipeline", self.render_cpp)
        self.assertIn("FramePipeline(transform_palette_pipeline", self.render_cpp)
        geometry = section(self.render_cpp, "PipelineConfig geometry_pipeline", "PipelineConfig gradient_pipeline")
        gradient = section(self.render_cpp, "PipelineConfig gradient_pipeline", "PipelineConfig transform_palette_pipeline")
        transform = section(self.render_cpp, "PipelineConfig transform_palette_pipeline", "gfx::PipelineRef blit_pipeline")
        for helper in (geometry, gradient, transform):
            self.assertNotIn("gfx::pipeline_ref(", helper)

    def test_texture_cache_uses_exact_view_identity_and_retains_view_owner(self):
        body = section(self.render_cpp, "gfx::BindGroupRef WebGPURenderInterface::FrameTextureBindGroup", "gfx::BindGroupRef WebGPURenderInterface::FrameUniformBindGroup")
        self.assertIn("reinterpret_cast<uintptr_t>(view.Get())", body)
        self.assertIn("entry.valid && entry.identity == identity", body)
        self.assertIn(".view = view", body)
        self.assertLess(body.index("entry.valid && entry.identity == identity"), body.index("texture_bind_group_ref(view)"))
        self.assertIn("TextureBindGroupCacheCapacity = 32", self.render_hpp)

    def test_uniform_and_palette_bind_groups_are_singletons_within_frame(self):
        uniform = section(self.render_cpp, "gfx::BindGroupRef WebGPURenderInterface::FrameUniformBindGroup", "gfx::BindGroupRef WebGPURenderInterface::FrameTransformPaletteBindGroup")
        palette = section(self.render_cpp, "gfx::BindGroupRef WebGPURenderInterface::FrameTransformPaletteBindGroup", "Rml::CompiledGeometryHandle WebGPURenderInterface::CompileGeometry")
        self.assertIn("m_uniformBindGroupRef == 0", uniform)
        self.assertIn("uniform_bind_group_ref()", uniform)
        self.assertIn("m_transformPaletteBindGroupRef == 0", palette)
        self.assertIn("transform_palette_bind_group_ref()", palette)

    def test_all_frame_refs_reset_at_every_lifetime_boundary(self):
        reset = section(self.render_cpp, "void WebGPURenderInterface::ResetFrameStateCache", "gfx::PipelineRef WebGPURenderInterface::FramePipeline")
        for token in ("m_pipelineCache = {};", "m_textureBindGroupCache = {};", "m_uniformBindGroupRef = 0;", "m_transformPaletteBindGroupRef = 0;"):
            self.assertIn(token, reset)
        for start, end in (
            ("void WebGPURenderInterface::BeginFrame", "bool WebGPURenderInterface::EndFrame"),
            ("bool WebGPURenderInterface::EndFrame", "Rml::LayerHandle WebGPURenderInterface::PushLayer"),
            ("void WebGPURenderInterface::CreateDeviceObjects", "void WebGPURenderInterface::NewFrame"),
        ):
            self.assertIn("ResetFrameStateCache();", section(self.render_cpp, start, end))
        setter = section(self.render_hpp, "void SetRenderTargetFormat", "wgpu::TextureView GetClipMaskStencilView")
        self.assertIn("m_renderTargetFormat != render_target_format", setter)
        self.assertIn("ResetFrameStateCache();", setter)

    def test_stencil_reference_is_memoized_and_invalidation_clears_it(self):
        state = section(self.pipeline_hpp, "struct RmlPassBindingState", "void initialize_pipeline")
        self.assertIn("uint32_t stencilRef = 0;", state)
        self.assertIn("bool stencilRefValid = false;", state)
        self.assertIn("stencilRefValid = false;", state)
        render = section(self.pipeline_cpp, "void render(const DrawData& data", "uint32_t sampler_mode")
        self.assertIn("!state.stencilRefValid || state.stencilRef != data.stencilRef", render)
        self.assertEqual(render.count("pass.SetStencilReference(data.stencilRef)"), 1)

    def test_zero_blend_constant_is_removed_from_normal_and_gradient_draws(self):
        draw = section(self.render_cpp, "void WebGPURenderInterface::DrawGeometry", "void WebGPURenderInterface::ReleaseGeometry")
        self.assertIn("setBlendConstant ? 1u : 0u", draw)
        gradient = section(self.render_cpp, "void WebGPURenderInterface::RenderShader", "void WebGPURenderInterface::ReleaseShader")
        self.assertNotIn(".hasBlendConstant = 1", gradient)
        clip = section(self.render_cpp, "void WebGPURenderInterface::RenderToClipMask", "void WebGPURenderInterface::SetTransform")
        self.assertIn("0.f, false, true", clip)

if __name__ == "__main__":
    unittest.main()
