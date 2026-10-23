"""The pipeline dirty-write helper sets both dirty flags."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.aurora_test_support import replay
from tests.test_gx_pipeline_config_reuse import balanced_block
from tools import build


ROOT = Path(__file__).resolve().parents[1]
TARGET = "lib/gx/command_processor.cpp"


class PipelineDirtyHelper(unittest.TestCase):
    def test_native_actual_helper_sets_both_flags(self):
        source = replay(TARGET)
        helper = balanced_block(source, "static inline void mark_pipeline_state_dirty() noexcept")
        fixture = """#include <cassert>
static bool g_pipelineConfigDirty;
static struct { bool stateDirty; } g_gxState;
""" + helper + """
int main() {
  for (int config = 0; config != 2; ++config) {
    for (int state = 0; state != 2; ++state) {
      g_pipelineConfigDirty = config;
      g_gxState.stateDirty = state;
      mark_pipeline_state_dirty();
      assert(g_pipelineConfigDirty && g_gxState.stateDirty);
    }
  }
}
"""
        with tempfile.TemporaryDirectory(prefix="dirty-helper-", dir=ROOT / "build") as tmp:
            folder = Path(tmp)
            src, exe = folder / "test.cpp", folder / "test.exe"
            src.write_text(fixture)
            compiled = subprocess.run([build.ZIG, "c++", "-O2", "-UNDEBUG", str(src), "-o", str(exe)],
                                      capture_output=True, text=True, timeout=90)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
