"""Validate the bounded live-scene snapshot and Blender conversion helpers."""

import ast
import json
import subprocess
import tempfile
from pathlib import Path
import unittest
from types import SimpleNamespace

from tools import blender_scene, build


class CornerLayerLifetime(unittest.TestCase):
    def test_layers_are_reacquired_after_customdata_allocation(self):
        class StaleLayer:
            @property
            def data(self):
                raise AssertionError('pre-allocation RNA wrapper was retained')

        class Layers:
            def __init__(self):
                self.current = SimpleNamespace(data=[SimpleNamespace() for _ in range(3)])
                self.lookups = 0

            def new(self, **kwargs):
                return StaleLayer()

            def __getitem__(self, name):
                self.lookups += 1
                return self.current

        uv, color = Layers(), Layers()
        mesh = SimpleNamespace(uv_layers=uv, color_attributes=color,
                               polygons=[SimpleNamespace(use_smooth=True)])
        blender_scene._assign_corner_data(mesh, [{'loops': [_loop(i, i/2, 0) for i in range(3)]}])
        self.assertEqual((uv.lookups, color.lookups), (1, 1))
        self.assertEqual(uv.current.data[2].uv, (1, 0))
        self.assertEqual(color.current.data[0].color, (1, 128/255, 64/255, 1))


IDENTITY = [
    [1.0, 0.0, 0.0, 0.0],
    [0.0, 1.0, 0.0, 0.0],
    [0.0, 0.0, 1.0, 0.0],
]


def _loop(vertex, u, v):
    return {
        "vertex": vertex,
        "normal": 0,
        "uv": [u, v],
        "rgba": [255, 128, 64, 255],
        "normal_value": [0.0, 1.0, 0.0],
    }


def _fixture():
    return {
        "schema": blender_scene.SCHEMA,
        "coordinate_system": "native_hsf_y_up",
        "tick": 900,
        "overlay": "fixturedll",
        "models": [{
            "model_id": 12,
            "model_matrix": IDENTITY,
            "materials": [{
                "index": 0,
                "name": "fixture_mat",
                "rgba": [80, 120, 160, 255],
                "inv_alpha": 0,
                "ref_alpha": 0,
            }],
            "objects": [{
                "object_index": 4,
                "instance": 0,
                "name": "duplicate_name",
                "type": 2,
                "flags": 0,
                "hidden": False,
                "supported": True,
                "transform": IDENTITY,
                "vertices": [
                    [0.0, 0.0, 0.0],
                    [1.0, 0.0, 0.0],
                    [1.0, 0.0, 1.0],
                    [0.0, 0.0, 1.0],
                ],
                "faces": [
                    {"primitive": "tri", "material": 0,
                     "loops": [_loop(0, 0.0, 0.0), _loop(1, 1.0, 0.0),
                               _loop(2, 1.0, 1.0)]},
                    {"primitive": "quad", "material": 0,
                     "loops": [_loop(0, 0.0, 0.0), _loop(2, 1.0, 1.0),
                               _loop(3, 0.0, 1.0), _loop(1, 1.0, 0.0)]},
                    {"primitive": "strip", "material": 0,
                     "loops": [_loop(0, 0.0, 0.0), _loop(1, 1.0, 0.0),
                               _loop(2, 1.0, 1.0), _loop(3, 0.0, 1.0)]},
                ],
            }, {
                "object_index": 8,
                "instance": 1,
                "name": "duplicate_name",
                "type": 2,
                "flags": 0,
                "hidden": False,
                "supported": False,
                "unsupported_reason": "skinned_cenv_mesh",
                "transform": IDENTITY,
            }],
            "hsf_cameras": [{
                "object_index": 9,
                "name": "fixture_camera",
                "position": [0.0, 5.0, 10.0],
                "target": [0.0, 0.0, 0.0],
                "up_rot": 0.0,
                "fov": 45.0,
                "near": 1.0,
                "far": 1000.0,
            }],
        }],
        "cameras": [{
            "camera_id": 0,
            "position": [0.0, 10.0, 20.0],
            "target": [0.0, 0.0, 0.0],
            "up": [0.0, 1.0, 0.0],
            "fov": 45.0,
        }],
    }


