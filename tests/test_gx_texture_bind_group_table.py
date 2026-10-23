"""Replay the complete Aurora queue and exercise the 0079 GX bind-group table."""

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
PATCH = ROOT / "compat/aurora/base/0079-gx-texture-bind-group-table.patch"
SELFTEST = ROOT / "tests/native/gx_texture_bind_group_table_selftest.cpp"
TARGET = "lib/gx/gx.cpp"


def replay(target, stop_before=None):
    return replay_pinned_source(target, stop_before=stop_before)


class GxTextureBindGroupTable(unittest.TestCase):
    def test_replayed_source_and_table_parity(self):
        self.assertTrue(SELFTEST.is_file())
        gx = replay(TARGET)

        memo_start = gx.index("struct TextureBindGroupMemoKey {")
        memo_end = gx.index("static WGPUSampler texture_sampler", memo_start)
        build_start = gx.index("GXBindGroups build_bind_groups", memo_end)
        build_end = gx.index("\nvoid initialize() noexcept {", build_start)
        subject = gx[memo_start:memo_end] + gx[build_start:build_end]
        self.assertIn("TextureBindGroupMemoTableSize = 64", subject)
        self.assertIn("sTextureBindGroupMemoTable", subject)
        self.assertIn("std::has_unique_object_representations_v<TextureBindGroupMemoKey>", subject)
        self.assertIn("texture_bind_group_memo_slot(key)", subject)
        self.assertIn("refresh_texture_bind_group_memo_frame(frame)", subject)
        self.assertIn("tableEntry.key == key", subject)
        self.assertNotIn("wgpu::", subject[subject.index("struct TextureBindGroupMemoTableEntry") :
                        subject.index("static_assert", subject.index("struct TextureBindGroupMemoTableEntry"))])

        with tempfile.TemporaryDirectory(prefix="gx-texture-bind-group-table-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            (directory / "gx_texture_bind_group_table_subject.inc").write_text(subject, encoding="utf-8")
            executable = directory / "gx-texture-bind-group-table.exe"
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
        self.assertIn("PASS: GX texture bind-group table", run_result.stdout)


if __name__ == "__main__":
    unittest.main()
