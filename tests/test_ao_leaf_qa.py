"""CPU-only contracts for the isolated plant camera and neutral AO control."""
import ast
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / 'tests/integration/build_board_qa.py'
RUNNER = ROOT / 'tests/integration/run_board_qa.py'


def camera_transform():
    # Importing the builder removes its READY attestation and starts a build.
    # Compile only these pure string transforms, never its module-level code.
    tree = ast.parse(BUILDER.read_text())
    body = [node for node in tree.body if isinstance(node, ast.FunctionDef)
            and node.name in ('replace_once', 'leaf_camera_probe')]
    namespace = {}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(BUILDER), 'exec'), namespace)
    return namespace['leaf_camera_probe']


def leaf_mode_error():
    # Importing the runner would parse argv, create a run directory, and may
    # start a native process. Extract only the pure argument contract.
    tree = ast.parse(RUNNER.read_text())
    body = [node for node in tree.body if isinstance(node, ast.FunctionDef)
            and node.name == 'leaf_camera_mode_error']
    namespace = {}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace['leaf_camera_mode_error']


def leaf_fixture_validation():
    # Extract only pure runner argument validators; importing the runner would
    # parse argv, create a run directory, and may start a native process.
    tree = ast.parse(RUNNER.read_text())
    wanted = [node for node in tree.body if isinstance(node, ast.FunctionDef)
              and node.name in ('leaf_freeze_error', 'geometry_at_second_error')]
    namespace = {}
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(RUNNER), 'exec'), namespace)
    return namespace


