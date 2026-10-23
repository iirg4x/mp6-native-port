"""Source and lifetime contracts for borrowed RmlUi geometry.

RmlUi keeps the vertex span passed to CompileGeometry alive until
ReleaseGeometry.  The backend may therefore retain that span for ordinary
geometry, but indices still need renderer-owned normalization and every
asynchronous draw must receive either a staging range or a retained resident
resource.  These tests intentionally inspect bounded function slices and
replay the complete pinned Aurora patch queue; a broad whole-file substring
check would miss the lifetime/order regressions this optimization can cause.
"""

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


PATCH = ROOT / "compat/aurora/base/0068-rml-borrow-vertices.patch"
SOURCE_ROOT = ROOT / "third_party/aurora"
CPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.cpp"
HPP = SOURCE_ROOT / "lib/rmlui/WebGPURenderInterface.hpp"
PIPELINE_CPP = SOURCE_ROOT / "lib/rmlui/pipeline.cpp"
PIPELINE_HPP = SOURCE_ROOT / "lib/rmlui/pipeline.hpp"

TARGETS = {
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


def _draw_initializer(function_source: str) -> str:
    start = function_source.index("gfx::push_draw_command(DrawData{")
    end = function_source.index("\n  });", start)
    return function_source[start:end]


class RmlBorrowedGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = CPP.read_text(encoding="utf-8")
        cls.hpp = HPP.read_text(encoding="utf-8")
        cls.pipeline_cpp = PIPELINE_CPP.read_text(encoding="utf-8")
        cls.pipeline_hpp = PIPELINE_HPP.read_text(encoding="utf-8")

    def test_compile_geometry_borrows_vertices_but_normalizes_owned_indices(self):
        data = _slice_between(self.cpp, "struct ShaderGeometryData {", "struct ShaderTextureData {")
        self.assertRegex(data, r"\bRml::Span<const Rml::Vertex>\s+vertices\s*;")
        self.assertRegex(data, r"\bstd::vector<uint32_t>\s+indices\s*;")
        self.assertNotRegex(data, r"std::vector\s*<\s*Rml::Vertex\s*>")

        compile = _slice_between(
            self.cpp,
            "Rml::CompiledGeometryHandle WebGPURenderInterface::CompileGeometry",
            "\n}\n\nvoid WebGPURenderInterface::RenderGeometry",
        )
        self.assertIn("geometryData->vertices = vertices;", compile)
        self.assertRegex(compile, r"indices\.reserve\(indices\.size\(\)\)")
        self.assertRegex(compile, r"for\s*\(const int index : indices\)")
        self.assertIn("static_cast<uint32_t>(index)", compile)
        self.assertNotIn("assign(vertices", compile)
        self.assertNotRegex(compile, r"vertices\.data\(\).*indices")

    def test_draw_and_gradient_stage_borrowed_vertices_before_queueing_draw(self):
        draw = _slice_between(
            self.cpp,
            "void WebGPURenderInterface::DrawGeometry",
            "\n}\n\nvoid WebGPURenderInterface::ReleaseGeometry",
        )
        shader = _slice_between(
            self.cpp,
            "void WebGPURenderInterface::RenderShader",
            "\n}\n\nvoid WebGPURenderInterface::ReleaseShader",
        )

        for label, function_source in (("DrawGeometry", draw), ("RenderShader", shader)):
            vertex_push = function_source.index("gfx::push_verts(")
            index_push = function_source.index("gfx::push_indices(")
            draw_queue = function_source.index("gfx::push_draw_command(DrawData{")
            self.assertLess(vertex_push, draw_queue, label)
            self.assertLess(index_push, draw_queue, label)
            initializer = _draw_initializer(function_source)
            # DrawData is copied into an asynchronous frame packet. No Rml
            # span/vector pointer may escape through that trivially-copyable
            # payload; only ranges or a retained resident handle are allowed.
            # Scalar metadata such as indexCount may legitimately read the
            # borrowed geometry's size. Only pointer/span escapes are unsafe.
            self.assertNotRegex(
                initializer,
                r"geometryData->(?:vertices|indices)\.data\(\)|\b(?:vertices|indices)\.data\(\)",
                label,
            )
            self.assertNotRegex(initializer, r"Rml::Span|Rml::Vertex\s*\*|uint8_t\s*\*", label)

        # ResidentGeometry is a raw pointer in DrawData by design, so the
        # owning shared_ptr must be pinned before the queue copy is published.
        retain = draw.index("gfx::retain_frame_resource(geometryData->resident)")
        queued = draw.index("gfx::push_draw_command(DrawData{")
        self.assertLess(retain, queued)

        draw_data = _slice_between(self.pipeline_hpp, "struct DrawData {", "};\nstatic_assert")
        self.assertIn("const ResidentGeometry* residentGeometry", draw_data)
        self.assertNotIn("Rml::Span", draw_data)
        self.assertNotRegex(draw_data, r"(?:Rml::Vertex|uint8_t|uint32_t)\s*\*")
        self.assertIn("static_assert(std::is_trivially_copyable_v<DrawData>)", self.pipeline_hpp)

    def test_clip_reset_releases_borrowed_vertices_before_replacing_array(self):
        ensure = _slice_between(
            self.cpp,
            "void WebGPURenderInterface::EnsureClipResetGeometry",
            "\n}\n\nvoid WebGPURenderInterface::NewFrame",
        )
        existing = ensure.index("if (m_clipResetGeometry != 0)")
        release = ensure.index("ReleaseGeometry(m_clipResetGeometry)", existing)
        clear_handle = ensure.index("m_clipResetGeometry = 0", release)
        replace_vertices = ensure.index("m_clipResetVertices =", existing)
        self.assertLess(release, replace_vertices)
        self.assertLess(clear_handle, replace_vertices)
        self.assertIn("CompileGeometry({m_clipResetVertices.data(), m_clipResetVertices.size()}", ensure)
        self.assertRegex(
            self.hpp,
            r"std::array<Rml::Vertex,\s*4>\s+m_clipResetVertices\s*(?:\{\})?\s*;",
        )

    def test_resident_promotion_owns_bytes_before_async_queue(self):
        create = _slice_between(
            self.cpp,
            "std::shared_ptr<ResidentGeometry> create_resident_geometry",
            "// UniformBlock",
        )
        vertex_resize = create.index("resident->vertexBytes.resize(resident->vertexSize)")
        index_resize = create.index("resident->indexBytes.resize(resident->indexSize)")
        vertex_copy = create.index("std::memcpy(resident->vertexBytes.data(), geometry.vertices.data()")
        index_copy = create.index("std::memcpy(resident->indexBytes.data(), geometry.indices.data()")
        returned = create.index("return resident;")
        self.assertLess(vertex_resize, vertex_copy)
        self.assertLess(index_resize, index_copy)
        self.assertLess(vertex_copy, returned)
        self.assertLess(index_copy, returned)
        self.assertNotIn("CreateBuffer", create)
        self.assertRegex(self.pipeline_hpp, r"std::vector<uint8_t>\s+vertexBytes\s*;")
        self.assertRegex(self.pipeline_hpp, r"std::vector<uint8_t>\s+indexBytes\s*;")

        # The worker-side upload reads those owned bytes, not the borrowed Rml
        # span. This is a lifetime and thread-boundary check, not just an API
        # name check.
        ensure_upload = _slice_between(
            self.pipeline_cpp,
            "bool ensure_resident_geometry",
            "constexpr std::string_view vertexSource",
        )
        self.assertIn("render_worker::is_worker_thread()", ensure_upload)
        self.assertIn("resident.vertexBytes", ensure_upload)
        self.assertIn("resident.indexBytes", ensure_upload)
        self.assertIn("gpuReady.store(true, std::memory_order_release)", ensure_upload)

if __name__ == "__main__":
    unittest.main()
