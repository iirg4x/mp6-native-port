"""Exercise battle and bingo preview bodies against bounded native stubs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = (
    "fn_1_898",
    "fn_1_854",
    "fn_1_0",
    "fn_1_2A4",
    "fn_1_43C",
    "fn_1_4F8",
    "fn_1_6F4",
)
GLOBALS = (
    "lbl_1_bss_A4",
    "lbl_1_bss_A2",
    "lbl_1_bss_A0",
    "lbl_1_bss_9C",
    "lbl_1_bss_98",
    "lbl_1_bss_94",
    "lbl_1_bss_90",
    "lbl_1_bss_8C",
    "lbl_1_bss_88",
    "lbl_1_bss_84",
    "lbl_1_bss_2",
    "lbl_1_bss_0",
)


def patched(decomp, relative):
    source = (decomp / relative).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (relative + ".patch")
    return apply_patches.apply_unified_diff(source, patch.read_text(encoding="utf-8"), relative)


def namespaced(source, prefix):
    for name in FUNCTIONS + GLOBALS:
        source = re.sub(r"\b" + re.escape(name) + r"\b", prefix + "_" + name, source)
    return source


class NativeMinigamePreviews(unittest.TestCase):
    def test_battle_and_bingo_preview_bodies_guard_state_and_keep_native_movie_size(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        modules = (
            ("battle", "src/REL/mgmbattledll/preview.c"),
            ("bingo", "src/REL/mgmbingodll/text_00000.c"),
        )
        bodies = []
        for prefix, relative in modules:
            source = patched(decomp, relative)
            self.assertIn('#include "mp6_widescreen.h"', source)
            self.assertEqual(source.count("movieSprite != HUSPR_NONE"), 2)
            self.assertIn("if (HuTHPStartCheck() != 0)", function_body(source, "fn_1_854"))
            self.assertIn("(mp6_widescreen_render_width() - 640) / 2", function_body(source, "fn_1_854"))
            self.assertEqual(function_body(source, "fn_1_898").count("GXEnd();"), 2)
            self.assertIn("if (lbl_1_bss_0 <= 0 || lbl_1_bss_8C == NULL) return;",
                          function_body(source, "fn_1_43C"))
            subject = "\n\n".join(function_body(source, name) for name in FUNCTIONS)
            bodies.append(namespaced(subject, prefix))

        fixture = (ROOT / "tests/native/mg_preview_selftest.c").read_text(encoding="utf-8")
        fixture = fixture.replace("/* MG_PREVIEW_NATIVE_SUBJECT */", "\n\n".join(bodies))
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source = temporary / "mg_preview_probe.c"
            executable = temporary / "mg_preview_probe.exe"
            source.write_text(fixture, encoding="utf-8")
            result = subprocess.run([
                build.ZIG, "cc", *build.COMMON_FLAGS, "-UNDEBUG", "-fsanitize=undefined",
                "-fno-sanitize-recover=all", str(source), "-o", str(executable),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
