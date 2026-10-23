"""Replay M675's native player staging bodies against bounded runtime stubs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body


ROOT = Path(__file__).resolve().parents[1]


class M675StagingNative(unittest.TestCase):
    def test_native_staging_is_baselined_and_waiting_gated(self):
        apply_patches.require_pinned_decomp()
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            patched = directory / "patched"
            apply_patches.apply_all(out_dir=str(patched))
            source = (patched / "src/REL/m675Dll/player.c").read_text(
                encoding="utf-8"
            )
            declarations = "\n".join(
                re.search(r"typedef struct " + name + r"(?:_s)? \{.*?^\} "
                          + name + ";", source, re.M | re.S).group()
                for name in ("M675PlayerConfig", "M675PlayerWork")
            )
            (directory / "m675_staging_types.inc").write_text(
                declarations, encoding="utf-8"
            )
            subject = "\n\n".join(
                function_definition_body(source, name)
                for name in (
                    "mp6_m675_stage_player",
                    "fn_1_5170",
                    "fn_1_55F4",
                )
            )
            (directory / "m675_staging_subject.inc").write_text(
                subject, encoding="utf-8"
            )
            executable = directory / "m675_staging.exe"
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    "-I",
                    str(patched / "include"),
                    *build.COMMON_FLAGS,
                    "-I",
                    str(directory),
                    "-O1",
                    "-UNDEBUG",
                    "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types",
                    "-Werror=int-conversion",
                    "-fsanitize=undefined,float-cast-overflow",
                    "-fno-sanitize-recover=all",
                    str(ROOT / "tests/native/m675_staging_selftest.c"),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(executable)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
