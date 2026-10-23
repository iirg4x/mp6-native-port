"""Replay Aurora 0077 and prove direct generated-index writes."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build
from tests.test_frame_resource_lifetimes import function


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "compat/aurora/base/0077-direct-generated-indices.patch"
SELFTEST = ROOT / "tests/native/direct_generated_indices_selftest.cpp"
TARGETS = ("lib/gx/command_processor.cpp", "lib/gfx/common.hpp", "lib/gfx/common.cpp")


def replay(target):
    """Aurora's port-modified source (vendored in third_party/aurora)."""
    return (ROOT / "third_party/aurora" / target).read_text(encoding="utf-8", errors="surrogateescape")


class DirectGeneratedIndices(unittest.TestCase):
    def test_replayed_source_and_native_parity(self):
        self.assertTrue(SELFTEST.is_file())

        cp = replay("lib/gx/command_processor.cpp")
        common = replay("lib/gfx/common.cpp")
        common_hpp = replay("lib/gfx/common.hpp")
        internal = replay("lib/internal.hpp")
        draw_start = cp.index("static void draw_prim")
        draw_end = cp.index("static void handle_draw(", draw_start)
        draw_body = cp[draw_start:draw_end]
        self.assertNotIn("handle_draw_idx_buf", cp)
        self.assertNotIn("handle_draw_unmerged_idxBuf", cp)
        self.assertNotIn("prepare_idx_buffer", cp)
        self.assertIn("gfx::reserve_indices", draw_body)
        self.assertNotIn("gfx::push_indices", draw_body)
        indexed_start = cp.index("} else if (subCmd == GX_AURORA_DRAW_INDEXED)")
        indexed_end = cp.index("} else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH)", indexed_start)
        self.assertIn("gfx::push_indices", cp[indexed_start:indexed_end])
        self.assertIn("IndexReservation reserve_indices(size_t length, size_t alignment)", common)
        self.assertIn("append_uninitialized(length)", common)
        self.assertIn("IndexReservation reserve_indices(size_t length, size_t alignment);", common_hpp)

        parts = {
            "checks": internal[internal.index("#define FATAL(") : internal.index("#define DEFAULT_FATAL(")],
            "buffer": common_hpp[
                common_hpp.index("class ByteBuffer {") : common_hpp.index("} // namespace aurora")
            ],
            "reserve": function(common, "reserve_indices"),
            "indices": "\n".join(
                function(cp, name)
                for name in ("drawable_vertex_count", "index_count", "write_idx_buffer")
            ),
            "draws": "\n".join(
                function(cp, name)
                for name in ("draw_prim", "handle_draw_unmerged", "push_gx_draw")
            ),
        }
        with tempfile.TemporaryDirectory(prefix="direct-generated-indices-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            for name, text in parts.items():
                (directory / (name + ".inc")).write_text(text, encoding="utf-8")
            executable = directory / "direct-generated-indices.exe"
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "c++",
                    "-std=c++20",
                    "-O2",
                    "-UNDEBUG",
                    "-I" + str(directory),
                    "-I" + str(ROOT / "third_party/aurora/include"),
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
        self.assertIn("PASS: direct generated GX indices", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
