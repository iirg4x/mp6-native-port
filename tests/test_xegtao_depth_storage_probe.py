"""CPU-only contracts for the bounded XeGTAO R16 storage probe."""

from __future__ import annotations

import importlib.util
import ast
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tests" / "integration" / "compare_xegtao_depth_storage.py"
SPEC = importlib.util.spec_from_file_location("compare_xegtao_depth_storage", MODULE_PATH)
assert SPEC and SPEC.loader
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


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
        "@binding(0) @group(1) var g_srcRawDepthPrefilter_0 : texture_2d<f32>;\n"
        + declarations
        + "\n@binding(4) @group(2) var g_outWorkingEdges_0 : "
        "texture_storage_2d<r32float, write>;\n"
        + stores
        + "\n"
    )


class XegtaoDepthStorageProbeTests(unittest.TestCase):
    def test_import_surface_is_gpu_free(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        tree = ast.parse(source)
        top_level_imports = {
            alias.name.split(".", 1)[0]
            for node in tree.body
            if isinstance(node, ast.Import)
            for alias in node.names
        }
        self.assertNotIn("numpy", top_level_imports)
        self.assertNotIn("wgpu", top_level_imports)
        self.assertIn("def run_gpu_comparison", source)
        self.assertIn("import numpy as np", source)
        self.assertIn("import wgpu", source)

    def test_output_path_is_new_and_inside_build(self):
        with tempfile.TemporaryDirectory(dir=probe.BUILD_ROOT) as temporary:
            root = Path(temporary)
            output = root / "report.json"
            self.assertEqual(probe.validate_output_path(output), output.resolve())
            output.write_text("{}", encoding="utf-8")
            with self.assertRaises(probe.ProbeError):
                probe.validate_output_path(output)

        outside = Path(tempfile.gettempdir()) / "mp6-r16-storage-report.json"
        outside.unlink(missing_ok=True)
        with self.assertRaises(probe.ProbeError):
            probe.validate_output_path(outside)

    def test_variant_bridge_removes_only_fp32_selector_and_absolutizes_includes(self):
        production = probe.PRODUCTION_BRIDGE.read_text(encoding="utf-8")
        transformed = probe.variant_bridge_text(production)
        self.assertEqual(production.count(probe._FP32_DEFINE), 1)
        self.assertNotIn(probe._FP32_DEFINE, transformed)
        self.assertEqual(transformed.count(probe._HALF_DEFINE), 1)
        self.assertIn(
            f'#include "{(probe.ROOT / "third_party" / "xegtao" / "XeGTAO.h").resolve().as_posix()}"',
            transformed,
        )
        self.assertIn(
            f'#include "{(probe.ROOT / "third_party" / "xegtao" / "XeGTAO.hlsli").resolve().as_posix()}"',
            transformed,
        )

        # The requested bridge edit is otherwise source-preserving: remove the
        # selector and replace exactly the two include lines.
        expected = production.replace(probe._FP32_DEFINE + "\n", "", 1)
        for old, path in probe._INCLUDE_LINES.items():
            expected = expected.replace(old, f'#include "{path.resolve().as_posix()}"', 1)
        self.assertEqual(transformed, expected)

    def test_generation_uses_production_conditional_bridge_and_r16_mode(self):
        source = probe.PRODUCTION_BRIDGE.read_text(encoding="utf-8")
        probe._validate_production_bridge(source)
        self.assertIn("#if !defined(MP6_XEGTAO_R16_STORAGE)", source)
        self.assertIn(probe._FP32_DEFINE, source)
        self.assertIn(probe._HALF_DEFINE, source)
        self.assertNotIn("tool.BRIDGE =", MODULE_PATH.read_text(encoding="utf-8"))
        self.assertIn(
            'tool.compile_all(slangc, shader_dir, timeout, "r16float")',
            MODULE_PATH.read_text(encoding="utf-8"),
        )

        with tempfile.TemporaryDirectory(dir=probe.BUILD_ROOT) as temporary:
            artifact = Path(temporary) / "artifact"
            metadata = probe.create_variant_bridge(artifact)
            self.assertFalse(artifact.exists())
            self.assertEqual(Path(metadata["path"]).resolve(), probe.PRODUCTION_BRIDGE.resolve())
            self.assertEqual(metadata["sourceSha256"], metadata["productionSourceSha256"])
            self.assertEqual(metadata["compileDefine"], "MP6_XEGTAO_R16_STORAGE=1")
            self.assertEqual(metadata["sourceMode"], "production-conditional-bridge")
            self.assertIn("disabled by", metadata["removedDefineAction"])

    def test_prefilter_rewrite_changes_exactly_five_group_one_bindings(self):
        source = _prefilter_source()
        transformed, metadata = probe.rewrite_prefilter_storage(source)
        self.assertEqual(metadata["stage"], probe.DEPTH_STAGE)
        self.assertEqual(metadata["group"], 1)
        self.assertEqual(metadata["bindings"], [1, 2, 3, 4, 5])
        self.assertEqual(transformed.count("texture_storage_2d<r16float, write>"), 5)
        self.assertEqual(transformed.count("texture_storage_2d<r32float, write>"), 1)
        self.assertEqual(transformed.count("textureStore"), 5)
        self.assertTrue(all(item["variantFormat"] == "r16float" for item in metadata["declarations"]))

    def test_prefilter_rewrite_rejects_wrong_binding_or_non_write_storage(self):
        source = _prefilter_source().replace("@binding(5) @group(1)", "@binding(6) @group(1)", 1)
        with self.assertRaises(probe.ProbeError):
            probe.rewrite_prefilter_storage(source)

        source = _prefilter_source().replace("r32float, write", "r32float, read", 1)
        with self.assertRaises(probe.ProbeError):
            probe.rewrite_prefilter_storage(source)

    def test_store_only_proof_rejects_an_output_escape(self):
        source = _prefilter_source().replace(
            "textureStore((g_outWorkingDepthMIP0_0)",
            "textureLoad(g_outWorkingDepthMIP0_0, vec2<i32>(0)); textureStore((g_outWorkingDepthMIP0_0)",
            1,
        )
        with self.assertRaises(probe.ProbeError):
            probe.rewrite_prefilter_storage(source)

    def test_compile_timeout_is_bounded(self):
        self.assertLessEqual(probe.COMPILER_TIMEOUT_SECONDS, 30.0)
        with tempfile.TemporaryDirectory(dir=probe.BUILD_ROOT) as temporary:
            artifact = Path(temporary) / "artifact"
            artifact.mkdir()
            bridge = artifact / "bridge.hlsl"
            bridge.write_text("// test", encoding="utf-8")
            metadata = {"path": str(bridge), "sourceSha256": "test"}
            with self.assertRaises(probe.ProbeError):
                probe.compile_variant(artifact, metadata, timeout=30.001)

    def test_feature_proxy_augments_original_device_request(self):
        class Adapter:
            features = {"texture-adapter-specific-format-features"}

            def __init__(self):
                self.kwargs = None

            def request_device_sync(self, **kwargs):
                self.kwargs = kwargs
                return "device"

        adapter = Adapter()
        wrapped = probe._FeatureAdapterProxy(adapter)
        self.assertEqual(wrapped.request_device_sync(required_features=["timestamp-query"]), "device")
        self.assertEqual(
            adapter.kwargs["required_features"],
            ["timestamp-query", "texture-adapter-specific-format-features"],
        )

    def test_manifest_shape_records_r16_scope(self):
        source = _prefilter_source()
        transformed, metadata = probe.rewrite_prefilter_storage(source)
        self.assertIn("texture_storage_2d<r16float, write>", transformed)
        manifest = {
            "entries": [],
            "depthStorageProbe": {
                "arithmetic": "FP32",
                "bias": {"baselineR32": probe.R32_BIAS, "variantR16": probe.R16_BIAS},
                "prefilter": metadata,
            },
        }
        encoded = json.dumps(manifest)
        self.assertIn('"variantR16": 0.9992', encoded)
        self.assertIn('"baselineR32": 0.99999', encoded)


if __name__ == "__main__":
    unittest.main()
