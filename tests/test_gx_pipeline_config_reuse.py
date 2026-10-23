"""Replay Aurora 0081 and prove the GX PipelineConfig memo contract."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build

try:
    from tests.aurora_test_support import replay as replay_pinned_source
except ModuleNotFoundError:
    from aurora_test_support import replay as replay_pinned_source  # type: ignore[no-redef]


ROOT = Path(__file__).resolve().parents[1]
SELFTEST = ROOT / "tests/native/gx_pipeline_config_reuse_selftest.cpp"

CP = "lib/gx/command_processor.cpp"
CP_HEADER = "lib/gx/command_processor.hpp"
GX = "lib/gx/gx.cpp"
TEST_HEADER = "tests/gx_test_common.hpp"
TARGETS = {CP, CP_HEADER, GX, TEST_HEADER}


def replay(
    target: str,
    stop_before: str | None = None,
    stop_after: str | None = None,
) -> str:
    """Replay the applicable base queue, optionally with an inclusive bound.

    ``stop_before`` preserves the pre-patch snapshot used by the 0081
    git-apply check.  ``stop_after`` is inclusive so a test can verify one
    patch without being invalidated by later queue entries.
    With neither bound, replay still follows the complete current queue.
    """
    return replay_pinned_source(
        target, stop_before=stop_before, stop_after=stop_after
    )


def balanced_block(source: str, marker: str) -> str:
    """Extract a source block while tolerating braces in strings/comments."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    state: str | None = None
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line":
            if char == "\n":
                state = None
            index += 1
            continue
        if state == "block":
            if char == "*" and next_char == "/":
                state = None
                index += 2
                continue
            index += 1
            continue
        if state in ('"', "'"):
            if char == "\\":
                index += 2
                continue
            if char == state:
                state = None
            index += 1
            continue
        if char == "/" and next_char == "/":
            state = "line"
            index += 2
            continue
        if char == "/" and next_char == "*":
            state = "block"
            index += 2
            continue
        if char in ('"', "'"):
            state = char
            index += 1
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise AssertionError(f"unterminated source block: {marker}")


def helper_block(source: str) -> str:
    """Extract declarations through the actual get_pipeline_config body."""
    start_marker = "static bool g_pipelineConfigDirty = true;"
    function_marker = "static const PipelineConfig& get_pipeline_config(GXPrimitive prim, GXVtxFmt fmt)"
    start = source.index(start_marker)
    function = balanced_block(source, function_marker)
    function_start = source.index(function_marker, start)
    function_end = function_start + len(function)
    return source[start:function_end]


def inject_helper(template: str, helper: str) -> str:
    begin_marker = "// BEGIN ACTUAL_PIPELINE_CONFIG_HELPER"
    end_marker = "// END ACTUAL_PIPELINE_CONFIG_HELPER"
    begin = template.index(begin_marker)
    content_start = template.index("\n", begin) + 1
    end = template.index(end_marker, content_start)
    return template[:content_start] + helper + "\n" + template[end:]


class GxPipelineConfigReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not SELFTEST.is_file():
            raise AssertionError(f"missing fixture: {SELFTEST}")
        cls.after = {target: replay(target) for target in TARGETS}

    def test_replays_all_prior_patches_to_0081_candidate(self):
        self.assertEqual(set(self.after), TARGETS)
        self.assertIn("static bool g_pipelineConfigDirty = true;", self.after[CP])
        self.assertIn("void invalidate_pipeline_config() noexcept;", self.after[CP_HEADER])

    def test_preserves_dirty_writes_and_key_lifecycle(self):
        dirty = "g_gxState.stateDirty = true;"
        for anchor in (
            "frame==sConfigMemo.frame",
            "samples==sConfigMemo.samples",
            "prim==sConfigMemo.prim",
            "fmt==sConfigMemo.fmt",
            "if (hit) return sConfigMemo.value;",
            "sConfigMemo.value=defaults;",
            "populate_pipeline_config(sConfigMemo.value,prim,fmt);",
        ):
            self.assertIn(anchor, self.after[CP], anchor)
        self.assertEqual(self.after[GX].count("fifo::invalidate_pipeline_config();"), 2)
        self.assertEqual(self.after[TEST_HEADER].count("fifo::invalidate_pipeline_config();"), 2)

    def test_native_actual_helper_parity(self):
        """Compile the helper extracted from replayed source, never a model body."""
        helper = helper_block(self.after[CP])
        template = SELFTEST.read_text(encoding="utf-8")
        generated_source = inject_helper(template, helper)
        with tempfile.TemporaryDirectory(prefix="gx-pipeline-config-reuse-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            generated = directory / "gx_pipeline_config_reuse_generated.cpp"
            executable = directory / "gx-pipeline-config-reuse.exe"
            generated.write_text(generated_source, encoding="utf-8", newline="\n")
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "c++",
                    "-std=c++20",
                    "-O2",
                    "-UNDEBUG",
                    str(generated),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=60
            )
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
        self.assertIn("PASS GX pipeline config reuse actual helper", run_result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
