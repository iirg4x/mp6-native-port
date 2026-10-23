"""Structural tests for QA-only AO-owned depth snapshot attribution."""

import sys
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(ROOT))

from ao_depth_snapshot_profile_source import (  # noqa: E402
    instrument_ao_depth_snapshot,
    instrument_tex_copy_conv,
    instrument_tex_copy_conv_header,
)
from frame_profile_source import (  # noqa: E402
    HEADER,
    PROFILE_HEADER_TRANSFORMS,
    TRANSFORMS,
    instrument_ao,
    instrument_common,
)
from tools import build  # noqa: E402


AO_PATH = ROOT / "src" / "gx" / "ambient_occlusion.cpp"
COMMON_PATH = ROOT / "third_party" / "aurora" / "lib" / "gfx" / "common.cpp"
TEX_PATH = ROOT / "third_party" / "aurora" / "lib" / "gfx" / "tex_copy_conv.cpp"
TEX_HEADER_PATH = ROOT / "third_party" / "aurora" / "lib" / "gfx" / "tex_copy_conv.hpp"


class AoDepthSnapshotProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ao = AO_PATH.read_text(encoding="utf-8")
        cls.common = COMMON_PATH.read_text(encoding="utf-8")
        cls.tex = TEX_PATH.read_text(encoding="utf-8")
        cls.tex_header = TEX_HEADER_PATH.read_text(encoding="utf-8")

    def test_two_ao_call_sites_are_scoped_and_direct_readonly_is_not(self):
        transformed = instrument_ao(self.ao)
        self.assertEqual(transformed.count("AoDepthSnapshotScope"), 2)
        self.assertEqual(transformed.count("profileAoDepthResolved = gfx::resolve_pass"), 2)

        # Both scopes end before the caller examines the resolved target.  The
        # direct readonly-EFB path remains outside the attribution mechanism.
        self.assertEqual(transformed.count("profileAoDepthScope;\n            profileAoDepthResolved"), 1)
        self.assertEqual(transformed.count("profileAoDepthScope;\n        profileAoDepthResolved"), 1)
        self.assertIn("create_pass_with_readonly_efb_depth(width,height)", transformed)
        readonly_end = transformed.index("create_pass_with_readonly_efb_depth(width,height)")
        self.assertLess(readonly_end, transformed.index("AoDepthSnapshotScope"))

    def test_ownership_is_recorded_only_for_an_actual_depth_copy(self):
        transformed = instrument_common(self.common)
        self.assertIn("bool profileAoDepthSnapshot = false;", transformed)
        self.assertIn(
            "const bool profileAoDepthSnapshot = copyDepth &&\n      mp6_frame_profile_ao_depth_requested();",
            transformed,
        )

        reuse_start = transformed.index("const bool reuseDepth")
        copy_start = transformed.index("const bool copyDepth", reuse_start)
        self.assertNotIn("profileAoDepthSnapshot =", transformed[reuse_start:copy_start])

        allocation_start = transformed.index("if (copyDepth) {", copy_start)
        allocation_end = transformed.index("    }\n  }\n  out.width", allocation_start)
        allocation = transformed[allocation_start:allocation_end]
        self.assertIn("prevPass.snapshotDepthDst = entry.depth.view;", allocation)
        self.assertIn(
            "prevPass.profileAoDepthSnapshot = profileAoDepthSnapshot && bool(prevPass.snapshotDepthDst);",
            allocation,
        )

    def test_reuse_and_unrelated_snapshots_keep_generic_path(self):
        common = instrument_common(self.common)
        tex = instrument_tex_copy_conv(self.tex)

        render_call = common[common.index("if (passInfo.snapshotDepthDst)"):]
        self.assertIn("snapshot_depth_profiled", render_call)
        self.assertIn('"AO depth snapshot"', render_call)
        self.assertIn("snapshot_depth(cmd, passInfo.copySourceDepthView", render_call)
        self.assertEqual(tex.count("snapshot_depth_profiled"), 2)
        self.assertEqual(tex.count('pass_writes(profileLabel)'), 1)
        self.assertEqual(tex.count('pass_writes("Depth snapshot")'), 0)
        self.assertIn('snapshot_depth_profiled(cmd, srcDepth, msaaSamples, dst, "Depth snapshot");', tex)

    def test_source_drift_fails_closed(self):
        foliage_anchor = "if (!gfx::resolve_pass({.color=false,.depth=true},targets)"
        broken_ao = self.ao.replace(foliage_anchor, "if (!gfx::resolve_pass({.color=false,.depth=false},targets)", 1)
        with self.assertRaises(ValueError):
            instrument_ao(broken_ao)

        broken_tex = self.tex.replace("void snapshot_depth(", "void snapshot_depth_drifted(", 1)
        with self.assertRaises(ValueError):
            instrument_tex_copy_conv(broken_tex)

        broken_header = self.tex_header.replace("void snapshot_depth(", "void snapshot_depth_drifted(", 1)
        with self.assertRaises(ValueError):
            instrument_tex_copy_conv_header(broken_header)

        render_anchor = "tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView"
        broken_common = self.common.replace(render_anchor, "tex_copy_conv::snapshot_depth_drifted(cmd, passInfo.copySourceDepthView", 1)
        with self.assertRaises(ValueError):
            instrument_common(broken_common)

        with self.assertRaises(ValueError):
            instrument_ao_depth_snapshot(instrument_ao_depth_snapshot(self.ao))

    def test_profile_only_output_wiring(self):
        self.assertIn("lib/gfx/tex_copy_conv.cpp", TRANSFORMS)
        self.assertIn("lib/gfx/tex_copy_conv.hpp", PROFILE_HEADER_TRANSFORMS)
        self.assertIn("mp6_frame_profile", instrument_tex_copy_conv(self.tex))
        self.assertIn("snapshot_depth_profiled", instrument_tex_copy_conv_header(self.tex_header))
        self.assertEqual(self.common, COMMON_PATH.read_text(encoding="utf-8"))
        self.assertEqual(self.tex, TEX_PATH.read_text(encoding="utf-8"))
        self.assertIn("mp6_frame_profile_ao_depth_requested", HEADER)
        self.assertIn("AoDepthSnapshotScope", HEADER)
        common = instrument_common(self.common)
        self.assertEqual(common.count('extern "C" void mp6_frame_profile_ao_depth_begin() noexcept'), 1)
        self.assertEqual(common.count('extern "C" void mp6_frame_profile_ao_depth_end() noexcept'), 1)
        self.assertEqual(common.count('extern "C" bool mp6_frame_profile_ao_depth_requested() noexcept'), 1)

    def test_request_scope_bridges_translation_units_and_thread_local_state(self):
        """The generated header's C bridge must nest and restore per thread."""
        with tempfile.TemporaryDirectory(prefix="ao-depth-scope-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            (folder / "mp6_frame_profile.hpp").write_text(HEADER, encoding="utf-8")
            bridge = folder / "bridge.cpp"
            bridge.write_text(
                r'''
#include "mp6_frame_profile.hpp"
namespace { thread_local unsigned depth = 0; }
extern "C" void mp6_frame_profile_ao_depth_begin() noexcept { ++depth; }
extern "C" void mp6_frame_profile_ao_depth_end() noexcept { if (depth) --depth; }
extern "C" bool mp6_frame_profile_ao_depth_requested() noexcept { return depth != 0; }
extern "C" unsigned ao_depth_for_test() noexcept { return depth; }
''',
                encoding="utf-8",
            )
            consumer = folder / "consumer.cpp"
            consumer.write_text(
                r'''
#include "mp6_frame_profile.hpp"
#include <atomic>
#include <thread>
extern "C" unsigned ao_depth_for_test() noexcept;

static int one_thread() {
  if (ao_depth_for_test() != 0) return 1;
  {
    mp6_frame_profile::AoDepthSnapshotScope outer;
    if (ao_depth_for_test() != 1) return 2;
    { mp6_frame_profile::AoDepthSnapshotScope inner;
      if (ao_depth_for_test() != 2) return 3;
    }
    if (ao_depth_for_test() != 1) return 4;
  }
  return ao_depth_for_test() == 0 ? 0 : 5;
}

int main() {
  if (one_thread() != 0) return 10 + one_thread();
  std::atomic<int> failures{0};
  auto worker = [&] {
    if (ao_depth_for_test() != 0) { ++failures; return; }
    { mp6_frame_profile::AoDepthSnapshotScope scope;
      if (ao_depth_for_test() != 1) ++failures;
    }
    if (ao_depth_for_test() != 0) ++failures;
  };
  std::thread first(worker), second(worker);
  first.join(); second.join();
  return failures.load() == 0 ? 0 : 20 + failures.load();
}
''',
                encoding="utf-8",
            )
            executable = folder / "scope-test.exe"
            subprocess.run(
                [str(build.ZIG), "c++", "-std=c++20", "-O2", str(bridge), str(consumer), "-o", str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
