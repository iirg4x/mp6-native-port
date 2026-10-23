"""Offline invariants for the pinned XeGTAO-to-WGSL bridge."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "compile_xegtao", ROOT / "tools" / "compile_xegtao.py"
)
assert SPEC and SPEC.loader
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def _reflection(*names: str) -> dict:
    return {
        "entryPoints": [
            {
                "bindings": [
                    {"name": name, "binding": {"used": 1}} for name in names
                ]
            }
        ]
    }


class XegtaoToolchainTests(unittest.TestCase):
    def test_pinned_sources_have_canonical_hashes(self) -> None:
        tool.verify_upstream()


    def test_write_only_accepts_store_only_output(self) -> None:
        source = """@group(2) @binding(3) var g_outWorkingAOTerm_0 : texture_storage_2d<r32uint, read_write>;
fn main() { textureStore((g_outWorkingAOTerm_0), vec2<i32>(0), vec4<u32>(1, 0, 0, 1)); }
"""
        converted, outputs = tool.make_write_only(
            source, _reflection("g_outWorkingAOTerm")
        )
        self.assertNotIn("read_write", converted)
        self.assertIn("texture_storage_2d<r32uint, write>", converted)
        self.assertEqual(outputs[0]["storageAccess"], "write")


    def test_write_only_rejects_output_escape(self) -> None:
        declaration = "@group(2) @binding(3) var g_outWorkingAOTerm_0 : texture_storage_2d<r32uint, read_write>;\n"
        escaped = (
            "fn helper(x: texture_storage_2d<r32uint, read_write>) {}\n"
            "fn main() { helper(g_outWorkingAOTerm_0); }\n"
        )
        with self.assertRaises(tool.ToolchainError):
            tool.make_write_only(declaration + escaped, _reflection("g_outWorkingAOTerm"))

        escaped = (
            "fn main() { textureStore((g_outWorkingAOTerm_0), vec2<i32>(0), "
            "vec4<u32>(1, 0, 0, 1)); helper(g_outWorkingAOTerm_0); }\n"
        )
        with self.assertRaises(tool.ToolchainError):
            tool.make_write_only(declaration + escaped, _reflection("g_outWorkingAOTerm"))

        escaped = "fn main() { textureGather((g_outWorkingAOTerm_0), vec2<f32>(0.0)); }\n"
        with self.assertRaises(tool.ToolchainError):
            tool.make_write_only(declaration + escaped, _reflection("g_outWorkingAOTerm"))


    def test_scalar_outputs_are_saturated_like_r8_uint(self) -> None:
        source = (
            "@group(2) @binding(3) var g_outWorkingAOTerm_0 : texture_storage_2d<r32uint, read_write>;\n"
            "fn main() { textureStore((g_outWorkingAOTerm_0), vec2<i32>(0), vec4<u32>((u32(value)), 0, 0, 1)); }\n"
        )
        converted, changed = tool.clamp_scalar_outputs(source, "CSGTAOHigh")
        self.assertTrue(changed)
        self.assertIn("min(255u, u32(value))", converted)


    def test_compiler_version_is_pinned(self) -> None:
        self.assertEqual(tool.SLANG_VERSION, "2026.19")
        self.assertEqual(
            tool.SLANG_SHA256,
            "fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33",
        )

    def test_upstream_ultra_uses_nine_slices_and_three_steps(self) -> None:
        source = tool.BRIDGE.read_text()
        body = source.split('void CSGTAOUltra(', 1)[1].split('\n}', 1)[0]
        self.assertIn('XeGTAO_MainPass(pixCoord, 9, 3,', body)
        self.assertIn('CSGTAOUltra', tool.STAGES)
        with self.assertRaises(tool.ToolchainError):
            tool.clamp_scalar_outputs('// no scalar output', 'CSGTAOUltra')

    def test_compiler_version_mismatch_is_rejected(self) -> None:
        completed = mock.Mock(returncode=0, stdout="2026.18\n", stderr="")
        with mock.patch.object(tool.subprocess, "run", return_value=completed):
            with self.assertRaises(tool.ToolchainError):
                tool.verify_slangc_version(Path("mock-slangc.exe"))

    def test_header_embeds_only_accepted_wgsl(self) -> None:
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
            manifest = {
                "entries": entries,
                "upstream": {"commit": tool.UPSTREAM_COMMIT},
                "compiler": {"version": tool.SLANG_VERSION},
            }
            header = root / "generated" / "xegtao_shaders.hpp"
            tool.emit_header(root, header, manifest)
            text = header.read_text(encoding="utf-8")
            self.assertIn("namespace mp6::xegtao::shaders", text)
            for stage in tool.STAGES:
                self.assertIn(f"inline constexpr char {stage}[]", text)
                self.assertIn(f"// {stage}", text)
            self.assertNotIn(".raw.wgsl", text)


if __name__ == "__main__":
    unittest.main()
