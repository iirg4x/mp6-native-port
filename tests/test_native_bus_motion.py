"""Regression coverage for MGMfree's slowed bus-stop motion timers.

The fixture embeds the recovered ``fn_1_1C74`` body after the current decomp
patches are replayed.  That keeps this test tied to the real state layout and
the actual layer-5/layer-6 reset code instead of reimplementing the fix in
Python.  A source-level negative control changes only the two new timer
decrements back to one wall-clock tick; it must fail at the first widescreen
case because the authored 1079-frame motion is still live.
"""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body


ROOT = Path(__file__).resolve().parents[1]
MODULE = "src/REL/mgmfreedll/menu.c"


def patched_menu() -> str:
    """Replay the checked-in MGMfree patch queue against the pinned source."""

    decomp = Path(apply_patches.require_pinned_decomp())
    source = (decomp / MODULE).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (MODULE + ".patch")
    return apply_patches.apply_unified_diff(
        source, patch.read_text(encoding="utf-8"), MODULE
    )


def fixture_source(body: str, state_typedef: str) -> str:
    template = (ROOT / "tests/native/bus_motion_selftest.c").read_text(
        encoding="utf-8"
    )
    template = template.replace("/* MGMFREE_MODEL_STATE_TYPE */", state_typedef, 1)
    template = template.replace("/* BUS_MOTION_SUBJECT */", body, 1)
    return template


def compile_fixture(source: str, directory: Path, name: str) -> Path:
    source_path = directory / (name + ".c")
    executable = directory / (name + ".exe")
    source_path.write_text(source, encoding="utf-8")
    result = subprocess.run(
        [
            build.ZIG,
            "cc",
            *build.COMMON_FLAGS,
            "-I",
            str(ROOT / "tests/native"),
            "-UNDEBUG",
            "-O1",
            "-fsanitize=undefined",
            "-fno-sanitize-recover=all",
            str(source_path),
            "-lm",
            "-o",
            str(executable),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{name} fixture failed to compile:\n{result.stdout}\n{result.stderr}"
        )
    return executable


class NativeBusMotion(unittest.TestCase):
    def test_scaled_bus_motion_timer_and_negative_wall_clock_control(self):
        menu = patched_menu()
        body = function_definition_body(menu, "fn_1_1C74")
        state = re.search(
            r"typedef struct MgmfreeModelState\s*\{.*?^\}\s*MGMFREE_MODEL_STATE;",
            menu,
            re.M | re.S,
        )
        self.assertIsNotNone(state, "missing recovered MGMFREE_MODEL_STATE typedef")
        self.assertEqual(
            body.count("record->value.countdown -= travelStep"),
            2,
            "layer 5 and layer 6 must both use motion-time decrement",
        )

        positive = fixture_source(body, state.group())
        broken_body = body.replace(
            "record->value.countdown -= travelStep",
            "record->value.countdown -= 1.0f",
        )
        self.assertEqual(
            broken_body.count("record->value.countdown -= 1.0f")
            - body.count("record->value.countdown -= 1.0f"),
            2,
            "negative control must alter only the two bus motion timers",
        )
        negative = fixture_source(broken_body, state.group())

        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            executable = compile_fixture(positive, directory, "bus_motion")
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"patched bus motion fixture failed:\n{result.stdout}\n{result.stderr}",
            )
            self.assertIn("MGMfree bus motion timing: PASS", result.stdout)

            broken = compile_fixture(negative, directory, "bus_motion_wall_clock")
            result = subprocess.run(
                [str(broken)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertNotEqual(
                result.returncode,
                0,
                "wall-clock decrement unexpectedly passed the widescreen regression",
            )


if __name__ == "__main__":
    unittest.main()
