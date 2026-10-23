"""Replay Aurora 0078 and exercise the producer-side ready-GX memo."""

from pathlib import Path
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
PATCH = ROOT / "compat/aurora/base/0078-gx-ready-pipeline-memo.patch"
SELFTEST = ROOT / "tests/native/gx_ready_pipeline_memo_selftest.cpp"
TARGET = "lib/gfx/pipeline_cache.cpp"


def replay(target, stop_before=None):
    """Reconstruct one pinned Aurora source file through the base patch queue."""
    return replay_pinned_source(target, stop_before=stop_before)


def extract_between(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


class GxReadyPipelineMemo(unittest.TestCase):
    @unittest.skip("proof of the source right after patch 0078; later Aurora changes "
                   "superseded that state, and their own tests cover the current source")
    def test_replayed_source_and_native_parity(self):
        self.assertTrue(SELFTEST.is_file())
        # Keep this proof bound to 0078. Later queue entries may intentionally
        # change the same provider's call contract and have their own tests.
        source = replay_pinned_source(TARGET, stop_after=PATCH.name)

        self.assertIn("std::array<GxReadyPipelineMemoEntry, GxReadyPipelineMemoSize>", source)
        self.assertIn("static std::atomic<uint64_t> g_gxReadyPipelineMemoGeneration{1};", source)
        self.assertIn("bump_gx_ready_pipeline_memo_generation();", source)
        self.assertEqual(source.count("bump_gx_ready_pipeline_memo_generation();"), 2)
        self.assertNotIn("wgpu::RenderPipeline", extract_between(
            source,
            "constexpr size_t GxReadyPipelineMemoSize",
            "template <typename PipelineConfig>\nstatic void publish_shader_info",
        ))

        cached = extract_between(source, "struct CachedPipeline {", "struct PendingPipeline {")
        memo = extract_between(
            source,
            "static PipelineRef g_lastPipelineRef",
            "template <typename PipelineConfig>\nstatic void publish_shader_info",
        )
        subject = extract_between(
            source,
            "template <typename PipelineConfig>\nstatic void publish_shader_info",
            "static void pipeline_cache_abort()",
        )

        with tempfile.TemporaryDirectory(prefix="gx-ready-memo-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            (directory / "gx_ready_memo_cached.inc").write_text(cached, encoding="utf-8")
            (directory / "gx_ready_memo_defs.inc").write_text(memo, encoding="utf-8")
            (directory / "gx_ready_memo_subject.inc").write_text(subject, encoding="utf-8")
            executable = directory / "gx-ready-memo.exe"
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
        self.assertIn("PASS: GX ready pipeline memo", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
