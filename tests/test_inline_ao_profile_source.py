"""Structural and compiled-fake checks for isolated inline-AO profiling transforms."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from frame_profile_source import instrument_ao as instrument_frame_ao
from inline_ao_profile_source import INLINE_SPAN, instrument_ao, instrument_gpu, replace_once
from tools import build


GPU_SOURCE = ROOT / "third_party" / "aurora" / "lib" / "webgpu" / "gpu.cpp"
AO_SOURCE = ROOT / "src" / "gx" / "ambient_occlusion.cpp"


class InlineAoProfileSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not GPU_SOURCE.exists() or not AO_SOURCE.exists():
            raise unittest.SkipTest("pinned Aurora/AO source unavailable")
        cls.gpu = GPU_SOURCE.read_text(encoding="utf-8")
        cls.ao = AO_SOURCE.read_text(encoding="utf-8")
        cls.gpu_transformed = instrument_gpu(cls.gpu)
        # The inline transform is intentionally the second AO transform in the
        # isolated builder; this is the composition used by the benchmark.
        cls.ao_frame = instrument_frame_ao(cls.ao)
        cls.ao_transformed = instrument_ao(cls.ao_frame)

    def test_gpu_source_anchor_and_optional_feature_request(self):
        self.assertEqual(self.gpu.count("FeatureName::TimestampQuery"), 1)
        feature = "FeatureName::ChromiumExperimentalTimestampQueryInsidePasses"
        self.assertEqual(self.gpu_transformed.count(feature), 2)
        self.assertIn(
            "if (feature == wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses)",
            self.gpu_transformed,
        )
        self.assertIn(
            'Log.info("[MP6-INLINE-AO] feature={}",',
            self.gpu_transformed,
        )
        census = self.gpu_transformed.index("g_adapter.GetFeatures(&supportedFeatures);")
        device = self.gpu_transformed.index("g_adapter.RequestDevice(")
        report = self.gpu_transformed.index("[MP6-INLINE-AO] feature=")
        self.assertLess(census, report)
        self.assertLess(report, device)
        self.assertNotIn(
            "requiredFeatures.push_back(wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses)",
            self.gpu_transformed,
        )

    def test_gpu_instance_toggle_preserves_chain_and_precedes_creation(self):
        transformed = self.gpu_transformed
        create = transformed.index("g_instance = wgpu::CreateInstance(&instanceDescriptor);")
        features = transformed.index("g_adapter.GetFeatures(&supportedFeatures);")
        pre_create = transformed[:create]
        toggle_matches = list(re.finditer(
            r"wgpu::DawnTogglesDescriptor\s+(\w+)\s*\("
            r"\s*wgpu::DawnTogglesDescriptor::Init\{(?P<body>.*?)\}\s*\);",
            pre_create,
            re.DOTALL,
        ))
        self.assertTrue(toggle_matches, "missing instance Dawn toggles descriptor")
        toggle = toggle_matches[-1]
        toggle_name = toggle.group(1)
        body = re.sub(r"\s+", " ", toggle.group("body"))
        self.assertIn('.enabledToggles = inlineAoInstanceToggleNames', body)
        self.assertIn('inlineAoInstanceToggleNames[] = {"allow_unsafe_apis"}', pre_create)
        self.assertIn(
            f"{toggle_name}.nextInChain = instanceDescriptor.nextInChain;",
            pre_create[toggle.end():],
        )
        self.assertIn(
            f"instanceDescriptor.nextInChain = &{toggle_name};",
            pre_create[toggle.end():],
        )
        self.assertLess(toggle.start(), create)
        self.assertLess(create, features)

    def test_gpu_transform_has_non_tracy_fail_closed_guard(self):
        transformed = self.gpu_transformed
        guard = transformed.index("#ifdef TRACY_ENABLE")
        error = transformed.index("#error", guard)
        end = transformed.index("#endif", error)
        create = transformed.index("g_instance = wgpu::CreateInstance(&instanceDescriptor);")
        self.assertLess(guard, error)
        self.assertLess(error, end)
        self.assertLess(end, create)

    def test_gpu_and_ao_transforms_fail_closed_and_do_not_mutate_source(self):
        with self.assertRaises(ValueError):
            replace_once("anchor anchor", "anchor", "changed")
        with self.assertRaises(ValueError):
            instrument_gpu(self.gpu.replace("TimestampQuery", "TimestampQueryMissing", 1))
        with self.assertRaises(ValueError):
            instrument_gpu(self.gpu_transformed)
        broken_ao = self.ao_frame.replace(
            "    pass.SetBindGroup(0, bindings);\n    pass.Draw(3);",
            "    pass.SetBindGroup(0, bindings);\n    pass.Draw(4);",
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(broken_ao)
        with self.assertRaises(ValueError):
            instrument_ao(self.ao_transformed)
        self.assertEqual(GPU_SOURCE.read_text(encoding="utf-8"), self.gpu)
        self.assertEqual(AO_SOURCE.read_text(encoding="utf-8"), self.ao)

    def test_draw_order_bindings_and_resource_release_are_unchanged(self):
        transformed = self.ao_transformed
        self.assertEqual(transformed.count("pass.Draw(3);"), self.ao_frame.count("pass.Draw(3);"))
        self.assertEqual(transformed.count("BeginRenderPass"), self.ao_frame.count("BeginRenderPass"))
        self.assertEqual(transformed.count("End();"), self.ao_frame.count("End();"))
        for token in (
            "pass.SetViewport",
            "pass.SetScissorRect",
            "pass.SetPipeline",
            "pass.SetBindGroup",
            "job.composite = nullptr",
            "job.upstream.scratch.reset()",
            "job.available.store(true",
            "job->available.store(true",
        ):
            self.assertEqual(transformed.count(token), self.ao_frame.count(token), token)
        composite = transformed.index('"AO composite inline"')
        composite_draw = transformed.index("pass.Draw(3);", composite)
        composite_end = transformed.index("++g_composited;", composite_draw)
        self.assertLess(composite, composite_draw)
        self.assertLess(composite_draw, composite_end)
        self.assertLess(transformed.index("}", composite_draw), composite_end)
        foliage = transformed.index('"AO foliage seed inline"')
        foliage_draw = transformed.index("pass.Draw(3);", foliage)
        foliage_end = transformed.index("++g_foliageSeeded;", foliage_draw)
        self.assertLess(foliage, foliage_draw)
        self.assertLess(foliage_draw, foliage_end)
        self.assertLess(transformed.index("}", foliage_draw), foliage_end)

    def test_span_helper_is_public_api_only_and_device_gated(self):
        self.assertIn("device.HasFeature(wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses)", INLINE_SPAN)
        self.assertIn("const wgpu::PassTimestampWrites *allocated = gfx::gpu_pass_timestamps(label);", INLINE_SPAN)
        self.assertIn("writes = *allocated;", INLINE_SPAN)
        self.assertIn("pass.WriteTimestamp(writes.querySet, writes.beginningOfPassWriteIndex);", INLINE_SPAN)
        self.assertIn("pass.WriteTimestamp(writes.querySet, writes.endOfPassWriteIndex);", INLINE_SPAN)
        self.assertNotIn("gpu_prof", INLINE_SPAN)
        self.assertIn("scope=nested-efb timestamps=%s", INLINE_SPAN)
        self.assertEqual(self.ao_transformed.count("[MP6-INLINE-AO] scope=nested-efb"), 1)

    def test_compiled_fake_unsupported_and_no_recording_paths(self):
        # Compile the exact helper body against a tiny fake Dawn/public-gfx
        # surface.  This exercises the feature gate, null allocator path, and
        # paired begin/end writes without building the renderer or using a GPU.
        source = r'''#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
namespace wgpu {
enum class FeatureName { ChromiumExperimentalTimestampQueryInsidePasses };
struct QuerySet { int id=0; };
struct PassTimestampWrites {
  QuerySet querySet{};
  uint32_t beginningOfPassWriteIndex=0, endOfPassWriteIndex=0;
};
struct Device {
  bool supported=false;
  bool HasFeature(FeatureName) const { return supported; }
};
struct RenderPassEncoder {
  mutable int writes=0;
  mutable uint32_t first=0, second=0;
  mutable int query=0, endQuery=0;
  void WriteTimestamp(QuerySet querySet, uint32_t index) const {
    if (writes++ == 0) { first=index; query=querySet.id; }
    else { second=index; endQuery=querySet.id; }
  }
};
}
namespace gfx {
const wgpu::PassTimestampWrites *next=nullptr;
int calls=0;
const wgpu::PassTimestampWrites *gpu_pass_timestamps(const char *) noexcept {
  ++calls; return next;
}
}
''' + INLINE_SPAN + r'''
int main() {
  wgpu::Device unsupported{};
  wgpu::RenderPassEncoder noWrites;
  { InlineAoTimestampSpan span(unsupported, noWrites, "unsupported"); }
  assert(noWrites.writes==0 && gfx::calls==0);

  wgpu::Device supported{true};
  wgpu::RenderPassEncoder missing;
  { InlineAoTimestampSpan span(supported, missing, "missing"); }
  assert(missing.writes==0 && gfx::calls==1);

  wgpu::PassTimestampWrites allocation{{7}, 12, 13};
  gfx::next=&allocation;
  wgpu::RenderPassEncoder recorded;
  { InlineAoTimestampSpan span(supported, recorded, "recorded");
    assert(recorded.writes==1 && recorded.first==12 && recorded.query==7); }
  assert(recorded.writes==2 && recorded.second==13 && recorded.endQuery==7);

  // The public helper owns a shared static descriptor.  The span must retain
  // its copied query/index pair even if that descriptor changes before scope
  // exit (for example, when another pass asks for a pair).
  wgpu::RenderPassEncoder copied;
  { InlineAoTimestampSpan span(supported, copied, "copied");
    allocation.querySet.id=99; allocation.endOfPassWriteIndex=42;
  }
  assert(copied.writes==2 && copied.first==12 && copied.second==13 && copied.endQuery==7);
}
'''
        with tempfile.TemporaryDirectory(prefix="inline-ao-span-", dir=ROOT / "build") as directory:
            directory = Path(directory)
            cpp = directory / "span.cpp"
            exe = directory / "span.exe"
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", str(cpp), "-o", str(exe)],
                capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
