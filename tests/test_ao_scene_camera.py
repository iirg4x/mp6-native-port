"""CPU contracts for the opt-in QA fixed scene-camera override."""

import ast
import math
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / "tests" / "integration" / "build_board_qa.py"
RUNNER = ROOT / "tests" / "integration" / "run_board_qa.py"


def _function(path, name, dependencies=(), namespace=None):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    wanted = set(dependencies) | {name}
    body = [node for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name in wanted]
    if len(body) != len(wanted):
        raise AssertionError(f"missing functions in {path}: {wanted}")
    scope = {} if namespace is None else dict(namespace)
    exec(compile(ast.Module(body=body, type_ignores=[]), str(path), "exec"), scope)
    return scope[name]


def _builder_probe():
    return _function(BUILDER, "leaf_camera_probe", ("replace_once",))


def _scene_camera_error():
    return _function(RUNNER, "scene_camera_error", namespace={"math": math})


class SceneCameraContracts(unittest.TestCase):
    def test_parser_accepts_finite_non_singular_camera(self):
        error = _scene_camera_error()
        self.assertIsNone(error(None, False))
        self.assertIsNone(error([0.0, 10.0, 10.0, 0.0, 0.0, 0.0, 45.0], False))
        self.assertIsNone(error([-3.5, 4.0, 9.0, 1.0, 2.0, 0.0, 178.999], False))

    def test_parser_rejects_invalid_shape_fov_points_and_up_singular_view(self):
        error = _scene_camera_error()
        valid = [0.0, 10.0, 10.0, 0.0, 0.0, 0.0, 45.0]
        self.assertIn("7 finite floats", error(valid[:6], False))
        self.assertIn("fov", error([*valid[:6], 0.0], False))
        self.assertIn("fov", error([*valid[:6], 179.0], False))
        self.assertIn("finite", error([math.nan, *valid[1:]], False))
        self.assertIn("finite", error([math.inf, *valid[1:]], False))
        self.assertIn("distinct", error([0.0, 1.0, 2.0, 0.0, 1.0, 2.0, 45.0], False))
        self.assertIn("vertical-only", error([0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 45.0], False))
        self.assertIn("--leaf-camera", error(valid, True))

    def test_probe_injects_scene_override_after_look_at_and_preserves_leaf_modes(self):
        probe = _builder_probe()
        source = (
            "    cameraP->lookAtFunc(cameraP);\n"
            "    cameraP->dispOn = TRUE;\n"
        )
        transformed = probe(source)
        scene = transformed.index('getenv("MP6_QA_SCENE_CAMERA")')
        self.assertGreater(scene, transformed.index("cameraP->lookAtFunc(cameraP)"))
        self.assertIn('static int qaSceneParsed;', transformed)
        self.assertEqual(transformed.count('sscanf(spec, "%f %f %f %f %f %f %f"'), 1)
        for assignment in (
                "cameraP->eye = (HuVecF){sceneEyeX, sceneEyeY, sceneEyeZ};",
                "cameraP->center = (HuVecF){sceneCenterX, sceneCenterY, sceneCenterZ};",
                "cameraP->up = (HuVecF){0.0f, 1.0f, 0.0f};",
                "cameraP->fov = sceneFov;",
                "Hu3DCameraPerspectiveSet(cameraP->bit, cameraP->fov,"):
            self.assertIn(assignment, transformed)
        self.assertEqual(transformed.count('mp6_frame_dump_trigger("scene.camera")'), 1)
        self.assertIn('[SCENE-CAMERA] {\\"tick\\":%ld', transformed)
        self.assertIn('if (getenv("MP6_QA_LEAF_CAMERA")', transformed)
        self.assertIn('float shift = 4.0f * HuSin', transformed)
        self.assertIn('qaLeafMode', transformed)

    def test_runner_wires_scene_env_before_run_directory_and_unshaded_qa_rewrite(self):
        runner = RUNNER.read_text(encoding="utf-8")
        builder = BUILDER.read_text(encoding="utf-8")
        self.assertIn("parser.add_argument('--scene-camera', nargs=7, type=float", runner)
        self.assertIn("env['MP6_QA_SCENE_CAMERA'] = ' '.join", runner)
        self.assertIn("scene_camera_error(args.scene_camera, args.leaf_camera)", runner)
        self.assertLess(runner.index("scene_camera_error(args.scene_camera, args.leaf_camera)"),
                        runner.index("directory = root / 'build/board-qa-runs'"))
        self.assertIn("env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}", runner)
        self.assertIn("p.depth[3] = level == 2 ? 0.35f : 0.20f;", builder)
        self.assertIn('std::getenv("MP6_QA_UNSHADED_AO") ? 0.0f', builder)
        self.assertIn('contents = replace_once(contents,', builder)
        # The default production value remains the fallback arm of the QA-only
        # rewrite, so ordinary/release behavior is unchanged.
        self.assertIn('(level == 2 ? 0.35f : 0.20f);', builder)


if __name__ == "__main__":
    unittest.main()
