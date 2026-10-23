"""CPU-only contracts for reproducible R16 XeGTAO shader generation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("compile_xegtao_r16", ROOT / "tools" / "compile_xegtao.py")
assert SPEC and SPEC.loader
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def _prefilter_source() -> str:
    declarations = "\n".join(
        f"@binding({binding}) @group(1) var g_outWorkingDepthMIP{index}_0 : "
        "texture_storage_2d<r32float, write>;"
        for index, binding in enumerate(range(1, 6))
    )
    stores = "\n".join(
        f"fn write{index}() {{ textureStore((g_outWorkingDepthMIP{index}_0), "
        "vec2<i32>(0), vec4<f32>(1.0, 0, 0, 1)); }}"
        for index in range(5)
    )
    return (
        declarations
        + "\n@binding(4) @group(2) var g_outWorkingEdges_0 : "
        "texture_storage_2d<r32float, write>;\n"
        + stores
        + "\n"
    )


class XegtaoR16ToolchainTests(unittest.TestCase):
    def test_storage_selector_and_bias_constants_are_pinned(self):
        self.assertEqual(tool.DEPTH_STORAGE_FORMATS, ("r32float", "r16float"))
        self.assertEqual(tool.R32_DEPTH_BIAS, 0.99999)
        self.assertEqual(tool.R16_DEPTH_BIAS, 0.99920)
        self.assertEqual(tool.R16_STORAGE_DEFINE, "MP6_XEGTAO_R16_STORAGE")

    def test_bridge_keeps_fp32_default_and_conditional_r16_selector(self):
        source = tool.BRIDGE.read_text(encoding="utf-8")
        self.assertIn("#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0", source)
        self.assertIn("#if !defined(MP6_XEGTAO_R16_STORAGE)", source)
        self.assertIn("#define XE_GTAO_FP32_DEPTHS 1", source)
        self.assertIn('"../../../third_party/xegtao/XeGTAO.h"', source)
        self.assertIn('"../../../third_party/xegtao/XeGTAO.hlsli"', source)

    def test_adaptation_changes_exactly_five_prefilter_depth_uavs(self):
        transformed, metadata = tool.adapt_prefilter_depth_storage(_prefilter_source())
        self.assertEqual(metadata["group"], 1)
        self.assertEqual(metadata["bindings"], [1, 2, 3, 4, 5])
        self.assertEqual(transformed.count("texture_storage_2d<r16float, write>"), 5)
        self.assertEqual(transformed.count("texture_storage_2d<r32float, write>"), 1)
        self.assertEqual(transformed.count("textureStore"), 5)
        self.assertTrue(all(item["format"] == "r16float" for item in metadata["declarations"]))

    def test_adaptation_rejects_output_escape_and_wrong_binding(self):
        escaped = _prefilter_source().replace(
            "textureStore((g_outWorkingDepthMIP0_0)",
            "textureLoad(g_outWorkingDepthMIP0_0, vec2<i32>(0)); textureStore((g_outWorkingDepthMIP0_0)",
            1,
        )
        with self.assertRaises(tool.ToolchainError):
            tool.adapt_prefilter_depth_storage(escaped)

        wrong_binding = _prefilter_source().replace("@binding(5) @group(1)", "@binding(6) @group(1)", 1)
        with self.assertRaises(tool.ToolchainError):
            tool.adapt_prefilter_depth_storage(wrong_binding)

    def test_depth_bias_proof_requires_matching_main_stage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for stage in tool.MAIN_DEPTH_STAGES:
                (root / f"{stage}.wgsl").write_text(
                    "var depthBias : f32 = 0.99919998645782471f;\n", encoding="utf-8"
                )
            tool.verify_depth_bias(root, tool.R16_DEPTH_BIAS)
            with self.assertRaises(tool.ToolchainError):
                tool.verify_depth_bias(root, tool.R32_DEPTH_BIAS)

    def test_run_compiler_adds_define_only_for_r16(self):
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # The mock compiler call only needs to leave both expected outputs.
            def run(command, **kwargs):
                output = Path(command[command.index("-o") + 1])
                reflection = Path(command[command.index("-reflection-json") + 1])
                output.write_text("// generated", encoding="utf-8")
                reflection.write_text('{"entryPoints":[{"bindings":[]}]}' , encoding="utf-8")
                return completed

            with mock.patch.object(tool.subprocess, "run", side_effect=run):
                _raw, _reflection, command = tool.run_compiler(
                    Path("slangc.exe"), "CSGenerateNormals", root, 30.0, "r32float"
                )
                self.assertNotIn("-DMP6_XEGTAO_R16_STORAGE=1", command)
                _raw, _reflection, command = tool.run_compiler(
                    Path("slangc.exe"), "CSGenerateNormals", root, 30.0, "r16float"
                )
                self.assertIn("-DMP6_XEGTAO_R16_STORAGE=1", command)

    def test_emit_header_supports_separate_r16_namespace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = []
            for stage in tool.STAGES:
                stage_path = root / f"{stage}.wgsl"
                stage_path.write_text(f"// {stage}\n", encoding="utf-8")
                entries.append(
                    {
                        "name": stage,
                        "wgsl": stage_path.name,
                        "wgslSha256": tool.sha256(stage_path),
                    }
                )
            manifest = {"entries": entries}
            header = root / "generated_r16_shaders.hpp"
            tool.emit_header(root, header, manifest, "mp6::xegtao::r16_shaders")
            text = header.read_text(encoding="utf-8")
            self.assertIn("namespace mp6::xegtao::r16_shaders {", text)
            self.assertIn("} // namespace mp6::xegtao::r16_shaders", text)
            self.assertNotIn("namespace mp6::xegtao::shaders {", text)

    def test_checked_in_r16_header_is_separate_and_complete(self):
        header = ROOT / "src" / "gx" / "xegtao" / "generated_r16_shaders.hpp"
        self.assertTrue(header.is_file())
        text = header.read_text(encoding="utf-8")
        self.assertIn("namespace mp6::xegtao::r16_shaders {", text)
        self.assertIn("texture_storage_2d<r16float, write>", text)
        self.assertNotIn("namespace mp6::xegtao::shaders {", text)
        for stage in tool.STAGES:
            self.assertIn(f"inline constexpr char {stage}[]", text)


if __name__ == "__main__":
    unittest.main()
