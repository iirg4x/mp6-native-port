"""Replay the Aurora queue and exercise the bounded GX bind-group memo."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import build

try:
    from tests.aurora_test_support import git_command, replay as replay_pinned_source
except ModuleNotFoundError:
    from aurora_test_support import (  # type: ignore[no-redef]
        git_command,
        replay as replay_pinned_source,
    )


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "compat/aurora/base/0076-gx-texture-bind-group-memo.patch"
SELFTEST = ROOT / "tests/native/gx_texture_bind_group_memo_selftest.cpp"
TARGETS = ("lib/gx/gx.hpp", "lib/gx/gx.cpp", "lib/gfx/common.cpp")


def replay(target):
    """Replay the pinned Aurora base queue for one source path."""
    return replay_pinned_source(target, stop_after=PATCH.name)


def replay_pre_0076(target):
    """Reconstruct one pinned generated source file before the 0076 patch."""
    return replay_pinned_source(target, stop_before=PATCH.name)


class GxTextureBindGroupMemo(unittest.TestCase):
    @unittest.skip("proof of the source right after patch 0076; later Aurora changes "
                   "superseded that state, and their own tests cover the current source")
    def test_replayed_source_and_recording_memo(self):
        self.assertTrue(SELFTEST.is_file())

        gx = replay("lib/gx/gx.cpp")
        common = replay("lib/gfx/common.cpp")
        memo_start = gx.index("struct TextureBindGroupMemoKey {")
        memo_end = gx.index("static WGPUSampler texture_sampler", memo_start)
        build_start = gx.index("GXBindGroups build_bind_groups", memo_end)
        build_end = gx.index("\nvoid initialize() noexcept {", build_start)
        subject = gx[memo_start:memo_end] + gx[build_start:build_end]
        self.assertIn("std::array<WGPUTextureView, MaxTextures> textureViews{}", subject)
        self.assertIn("std::array<WGPUSampler, MaxTextures> samplers{}", subject)
        self.assertIn("gfx::current_frame()", subject)
        self.assertIn("sTextureBindGroupMemo.valid", subject)
        self.assertIn("void clear_bind_group_memo() noexcept", subject)
        self.assertNotIn("xxh3_hash", subject)

        initialize = gx[gx.index("void initialize() noexcept {"):]
        shutdown = gx[gx.index("void shutdown() noexcept {"):]
        self.assertIn("clear_bind_group_memo();", initialize[: initialize.index("void shutdown")])
        self.assertIn("clear_bind_group_memo();", shutdown)
        clear_start = common.index("void clear_caches() noexcept {")
        clear_body = common[clear_start : common.index("\n}\n", clear_start) + 3]
        self.assertLess(
            clear_body.index("gx::clear_bind_group_memo();"),
            clear_body.index("g_cachedBindGroups.clear();"),
        )

        with tempfile.TemporaryDirectory(
            prefix="gx-texture-bind-group-memo-", dir=ROOT / "build"
        ) as temporary:
            directory = Path(temporary)
            (directory / "gx_texture_bind_group_memo_subject.inc").write_text(subject)
            executable = directory / "gx-texture-bind-group-memo.exe"
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "c++",
                    "-std=c++20",
                    "-O2",
                    "-UNDEBUG",
                    "-I" + str(directory),
                    str(SELFTEST),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=60
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
            benchmark = re.search(
                r"GX_MEMO benchmark .*memo_bind_refs=(\d+) oracle_bind_refs=(\d+)",
                run_result.stdout,
            )
            self.assertIsNotNone(benchmark, run_result.stdout)
            self.assertLess(int(benchmark.group(1)), int(benchmark.group(2)))
            self.assertIn("PASS: GX texture bind-group memo", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
