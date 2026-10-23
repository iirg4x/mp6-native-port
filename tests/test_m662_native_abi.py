"""Executable ABI, reset, timer, camera, and particle probes for Trick or Tree."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body


ROOT = Path(__file__).resolve().parents[1]
MODULE = "m662dll"


def patched(unit):
    decomp = Path(apply_patches.require_pinned_decomp())
    rel = f"src/REL/{MODULE}/{unit}.c"
    source = (decomp / rel).read_text(encoding="utf-8")
    patch = ROOT / "compat/decomp" / (rel + ".patch")
    if patch.exists():
        source = apply_patches.apply_unified_diff(source, patch.read_text(encoding="utf-8"), rel)
    return apply_patches.with_rel_state(source, rel)


def reset_tail(source):
    marker = '#include "mp6_rel_state.h"'
    position = source.find(marker)
    if position < 0:
        raise AssertionError("missing native reset owner")
    return source[position:]


class M662NativeABI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def test_patched_native_contracts_are_explicit(self):
        main = patched("main")
        game = patched("game")
        puzzle = patched("puzzle")
        ending = patched("ending")
        header = (ROOT / "include/mp6_m662_native.h").read_text(encoding="utf-8")

        self.assertIn('#include "mp6_m662_native.h"', main)
        self.assertIn('#include "mp6_m662_native.h"', game)
        self.assertIn('#include "mp6_m662_native.h"', puzzle)
        self.assertIn('#include "mp6_m662_native.h"', ending)
        for name in (
            "lbl_1_bss_C60", "lbl_1_bss_C58", "lbl_1_bss_C50",
            "lbl_1_bss_B08", "lbl_1_bss_ACC", "lbl_1_bss_A7C",
            "lbl_1_bss_A74", "lbl_1_bss_4B4", "lbl_1_bss_3F4",
            "lbl_1_bss_2F4", "lbl_1_bss_210", "lbl_1_bss_D0",
            "lbl_1_bss_0",
        ):
            self.assertEqual(main.count(f"X({name})"), 1, name)
        for name in ("lbl_1_data_0", "lbl_1_data_F6"):
            self.assertEqual(main.count(f"X({name})"), 1, name)
        for name in ("lbl_1_data_150", "lbl_1_data_32C"):
            self.assertEqual(game.count(f"X({name})"), 1, name)
        for name in ("lbl_1_data_448", "lbl_1_data_4E8"):
            self.assertEqual(puzzle.count(f"X({name})"), 1, name)
        self.assertIn("sizeof(M662ParticleParameters)", main)
        self.assertIn("sizeof(*particle->data)", main)
        self.assertRegex(
            header,
            r"MGTIMER \*fn_1_2EC0\(int timerType, int durationSeconds, int endSeconds,",
        )
        self.assertIn("void fn_1_48E8(M662Player *player, s16 spatialMode);", header)
        self.assertIn("void fn_1_5008(s16 soundSlot, s16 spatialMode, HuVecF *position);", header)

    def test_extracted_native_probe(self):
        main = patched("main")
        camera = function_definition_body(main, "fn_1_1918").replace(
            "fn_1_1918(", "fn_1_1918_real(", 1
        )
        setup = function_definition_body(main, "fn_1_130")
        interpolation = function_definition_body(main, "fn_1_2DA0")
        timer_create = function_definition_body(main, "fn_1_2EC0")
        timer_start = function_definition_body(main, "fn_1_2F34")
        timer_done = function_definition_body(main, "fn_1_2F60")
        timer_stop = function_definition_body(main, "fn_1_2F88")
        particle_setup = function_definition_body(main, "fn_1_3CEC")
        particle_emit = function_definition_body(main, "fn_1_4270")
        particle_update = function_definition_body(main, "fn_1_4470")
        game = patched("game")
        path_update = function_definition_body(game, "fn_1_795C")
        path_step = function_definition_body(game, "fn_1_7AF8")
        main_reset = reset_tail(main)
        game_reset = reset_tail(patched("game"))
        puzzle_reset = reset_tail(patched("puzzle"))

        fixture = (ROOT / "tests/native/m662_native_selftest.c").read_text(encoding="utf-8")
        subject = "\n\n".join((
            setup, camera, interpolation, timer_create, timer_start, timer_done,
            timer_stop, particle_setup, particle_emit, particle_update,
            path_update, path_step,
            main_reset, game_reset, puzzle_reset,
        ))
        source = fixture.replace("/* M662_NATIVE_SUBJECT */", subject)


        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "m662_native_selftest.c"
            executable = temporary / "m662_native_selftest.exe"
            source_path.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    *build.COMMON_FLAGS,
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(self.decomp / "include"),
                    "-UNDEBUG",
                    "-fsanitize=undefined",
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
                f"m662 native probe compile failed:\n{result.stdout}\n{result.stderr}",
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
                f"m662 native probe failed:\n{result.stdout}\n{result.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
