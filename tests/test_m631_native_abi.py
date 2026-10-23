"""Execute Conveyor Bolt's adapted native pointer/list and setup ABI."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body


ROOT = Path(__file__).resolve().parents[1]
MODULE = "m631Dll"


def patched(unit):
    """Return one pinned m631 source after applying its checked-in patch."""
    decomp = Path(apply_patches.require_pinned_decomp())
    relative = f"src/REL/{MODULE}/{unit}.c"
    source = (decomp / relative).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (relative + ".patch")
    if patch.exists():
        source = apply_patches.apply_unified_diff(source, patch.read_text(encoding="utf-8"), relative)
    return apply_patches.with_rel_state(source, relative)


def reset_tail(source):
    marker = '#include "mp6_rel_state.h"'
    position = source.find(marker)
    if position < 0:
        raise AssertionError("missing m631 reset owner")
    return source[position:]


class M631NativeABI(unittest.TestCase):
    def test_extracted_native_probe(self):
        utilities = patched("utilities")
        actor = patched("actor")
        game = patched("game")

        subject = "\n\n".join(
            (
                function_definition_body(utilities, "fn_1_AB4C"),
                function_definition_body(utilities, "fn_1_AB60"),
                function_definition_body(utilities, "fn_1_AF2C"),
                function_definition_body(utilities, "fn_1_AF94"),
                function_definition_body(utilities, "fn_1_AFF8"),
                function_definition_body(utilities, "fn_1_B004"),
                function_definition_body(utilities, "fn_1_B014"),
                function_definition_body(utilities, "fn_1_B0B8"),
                function_definition_body(utilities, "fn_1_B120"),
                function_definition_body(utilities, "fn_1_B1AC"),
                function_definition_body(actor, "fn_1_A4FC"),
                function_definition_body(game, "fn_1_A0"),
                reset_tail(actor),
                reset_tail(game),
                reset_tail(utilities),
            )
        )

        fixture = (ROOT / "tests/native/m631_native_selftest.c").read_text(
            encoding="utf-8"
        )
        source = fixture.replace("/* M631_NATIVE_SUBJECT */", subject)

        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "m631_native_selftest.c"
            executable = temporary / "m631_native_selftest.exe"
            source_path.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    *build.COMMON_FLAGS,
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(Path(apply_patches.require_pinned_decomp()) / "include"),
                    "-UNDEBUG",
                    "-fsanitize=undefined",
                    "-fno-sanitize-recover=all",
                    "-Werror=incompatible-function-pointer-types",
                    "-Werror=int-conversion",
                    "-Werror=implicit-function-declaration",
                    str(source_path),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"m631 native probe compile failed:\n{result.stdout}\n{result.stderr}",
            )
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"m631 native probe failed:\n{result.stdout}\n{result.stderr}",
            )
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
