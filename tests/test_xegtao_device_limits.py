"""Offline contracts for XeGTAO's five-write storage-texture limit."""

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


SOURCE_ROOT = ROOT / "third_party" / "aurora"
SOURCE = SOURCE_ROOT / "lib" / "webgpu" / "gpu.cpp"
TARGET = "lib/webgpu/gpu.cpp"


def _replay_source_queue() -> str:
    """Aurora's current gpu.cpp (third_party/aurora)."""
    return (ROOT / "third_party/aurora" / TARGET).read_text(encoding="utf-8")


class XeGTAODeviceLimitTests(unittest.TestCase):
    def test_requests_five_only_from_a_meaningful_supported_limit(self):
        source = SOURCE.read_text(encoding="utf-8", errors="surrogateescape")
        start = source.index("wgpu::Limits supportedLimits")
        block = source[start : source.index("std::vector<wgpu::FeatureName>", start)]

        self.assertIn("const wgpu::Status supportedLimitsStatus = g_adapter.GetLimits", block)
        self.assertIn("if (supportedLimitsStatus != wgpu::Status::Success)", block)
        self.assertIn("supportedStorageTextures != WGPU_LIMIT_U32_UNDEFINED", block)
        self.assertIn("supportedStorageTextures != 0", block)
        self.assertIn("supportedStorageTextures >= kXeGTAOPrefilterStorageTextures", block)
        self.assertIn("? kXeGTAOPrefilterStorageTextures", block)
        self.assertIn(": WGPU_LIMIT_U32_UNDEFINED", block)
        self.assertIn(
            ".maxStorageBuffersPerShaderStage = 2,\n"
            "        .maxStorageTexturesPerShaderStage = requestedStorageTextures,",
            block,
        )
        self.assertNotIn(".maxStorageTexturesPerShaderStage = 5,", block)

    def test_supported_requested_and_enabled_values_are_logged(self):
        source = SOURCE.read_text(encoding="utf-8", errors="surrogateescape")
        self.assertIn(
            '"\\n  maxStorageTexturesPerShaderStage: {} (supported: {}, requested: {})"',
            source,
        )
        enabled_start = source.index("wgpu::Limits enabledLimits")
        enabled = source[enabled_start : source.index("  }\n  g_queue", enabled_start)]
        self.assertIn("const wgpu::Status enabledLimitsStatus = g_device.GetLimits", enabled)
        self.assertIn("enabledLimitsStatus == wgpu::Status::Success", enabled)
        self.assertIn("Enabled maxStorageTexturesPerShaderStage", enabled)
        self.assertIn("Failed to query enabled device limits", enabled)


if __name__ == "__main__":
    unittest.main()
