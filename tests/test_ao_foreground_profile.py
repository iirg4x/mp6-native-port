"""Structural and compiled tests for AO foreground-pass attribution."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from frame_profile_source import HEADER, instrument_ao, instrument_common  # noqa: E402
from tools import build  # noqa: E402


AO_PATH = ROOT / "src" / "gx" / "ambient_occlusion.cpp"
COMMON_PATH = ROOT / "third_party" / "aurora" / "lib" / "gfx" / "common.cpp"


class AoForegroundProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ao = AO_PATH.read_text(encoding="utf-8")
        cls.common = COMMON_PATH.read_text(encoding="utf-8")

    def test_ao_target_creation_scopes_direct_and_fallback_paths(self):
        transformed = instrument_ao(self.ao)
        self.assertEqual(transformed.count("AoForegroundPassScope"), 2)

        direct_scope = transformed.index("AoForegroundPassScope")
        direct_call = transformed.index("create_pass_with_readonly_efb_depth", direct_scope)
        direct_end = transformed.index("FoliageSeedJob *job=nullptr;", direct_call)
        self.assertLess(direct_scope, direct_call)
        self.assertLess(direct_call, direct_end)

        fallback_scope = transformed.index("AoForegroundPassScope", direct_end)
        fallback_call = transformed.index("create_pass_from_efb_depth", fallback_scope)
        fallback_create = transformed.index("create_pass(targets.width,targets.height)", fallback_call)
        fallback_end = transformed.index("g_foliageSeedType==gfx::InvalidDrawType", fallback_create)
        self.assertLess(fallback_scope, fallback_call)
        self.assertLess(fallback_call, fallback_create)
        self.assertLess(fallback_create, fallback_end)

        # Scoping is attribution-only: the two target creation branches and
        # the inline seed callback's production commands remain unchanged.
        for token in (
            "create_pass_with_readonly_efb_depth(width,height)",
            "create_pass_from_efb_depth(source)",
            "pass.Draw(3);",
            "gfx::push_custom_draw(g_foliageSeedType,&job,sizeof(job))",
        ):
            self.assertEqual(transformed.count(token), self.ao.count(token), token)
        self.assertEqual(AO_PATH.read_text(encoding="utf-8"), self.ao)

    def test_common_captures_ownership_and_keeps_generic_offscreen(self):
        transformed = instrument_common(self.common)
        self.assertEqual(transformed.count("mp6_frame_profile_ao_foreground_requested"), 2)
        self.assertEqual(transformed.count("profileAoForegroundPass"), 5)
        self.assertEqual(transformed.count('"AO foreground mask"'), 1)
        self.assertIn(
            '.label = profileAoForegroundPass ? "AO foreground mask" : pass_label("Offscreen"),',
            transformed,
        )
        self.assertIn(
            "newPass.profileAoForegroundPass = profileAoForegroundPass;",
            transformed,
        )

        # The request is consumed while constructing the pass. Async render
        # only reads the stored RenderPass label; it cannot relabel unrelated
        # passes after the scope has ended.
        create = transformed.index("const bool profileAoForegroundPass")
        render = transformed.index(
            "static void render(wgpu::CommandEncoder& cmd, FramePacket& frame, "
            "RenderPass& passInfo, uint32_t passIndex) {"
        )
        self.assertLess(create, render)
        self.assertNotIn("mp6_frame_profile_ao_foreground_requested", transformed[render:])
        self.assertIn(
            '.timestampWrites = webgpu::gpu_prof::pass_writes(passInfo.label.empty() ? "Scene" : passInfo.label),',
            transformed[render:],
        )
        self.assertEqual(transformed.count('pass_label("Offscreen")'), 1)
        self.assertIn('"AO depth snapshot"', transformed)

    def test_source_drift_fails_closed(self):
        broken_ao = self.ao.replace(
            "gfx::create_pass_with_readonly_efb_depth(width,height)",
            "gfx::create_pass_with_readonly_efb_depth_drifted(width,height)",
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(broken_ao)

        broken_fallback = self.ao.replace(
            "gfx::create_pass_from_efb_depth(source)",
            "gfx::create_pass_from_efb_depth_drifted(source)",
            1,
        )
        with self.assertRaises(ValueError):
            instrument_ao(broken_fallback)

        broken_common = self.common.replace(
            '      .label = pass_label("Offscreen"),',
            '      .label = pass_label("Offscreen drifted"),',
            1,
        )
        with self.assertRaises(ValueError):
            instrument_common(broken_common)

    def test_compiled_scopes_nest_and_are_thread_local(self):
        with tempfile.TemporaryDirectory(prefix="ao-foreground-scope-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            (folder / "mp6_frame_profile.hpp").write_text(HEADER, encoding="utf-8")
            bridge = folder / "bridge.cpp"
            bridge.write_text(
                r'''
#include "mp6_frame_profile.hpp"
namespace {
thread_local unsigned depth = 0;
thread_local unsigned foreground = 0;
}
extern "C" void mp6_frame_profile_ao_depth_begin() noexcept { ++depth; }
extern "C" void mp6_frame_profile_ao_depth_end() noexcept { if (depth) --depth; }
extern "C" bool mp6_frame_profile_ao_depth_requested() noexcept { return depth != 0; }
extern "C" void mp6_frame_profile_ao_foreground_begin() noexcept { ++foreground; }
extern "C" void mp6_frame_profile_ao_foreground_end() noexcept { if (foreground) --foreground; }
extern "C" bool mp6_frame_profile_ao_foreground_requested() noexcept { return foreground != 0; }
extern "C" unsigned ao_depth_for_test() noexcept { return depth; }
extern "C" unsigned ao_foreground_for_test() noexcept { return foreground; }
''',
                encoding="utf-8",
            )
            consumer = folder / "consumer.cpp"
            consumer.write_text(
                r'''
#include "mp6_frame_profile.hpp"
#include <atomic>
#include <cassert>
#include <thread>
extern "C" unsigned ao_depth_for_test() noexcept;
extern "C" unsigned ao_foreground_for_test() noexcept;

static int one_thread() {
  if (ao_depth_for_test() != 0 || ao_foreground_for_test() != 0) return 1;
  {
    mp6_frame_profile::AoDepthSnapshotScope depth;
    if (ao_depth_for_test() != 1 || ao_foreground_for_test() != 0) return 2;
    {
      mp6_frame_profile::AoForegroundPassScope outer;
      if (ao_depth_for_test() != 1 || ao_foreground_for_test() != 1) return 3;
      {
        mp6_frame_profile::AoForegroundPassScope inner;
        if (ao_depth_for_test() != 1 || ao_foreground_for_test() != 2) return 4;
      }
      if (ao_foreground_for_test() != 1) return 5;
    }
    if (ao_depth_for_test() != 1 || ao_foreground_for_test() != 0) return 6;
  }
  return ao_depth_for_test() == 0 && ao_foreground_for_test() == 0 ? 0 : 7;
}

int main() {
  if (one_thread() != 0) return 10 + one_thread();
  std::atomic<int> failures{0};
  auto worker = [&] {
    if (ao_depth_for_test() != 0 || ao_foreground_for_test() != 0) { ++failures; return; }
    {
      mp6_frame_profile::AoForegroundPassScope scope;
      if (ao_depth_for_test() != 0 || ao_foreground_for_test() != 1) ++failures;
    }
    if (ao_depth_for_test() != 0 || ao_foreground_for_test() != 0) ++failures;
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
