"""Replay Aurora 0082 and verify the immutable PipelineConfig defaults."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build

try:
    from tests.test_gx_pipeline_config_reuse import (
        CP,
        SELFTEST,
        helper_block,
        inject_helper,
        replay,
    )
except ModuleNotFoundError:
    # unittest discover may import files below tests/ as top-level modules.
    from test_gx_pipeline_config_reuse import (  # type: ignore[no-redef]
        CP,
        SELFTEST,
        helper_block,
        inject_helper,
        replay,
    )


ROOT = Path(__file__).resolve().parents[1]

OLD_RESET = """  // Zero trailing stages and optional alpha compare before a fresh rebuild.
  sConfigMemo.value={};"""
NEW_RESET = """  // Restore all defaults, including inactive stages, without rebuilding the
  // large aggregate's identical member initializers on every cache miss.
  static constexpr PipelineConfig defaults{};
  sConfigMemo.value=defaults;"""


class GxPipelineConfigDefaults(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not SELFTEST.is_file():
            raise AssertionError(f"missing fixture: {SELFTEST}")
        cls.after = replay(CP)

    def test_native_actual_helper_default_and_lifecycle_parity(self):
        """Compile the actual helper against the existing fixture."""
        helper = helper_block(self.after)
        template = SELFTEST.read_text(encoding="utf-8")
        generated_source = inject_helper(template, helper)
        with tempfile.TemporaryDirectory(
            prefix="gx-pipeline-config-defaults-", dir=ROOT / "build"
        ) as temporary:
            directory = Path(temporary)
            generated = directory / "gx_pipeline_config_defaults_generated.cpp"
            executable = directory / "gx-pipeline-config-defaults.exe"
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
        self.assertIn("clean_hit_cases=108", run_result.stdout)
        self.assertIn("key_dimension_misses=4 lifecycle_invalidate=1", run_result.stdout)
        self.assertIn("zero_on_miss_clear=1 trailing_stages=7 alpha_clear=1", run_result.stdout)
        self.assertIn("PASS GX pipeline config reuse actual helper", run_result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
