"""Exercise the extracted Solo settings-card callback against native-shaped state."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body


ROOT = Path(__file__).resolve().parents[1]


class NativeMdsingSettings(unittest.TestCase):
    def test_settings_callback_uses_named_common_fields_and_updates_all_card_surfaces(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        relative = "src/REL/mdsingdll/mdsing.c"
        source = (decomp / relative).read_text(encoding="utf-8")
        patch = ROOT / "compat/decomp" / (relative + ".patch")
        source = apply_patches.apply_unified_diff(
            source, patch.read_text(encoding="utf-8"), relative
        )
        body = function_body(source, "fn_1_15280")
        self.assertIn("GwCommon.confSingleDiff", body)
        self.assertIn("GwCommon.storyMgPack", body)
        self.assertNotIn("((u8 *) & GwCommon)", body)
        self.assertEqual(body.count("Hu3DAnimAnimSet("), 2)
        self.assertEqual(body.count("difficultyCard->animationId[difficultySurface]"), 1)
        self.assertEqual(body.count("packCard->animationId[packSurface]"), 1)

        fixture = (ROOT / "tests/native/mdsing_settings_selftest.c").read_text(encoding="utf-8")
        fixture = fixture.replace("/* MDSING_NATIVE_SETTINGS_SUBJECT */", body)
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "mdsing_settings_probe.c"
            executable = temporary / "mdsing_settings_probe.exe"
            source_path.write_text(fixture, encoding="utf-8")
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    *build.COMMON_FLAGS,
                    "-UNDEBUG",
                    "-fsanitize=undefined",
                    "-fno-sanitize-recover=all",
                    str(source_path),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