class BlenderSceneSnapshot(unittest.TestCase):
    def test_native_face_writer_emits_parseable_loops_array(self):
        """Exercise the production C writer, not just a hand-built snapshot."""
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="scene-export-") as directory:
            directory = Path(directory)
            executable = directory / "qa_scene_export_selftest.exe"
            result = subprocess.run([
                build.ZIG, "cc", *build.COMMON_FLAGS, "-O2", "-UNDEBUG",
                "-ffunction-sections", "-Wl,--gc-sections",
                "-I", str(root / "tests" / "integration"),
                str(root / "tests" / "native" / "qa_scene_export_selftest.c"),
                "-o", str(executable),
            ], cwd=root, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=root, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            fragment = result.stdout.strip()
            self.assertTrue(fragment.startswith(","), fragment)
            parsed = json.loads("{" + fragment[1:] + "}")
            self.assertEqual(len(parsed["faces"]), 1)
            self.assertEqual(len(parsed["faces"][0]["loops"]), 3)

    def test_synthetic_snapshot_loads_and_preserves_native_face_kinds(self):
        fixture = _fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scene.json"
            path.write_text(json.dumps(fixture), encoding="utf-8")
            loaded = blender_scene.load_snapshot(path)
        self.assertEqual(loaded["tick"], 900)
        self.assertEqual(loaded["models"][0]["hsf_cameras"][0]["name"], "fixture_camera")
        faces = loaded["models"][0]["objects"][0]["faces"]
        self.assertEqual(blender_scene.blender_faces(faces[0]), [(0, 1, 2)])
        self.assertEqual(blender_scene.blender_faces(faces[1]), [(0, 2, 3, 1)])
        self.assertEqual(blender_scene.blender_faces(faces[2]), [(0, 1, 2), (2, 1, 3)])
        expanded = blender_scene._expanded_face_records(faces[2])
        self.assertEqual([[loop["vertex"] for loop in record["loops"]]
                          for record in expanded], [[0, 1, 2], [2, 1, 3]])

    def test_conversion_keeps_y_up_scene_orientation_explicit(self):
        self.assertEqual(blender_scene.native_to_blender([1.0, 2.0, 3.0]), (1.0, -3.0, 2.0))
        self.assertEqual(blender_scene.native_to_blender([0.0, 1.0, 0.0]), (0.0, 0.0, 1.0))
        self.assertEqual(blender_scene.matrix_to_blender(IDENTITY), [
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ])
        self.assertEqual(blender_scene.matrix_to_blender([
            [1.0, 0.0, 0.0, 1.0],
            [0.0, 1.0, 0.0, 2.0],
            [0.0, 0.0, 1.0, 3.0],
        ]), [
            [1.0, 0.0, 0.0, 1.0],
            [0.0, 1.0, 0.0, -3.0],
            [0.0, 0.0, 1.0, 2.0],
            [0.0, 0.0, 0.0, 1.0],
        ])

    def test_unsupported_mesh_requires_explicit_reason(self):
        fixture = _fixture()
        fixture["models"][0]["objects"][1].pop("unsupported_reason")
        with self.assertRaisesRegex(ValueError, "explain why it is unsupported"):
            blender_scene.validate_snapshot(fixture)

    def test_unsupported_model_requires_explicit_reason(self):
        fixture = _fixture()
        fixture["models"][0]["supported"] = False
        with self.assertRaisesRegex(ValueError, "explain why it is unsupported"):
            blender_scene.validate_snapshot(fixture)

    def test_bpy_is_not_imported_at_module_scope(self):
        tree = ast.parse(Path(blender_scene.__file__).read_text(encoding="utf-8"))
        top_level_bpy_imports = [node for node in tree.body
                                 if isinstance(node, (ast.Import, ast.ImportFrom))
                                 and any(alias.name == "bpy" for alias in node.names)]
        self.assertEqual(top_level_bpy_imports, [])


if __name__ == "__main__":
    unittest.main()
