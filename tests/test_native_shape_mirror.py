"""Native fixture coverage for the row-aware animated shape mirror."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    match = re.search(
        r"^(?:static )?(?:void|BOOL|float|int|HSF_OBJECT \*) "
        + re.escape(name)
        + r"\([^;]+?\)\n\{.*?^\}",
        text,
        re.M | re.S,
    )
    if match is None:
        raise AssertionError(f"could not extract {name}")
    return match.group()


class NativeShapeMirror(unittest.TestCase):
    def test_real_shape_code_preserves_native_prefix_and_row_seams(self):
        source = (ROOT / "src/hsf/mp6_widescreen_extrude.c").read_text(
            encoding="utf-8"
        )
        type_start = source.index("typedef enum {\n    MP6_WS_EXTEND_TILES")
        type_end = source.index("#define MP6_WS_EXTEND_MAX", type_start)
        types = source[type_start:type_end]

        shape_start = source.index("/* Reflect a row-structured shape mesh")
        shape_end = source.index(
            "\nvoid mp6_widescreen_extend_terrain_sides(", shape_start
        )
        shape = source[shape_start:shape_end]
        dispatcher = function(source, "mp6_ws_extend_apply")

        # Keep this fixture tied to the implementation's actual guarded API,
        # not a hand-written shape-only approximation.
        self.assertIn("mp6_ws_extend_locate_mesh_impl(modelId, objName, TRUE, 0, TRUE)", shape)
        self.assertIn("MP6_WS_SHAPE_SEA_FACES * 4", shape)
        self.assertIn("mp6_ws_extend_shape_faces", shape)
        self.assertIn("mp6_ws_extend_shape_apply", dispatcher)
        self.assertIn("case MP6_WS_EXTEND_SHAPE_MIRROR:", dispatcher)

        with tempfile.TemporaryDirectory(prefix="wide-shape-", dir=ROOT / "build") as tmp:
            directory = Path(tmp)
            (directory / "shape_types.inc").write_text(types, encoding="utf-8")
            (directory / "shape_subject.inc").write_text(
                shape + "\n" + dispatcher + "\n", encoding="utf-8"
            )
            exe = directory / "test.exe"
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    *build.COMMON_FLAGS,
                    "-O2",
                    "-UNDEBUG",
                    "-I" + str(directory),
                    str(ROOT / "tests/native/shape_mirror_selftest.c"),
                    "-o",
                    str(exe),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(exe)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
