"""Replay Aurora 0080 and prove FIFO/BP fast-path parity."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build

try:
    from tests.aurora_test_support import git_command, replay as replay_pinned_source
except ModuleNotFoundError:
    from aurora_test_support import (  # type: ignore[no-redef]
        git_command,
        replay as replay_pinned_source,
    )


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests/native/fifo_dispatch_reference.cpp"
SELFTEST = ROOT / "tests/native/fifo_dispatch_fastpaths_selftest.cpp"
TARGET = "lib/gx/command_processor.cpp"


def replay() -> str:
    """The current command processor (third_party/aurora)."""
    return replay_pinned_source(TARGET)


def function(source: str, marker: str) -> str:
    """Extract one balanced-brace function from a replayed production file."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {marker}")


def declaration(source: str, marker: str) -> str:
    """Extract a balanced struct declaration including its semicolon."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                end = source.index(";", index) + 1
                return source[start:end]
    raise AssertionError(f"unterminated declaration: {marker}")


def write_subjects(directory: Path, before: str, after: str) -> None:
    helper = function(after, "static u32 skip_nop_padding(")
    original_process = function(before, "void process(const u8* data, u32 size, bool bigEndian)")
    candidate_process = function(after, "void process(const u8* data, u32 size, bool bigEndian)")
    original_process = original_process.replace("void process(", "void process_original(", 1)
    candidate_process = candidate_process.replace("void process(", "void process_candidate(", 1)
    (directory / "dispatch.inc").write_text(
        helper + "\n" + original_process + "\n\n" + candidate_process + "\n",
        encoding="utf-8",
    )

    mapping = declaration(before, "struct TexBpRegMapping {")
    original_decode = function(before, "static std::optional<TexBpRegMapping> decode_tex_bp_reg(")
    candidate_decode = function(after, "static std::optional<TexBpRegMapping> decode_tex_bp_reg(")
    original_decode = original_decode.replace("decode_tex_bp_reg", "decode_original", 1)
    candidate_decode = candidate_decode.replace("decode_tex_bp_reg", "decode_candidate", 1)
    (directory / "bp.inc").write_text(
        mapping + "\n\n" + original_decode + "\n\n" + candidate_decode + "\n",
        encoding="utf-8",
    )


class FifoDispatchFastpaths(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not SELFTEST.is_file():
            raise AssertionError(f"missing fixture: {SELFTEST}")
        # The dispatch and BP decode exactly as they were before the fast
        # paths, frozen when Aurora was vendored; the current source must
        # behave identically.
        cls.before = REFERENCE.read_text(encoding="utf-8")
        cls.after = replay()

    def test_replayed_candidate_contract(self):
        self.assertIn("case CP_CMD_NOP:\n      continue;", self.before)
        self.assertIn("case CP_CMD_NOP:\n      pos = skip_nop_padding(data, pos, size);", self.after)
        self.assertIn("const u32 bank = (local >> 5) & 1u;", self.after)
        self.assertIn("regId >= 0xA0u && regId <= 0xBBu", self.after)
        self.assertNotIn("constexpr std::array mode0Ids", function(
            self.after, "static std::optional<TexBpRegMapping> decode_tex_bp_reg("
        ))


    def test_actual_process_and_bp_mapping_parity(self):
        with tempfile.TemporaryDirectory(
            prefix="fifo-dispatch-fastpaths-", dir=ROOT / "build"
        ) as temporary:
            directory = Path(temporary)
            write_subjects(directory, self.before, self.after)
            executable = directory / "fifo-dispatch-fastpaths.exe"
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
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
        self.assertIn("PASS FIFO process cases=21844 BP mapping cases=65558 valid=56",
                      run_result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