class LeafQaContracts(unittest.TestCase):
    def test_camera_is_explicit_and_relative_to_first_camera_tick(self):
        marker = '    cameraP->dispOn = TRUE;\n'
        transformed = camera_transform()(marker)
        self.assertIn('getenv("MP6_QA_LEAF_CAMERA") && mp6_tick_count >= 3000', transformed)
        self.assertIn('if (!qaLeafTriggered) qaLeafStart = mp6_tick_count;', transformed)
        self.assertIn('(float)(mp6_tick_count - qaLeafStart) * 3.0f', transformed)
        self.assertIn('fov = 45.0f;', transformed)
        self.assertIn('mp6_frame_dump_trigger("leaf.camera");', transformed)
        self.assertIn('if (relativeTick <= 360 && relativeTick % 6 == 0)', transformed)
        self.assertIn('[LEAF-CAMERA]', transformed)
        self.assertIn('\\"aspect\\"', transformed)
        self.assertTrue(transformed.endswith(marker))
        self.assertEqual(transformed.count(marker), 1)

    def test_camera_modes_keep_fixed_orientation_and_bounded_probe_records(self):
        transformed = camera_transform()('    cameraP->dispOn = TRUE;\n')
        self.assertIn('if (!strcmp(qaLeafMode, "dolly"))', transformed)
        self.assertIn('1.0f + 0.25f * HuSin((float)relativeTick * 3.0f)', transformed)
        self.assertIn('else if (!strcmp(qaLeafMode, "fov"))', transformed)
        self.assertIn('45.0f + 10.0f * HuSin((float)relativeTick * 3.0f)', transformed)
        self.assertIn('else if (!strcmp(qaLeafMode, "static"))', transformed)
        self.assertIn('qaLeafMode = "pan";', transformed)
        self.assertIn('{\\"mode\\":\\"%s\\"}', transformed)
        self.assertIn('{\\"tick\\":%ld,\\"relativeTick\\":%ld,\\"mode\\":\\"%s\\"', transformed)
        self.assertIn('mp6_tick_count,relativeTick,qaLeafMode,eyeX,eyeY,eyeZ,centerX,centerY,centerZ,fov,cameraP->aspect', transformed)

    def test_leaf_mode_argument_is_safe_and_forwarded_only_when_enabled(self):
        source = RUNNER.read_text()
        self.assertIn("parser.add_argument('--leaf-camera-mode', choices=('pan','static','dolly','fov'), default='pan'", source)
        self.assertIn("env['MP6_QA_LEAF_CAMERA_MODE'] = args.leaf_camera_mode", source)
        self.assertIn("env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}", source)
        self.assertLess(source.index('leaf_camera_mode_error(args.leaf_camera, args.leaf_camera_mode)'),
                        source.index("directory = root / 'build/board-qa-runs'"))

        error = leaf_mode_error()
        self.assertIsNone(error(False, 'pan'))
        self.assertIsNone(error(True, 'static'))
        self.assertIn('--leaf-camera', error(False, 'static'))

    def test_leaf_freeze_fixture_and_second_tick_are_qa_only_and_exactly_gated(self):
        builder = BUILDER.read_text()
        runner = RUNNER.read_text()
        geometry = (ROOT / 'tests/integration/ground_geometry_probe.h').read_text()
        self.assertIn('data==0x00D70006', builder)
        self.assertIn('rootName && !strcmp(rootName,"b01_m002")', builder)
        self.assertIn('Hu3DMotionTimeSet(model,7.0f);', builder)
        self.assertIn('Hu3DMotionSpeedSet(model,0.0f);', builder)
        self.assertIn('\\"asset\\":\\"00d70006\\"', builder)
        self.assertIn('\\"resolvedModel\\":%d', builder)
        self.assertIn('MP6_QA_LEAF_FREEZE', builder)
        self.assertNotIn('model==32', builder)
        self.assertIn('MP6_QA_GEOMETRY_AT_SECOND', geometry)
        self.assertIn('\\"motionSpeed\\"', geometry)
        self.assertIn('\\"localVertices\\"', geometry)
        self.assertIn('\\"localTransform\\"', geometry)
        self.assertIn("parser.add_argument('--leaf-freeze', action='store_true'", runner)
        self.assertIn("parser.add_argument('--geometry-at-second', type=int", runner)
        self.assertIn("env['MP6_QA_LEAF_FREEZE'] = '1'", runner)
        self.assertIn("env['MP6_QA_GEOMETRY_AT_SECOND'] = str(args.geometry_at_second)", runner)
        self.assertIn("env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}", runner)
        self.assertLess(runner.index('leaf_freeze_error(args.leaf_camera, args.leaf_freeze)'),
                        runner.index("directory = root / 'build/board-qa-runs'"))

        validators = leaf_fixture_validation()
        self.assertIsNone(validators['leaf_freeze_error'](True, True))
        self.assertIn('--leaf-camera', validators['leaf_freeze_error'](False, True))
        self.assertIsNone(validators['geometry_at_second_error'](3300, 3330, 3370))
        for values in ((None, 3330, 3370), (3300, 0, 3370),
                       (3300, 3371, 3370), (3300, 3300, 3370)):
            with self.subTest(values=values):
                self.assertIsNotNone(validators['geometry_at_second_error'](*values))

    def test_changed_or_ambiguous_camera_anchor_fails_closed(self):
        transform = camera_transform()
        for source in ('', '    cameraP->dispOn = TRUE;\n' * 2):
            with self.subTest(source=source), self.assertRaises(AssertionError):
                transform(source)

    def test_unshaded_control_still_targets_exact_shipping_ao_amount(self):
        source = BUILDER.read_text()
        shader = (ROOT / 'src/gx/ambient_occlusion_shader.hpp').read_text()
        amount = 'let amount = clamp(2.5 * occluded / max(unoccluded, 1e-6), 0.0, 1.0);'
        self.assertEqual(shader.count(amount), 1)
        self.assertIn(repr(amount) + ", 'let amount = 0.0;'", source)
        self.assertIn('MP6_QA_UNSHADED_AO', source)

    def test_geometry_records_installed_camera_and_corrected_vertices(self):
        source = (ROOT / 'tests/integration/ground_geometry_probe.h').read_text()
        guard = source.index('if (!target || (mp6_tick_count != targetTick')
        camera = source.index('FILE *camera=fopen("geometry-camera.jsonl","a");')
        self.assertLess(guard, camera)
        self.assertLess(source.index('if (spaceTick!=mp6_tick_count)'), camera)
        self.assertIn('const HU3D_CAMERA *c=Hu3DCamera+Hu3DCameraNo;', source)
        self.assertIn('c->fov,c->aspect', source)
        self.assertIn('Hu3DCameraMtx[row][col]', source)
        self.assertIn('HuVecF *vertices = mp6_ground_vertices(draw->model,obj);', source)


if __name__ == '__main__':
    unittest.main()
