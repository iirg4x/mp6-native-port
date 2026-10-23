"""CPU contracts for native leaf-camera log registration."""

from pathlib import Path
import json
import sys
import tempfile
import unittest

INTEGRATION = Path(__file__).resolve().parent / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

try:
    import numpy as np
except ImportError:  # pragma: no cover - optional analytic dependency
    np = None

if np is not None:
    from analyze_ao_leaf_camera_modes import (
        FREEZE_ASSET_TEXT,
        FREEZE_TARGET_NAMES,
        FREEZE_ROOT,
        LeafCameraRecord,
        LeafFreezeRecord,
        _camera_for_log,
        look_at_view_matrix,
        load_run_provenance,
        normalize_log_aspect,
        pair_camera_records,
        _repeated_pose_pairs,
        _repeated_pose_stats,
        parse_leaf_freeze_log,
        parse_leaf_camera_log,
        target_covered_by_bilinear_footprint,
        _segment_target_hits,
        _visibility_triangles_for_row,
        _xe_policy_from_role,
        validate_xe_policy_log,
        validate_frozen_geometry_pair,
        validate_provenance_pair,
        validate_camera_record_against_logs,
    )


def _record(*, tick=3300, relative_tick=300, mode="static", fov=45.0,
            aspect=1.2, eye=(-333.0, 90.0, 3273.0),
            center=(-333.0, 25.0, 3073.0)):
    return LeafCameraRecord(tick, relative_tick, mode, fov, aspect,
                            tuple(eye), tuple(center))


def _installed(record=None):
    row = record or _record()
    return {
        "tick": row.tick,
        "fov": row.fov,
        "aspect": 1.59000015,
        "viewport": [0, 0, 848, 480],
        "viewMatrix": look_at_view_matrix(row.eye, row.center).reshape(-1).tolist(),
    }


@unittest.skipIf(np is None, "optional NumPy unavailable")
class LeafCameraModeContracts(unittest.TestCase):
    def test_look_at_center_and_y_up_orientation(self):
        matrix = look_at_view_matrix((1, 2, 10), (1, 2, 0))
        np.testing.assert_allclose(matrix[:, :3], np.eye(3), atol=1e-12, rtol=0)
        np.testing.assert_allclose(matrix[:, 3], [-1, -2, -10], atol=1e-12, rtol=0)
        center_q = matrix[:, :3] @ np.array([1., 2., 0.]) + matrix[:, 3]
        np.testing.assert_allclose(center_q, [0, 0, -10], atol=1e-12, rtol=0)

    def test_look_at_dolly_changes_depth_and_screen_scale(self):
        near = _record(eye=(0, 0, 10), center=(0, 0, 0), aspect=1.2)
        far = _record(eye=(0, 0, 20), center=(0, 0, 0), aspect=1.2)
        near_m = look_at_view_matrix(near.eye, near.center)
        far_m = look_at_view_matrix(far.eye, far.center)
        point = np.array([1., 0., 0.])
        near_q = near_m[:, :3] @ point + near_m[:, 3]
        far_q = far_m[:, :3] @ point + far_m[:, 3]
        self.assertEqual(near_q[2], -10.)
        self.assertEqual(far_q[2], -20.)
        self.assertAlmostEqual(near_q[0] / -near_q[2], 2 * far_q[0] / -far_q[2])

    def test_fov_is_a_projection_scale_not_a_pose_change(self):
        eye = (0, 0, 10)
        center = (0, 0, 0)
        point = np.array([1., 0., 0.])
        matrix = look_at_view_matrix(eye, center)
        q = matrix[:, :3] @ point + matrix[:, 3]
        z = -q[2]
        near_fov = ((q[0] / (z * np.tan(np.radians(45.) / 2) * 1.59000015) + 1) * 640 / 2 - .5)
        wide_fov = ((q[0] / (z * np.tan(np.radians(55.) / 2) * 1.59000015) + 1) * 640 / 2 - .5)
        self.assertGreater(near_fov, wide_fov)
        self.assertNotEqual(near_fov, wide_fov)

    def test_installed_record_pose_and_fov_are_checked_at_same_tick(self):
        row = _record()
        installed = _installed(row)
        validate_camera_record_against_logs(installed, [row])
        with self.assertRaises(ValueError):
            validate_camera_record_against_logs(installed, [_record(center=(-333., 26., 3073.))])
        with self.assertRaises(ValueError):
            validate_camera_record_against_logs(installed, [_record(fov=46.)])

    def test_phase_mode_and_fov_mismatches_are_rejected(self):
        installed_aspect = 1.59000015
        good = [_record(tick=3000, relative_tick=0), _record(tick=3006, relative_tick=6)]
        pair_camera_records(good, list(good), installed_aspect=installed_aspect)
        with self.assertRaises(ValueError):
            pair_camera_records(good, [_record(tick=3000, relative_tick=6), good[1]],
                                installed_aspect=installed_aspect)
        with self.assertRaises(ValueError):
            pair_camera_records(good, [_record(tick=3000, mode="pan"), good[1]],
                                installed_aspect=installed_aspect)
        with self.assertRaises(ValueError):
            pair_camera_records(good, [_record(tick=3000, fov=46.), good[1]],
                                installed_aspect=installed_aspect)
        with self.assertRaises(ValueError):
            pair_camera_records(good, [_record(tick=3000, eye=(-332.99, 90., 3273.)), good[1]],
                                installed_aspect=installed_aspect)

    def test_repeated_pose_metric_is_separate_and_has_no_threshold(self):
        first = _record(tick=3000, relative_tick=0)
        second = _record(tick=3120, relative_tick=120)
        pairs = ((first, first), (second, second))
        repeated = _repeated_pose_pairs(pairs, [3000, 3120])
        common = np.ones((1, 3), dtype=bool)
        stats = _repeated_pose_stats(
            repeated,
            [np.array([[1., 2., 3.]]), np.array([[2., 4., 8.]])],
            common, [3000, 3120])
        self.assertEqual(repeated, ((0, 1),))
        self.assertEqual(stats["pair_count"], 1)
        self.assertEqual(stats["sample_count"], 3)
        self.assertAlmostEqual(stats["mean_abs_difference"], 8.0 / 3.0)
        self.assertAlmostEqual(stats["p95_abs_difference"], 4.7)
        self.assertEqual(stats["max_abs_difference"], 5.0)
        changed = _record(tick=3120, relative_tick=120, fov=46.)
        self.assertEqual(_repeated_pose_pairs(((first, first), (changed, changed)),
                                              [3000, 3120]), ())

    def test_visibility_segment_hits_front_target_but_not_behind_target(self):
        origin = np.array([0., 0., 0.])
        endpoint = np.array([[0., 0., 10.]])
        front = (np.array([-1., -1., 5.]), np.array([1., -1., 5.]),
                 np.array([0., 1., 5.]))
        behind = (np.array([-1., -1., 15.]), np.array([1., -1., 15.]),
                  np.array([0., 1., 15.]))
        np.testing.assert_array_equal(_segment_target_hits(origin, endpoint, (front,)), [True])
        np.testing.assert_array_equal(_segment_target_hits(origin, endpoint, (behind,)), [False])

    def test_visibility_quad_is_triangulated_as_two_triangles(self):
        row = {
            "name": "flower_a",
            "vertices": [[0., 0., 5.], [1., 0., 5.], [1., 1., 5.], [0., 1., 5.]],
            "faces": [[3, 0, 1, 2, 3]],
        }
        triangles = _visibility_triangles_for_row(row)
        self.assertEqual(len(triangles), 2)
        np.testing.assert_allclose(triangles[0], [
            [0., 0., 5.], [1., 1., 5.], [0., 1., 5.],
        ])
        np.testing.assert_allclose(triangles[1], [
            [0., 0., 5.], [0., 1., 5.], [1., 0., 5.],
        ])

    def test_visibility_crossed_storage_uses_native_diagonal_for_nonplanar_quad(self):
        # HSF quad storage is crossed in the x/y projection (0,1,2,3), while
        # native GX emission visits the perimeter as 0,2,3,1.  The non-planar
        # fourth corner makes the wrong diagonal observably different.
        row = {
            "name": "nonplanar_leaf_quad",
            "vertices": [[0., 0., 0.], [0., 1., .25],
                          [1., 0., 1.], [1., 1., 2.]],
            "faces": [[3, 0, 1, 2, 3]],
        }
        triangles = _visibility_triangles_for_row(row)
        np.testing.assert_allclose(triangles[0], [
            [0., 0., 0.], [1., 0., 1.], [1., 1., 2.],
        ])
        np.testing.assert_allclose(triangles[1], [
            [0., 0., 0.], [1., 1., 2.], [0., 1., .25],
        ])

    def test_visibility_bilinear_footprint_checks_all_four_corners(self):
        row = _record(tick=0, relative_tick=0, eye=(0., 10., 10.), center=(0., 0., 0.))
        camera = _camera_for_log(row, _installed(row), (10, 10), 1.2, 5e-4)
        origin = -camera.inverse @ camera.matrix[:, 3]
        target = camera.world_on_plane(np.array([5.]), np.array([5.]), 0.)[0]
        direction = target - origin
        direction /= np.linalg.norm(direction)
        basis_one = np.cross(direction, np.array([0., 1., 0.]))
        basis_one /= np.linalg.norm(basis_one)
        basis_two = np.cross(direction, basis_one)
        center = origin + (target - origin) * .5
        triangle = (
            center + basis_one * .02,
            center + basis_two * .02,
            center - (basis_one + basis_two) * .02,
        )
        covered = target_covered_by_bilinear_footprint(
            camera, np.array([[4.2]]), np.array([[4.2]]),
            np.array([[True]]), 0., (triangle,))
        self.assertTrue(bool(covered[0, 0]))

    def test_visibility_rejects_unsupported_faces_and_malformed_shapes(self):
        base = {
            "name": "flower_a",
            "vertices": [[0., 0., 5.], [1., 0., 5.], [0., 1., 5.]],
            "faces": [[2, 0, 1, 2]],
        }
        _visibility_triangles_for_row(base)
        with self.assertRaises(ValueError):
            _visibility_triangles_for_row({**base, "faces": [[1, 0, 1, 2]]})
        with self.assertRaises(ValueError):
            _visibility_triangles_for_row({**base, "faces": [[2, 0, 1]]})
        with self.assertRaises(ValueError):
            _visibility_triangles_for_row({**base, "vertices": [[0., 0.], [1., 0.], [0., 1.]]})

    def test_raw_or_installed_aspect_is_allowed_but_unknown_is_rejected(self):
        self.assertEqual(normalize_log_aspect(1.2, 1.59000015), "raw-board")
        self.assertEqual(normalize_log_aspect(1.59000015, 1.59000015), "installed")
        with self.assertRaises(ValueError):
            normalize_log_aspect(1.4, 1.59000015)

    def test_parser_ignores_legacy_mode_marker_but_requires_sample_fields(self):
        text = "\n".join([
            '[LEAF-CAMERA] {"mode":"static"}',
            '[LEAF-CAMERA] {"tick":3000,"relativeTick":0,"mode":"static",'
            '"fov":45,"aspect":1.2,"eye":[0,0,10],"center":[0,0,0]}',
        ])
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            path.write_text(text, encoding="utf-8")
            records = parse_leaf_camera_log(path)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].relative_tick, 0)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            path.write_text('[LEAF-CAMERA] {"tick":3000,"mode":"static"}\n', encoding="utf-8")
            with self.assertRaises(ValueError):
                parse_leaf_camera_log(path)

    @staticmethod
    def _result_request(**overrides):
        request = {
            "window_size": "1280x720",
            "capture_frames": 31,
            "capture_stride": 6,
            "capture_trigger": "leaf.camera",
            "capture_delay": 120,
            "ticks": 3370,
            "input_script": "period:30;timeout:3360;pressuntil:a/w01.live/120;wait:2000",
            "widescreen": True,
            "leaf_camera": True,
            "leaf_camera_mode": "static",
            "geometry_at": 3300,
            "implementation": "legacy",
            "unshaded_ao": True,
            "ao": 2,
            "xegtao_quality": "high",
            "xegtao_denoise": 2,
            "xegtao_resolution": "full",
            "xegtao_depth_storage": "r32float",
        }
        request.update(overrides)
        return request

    @classmethod
    def _write_result(cls, folder, *, role="neutral", sha="same", **overrides):
        request = cls._result_request(**overrides)
        storage = None
        if role.startswith("xe_"):
            quality, denoise = _xe_policy_from_role(role)
            request.update(implementation="xegtao", unshaded_ao=False,
                           xegtao_quality=quality, xegtao_denoise=denoise)
            storage = {"requested": "r32float", "actual": "r32float"}
        request.update(overrides)
        result = {"request": request, "sha256": sha, "exit_code": 0,
                  "crash_marker": False}
        if storage is not None:
            result["xegtao_depth_storage"] = storage
        path = Path(folder) / "result.json"
        path.write_text(json.dumps(result), encoding="utf-8")
        if role.startswith("xe_"):
            quality, denoise = _xe_policy_from_role(role)
            (Path(folder) / "game.log").write_text(
                "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float\n"
                "[MP6-AO] XeGTAO experimental: full-resolution direct uint output, "
                f"{quality.title()}, {denoise} spatial denoise passes; opt-in only\n",
                encoding="utf-8")
        return path

    @staticmethod
    def _freeze_row(tick, name, *, model=17, world_offset=0.0,
                    local_offset=0.0, transform_offset=0.0,
                    motion_time=7.0, motion_speed=0.0,
                    faces=None):
        local_vertices = [
            [0.0 + local_offset, 0.0, 0.0],
            [1.0 + local_offset, 0.0, 0.0],
            [0.0 + local_offset, 1.0, 0.0],
        ]
        world_vertices = [
            [10.0 + world_offset, 5.0, 20.0],
            [11.0 + world_offset, 5.0, 20.0],
            [10.0 + world_offset, 6.0, 20.0],
        ]
        return {
            "tick": tick,
            "model": model,
            "name": name,
            "motionTime": motion_time,
            "motionSpeed": motion_speed,
            "localVertices": local_vertices,
            "faces": faces if faces is not None else [[0, 1, 2]],
            "localTransform": {
                "pos": [transform_offset, 0.0, 0.0],
                "rot": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "vertices": world_vertices,
        }

    @classmethod
    def _write_freeze_fixture(cls, folder, *, role, sha="same",
                              ticks=(3300, 3360), model=17,
                              marker_model=None, second_tick=True,
                              world_offset=0.0, local_offset=0.0,
                              transform_offset=0.0, motion_time=7.0,
                              motion_speed=0.0, faces=None):
        folder = Path(folder)
        folder.mkdir(parents=True, exist_ok=True)
        geometry_at, geometry_second = ticks
        cls._write_result(
            folder, role=role, sha=sha, leaf_freeze=True,
            geometry_at=geometry_at,
            geometry_at_second=geometry_second if second_tick else None)
        marker_model = model if marker_model is None else marker_model
        (folder / "game.log").write_text(
            "[LEAF-FREEZE] " + json.dumps({
                "asset": FREEZE_ASSET_TEXT,
                "root": FREEZE_ROOT,
                "resolvedModel": marker_model,
                "time": 7,
                "speed": 0,
            }) + "\n", encoding="utf-8")
        with (folder / "geometry.jsonl").open("w", encoding="utf-8") as stream:
            for tick in ticks[:2 if second_tick else 1]:
                for name in FREEZE_TARGET_NAMES:
                    row = cls._freeze_row(
                        tick, name, model=model, world_offset=world_offset,
                        local_offset=local_offset, transform_offset=transform_offset,
                        motion_time=motion_time, motion_speed=motion_speed,
                        faces=faces)
                    stream.write(json.dumps(row) + "\n")

    def test_result_provenance_requires_roles_binary_and_phase(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral_dir = root / "neutral"
            xe_dir = root / "xe"
            neutral_dir.mkdir()
            xe_dir.mkdir()
            self._write_result(neutral_dir)
            self._write_result(xe_dir, role="xe_ultra3")
            neutral = load_run_provenance(neutral_dir, "neutral")
            xe = load_run_provenance(xe_dir, "xe_ultra3")
            validate_provenance_pair(neutral, xe)
            self.assertEqual(neutral["actual_quality"]["ao"], 2)
            self.assertEqual(xe["actual_quality"]["ao"], 2)
            self.assertEqual(xe["actual_quality"]["quality"], "ultra")
            self.assertEqual(xe["actual_quality"]["depth_storage"], "r32float")
            self.assertEqual(xe["actual_quality"]["depth_storage_actual"], "r32float")
            self.assertEqual(xe["actual_quality"]["logged_policy"]["quality"], "ultra")
            self.assertEqual(xe["actual_quality"]["logged_policy"]["denoise"], 3)

            self._write_result(xe_dir, role="xe_ultra3", sha="other")
            other_xe = load_run_provenance(xe_dir, "xe_ultra3")
            with self.assertRaises(ValueError):
                validate_provenance_pair(neutral, other_xe)
            self._write_result(xe_dir, role="xe_ultra3", xegtao_denoise=2)
            with self.assertRaises(ValueError):
                load_run_provenance(xe_dir, "xe_ultra3")

    def test_result_provenance_requires_explicit_ao2_for_both_roles(self):
        invalid_values = (("off", 0), ("wrong_level", 1),
                          ("wrong_type", "2"), ("bool", True))
        for role in ("neutral", "xe_ultra3"):
            with self.subTest(role=role):
                for label, value in (("missing", None), *invalid_values):
                    with self.subTest(value=label):
                        with tempfile.TemporaryDirectory() as folder:
                            path = Path(folder)
                            self._write_result(path, role=role)
                            result_path = path / "result.json"
                            result = json.loads(result_path.read_text(encoding="utf-8"))
                            if label == "missing":
                                del result["request"]["ao"]
                            else:
                                result["request"]["ao"] = value
                            result_path.write_text(json.dumps(result), encoding="utf-8")
                            with self.assertRaises(ValueError):
                                load_run_provenance(path, role)

    def test_xe_policy_log_rejects_requested_only_or_wrong_actual_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            path.write_text(
                "[MP6-AO] XeGTAO experimental: full-resolution direct uint output, "
                "Ultra, 2 spatial denoise passes; opt-in only\n"
                "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float\n",
                encoding="utf-8")
            with self.assertRaises(ValueError):
                validate_xe_policy_log(path)
            path.write_text(
                "[MP6-AO] XeGTAO experimental: full-resolution direct uint output, "
                "Ultra, 3 spatial denoise passes; opt-in only\n",
                encoding="utf-8")
            with self.assertRaises(ValueError):
                validate_xe_policy_log(path)

    def test_high2_policy_is_qualified_and_request_log_mismatches_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            xe_dir = path / "xe-high2"
            xe_dir.mkdir()
            self._write_result(xe_dir, role="xe_high2")
            provenance = load_run_provenance(
                xe_dir, "xe_high2", xe_quality="high", xe_denoise=2)
            self.assertEqual(provenance["actual_quality"]["quality"], "high")
            self.assertEqual(provenance["actual_quality"]["denoise"], 2)
            with self.assertRaises(ValueError):
                load_run_provenance(xe_dir, "xe_ultra3")

            self._write_result(xe_dir, role="xe_high2", xegtao_quality="ultra")
            with self.assertRaises(ValueError):
                load_run_provenance(
                    xe_dir, "xe_high2", xe_quality="high", xe_denoise=2)
            self._write_result(xe_dir, role="xe_high2")
            (xe_dir / "game.log").write_text(
                "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float\n"
                "[MP6-AO] XeGTAO experimental: full-resolution direct uint output, "
                "Ultra, 3 spatial denoise passes; opt-in only\n",
                encoding="utf-8")
            with self.assertRaises(ValueError):
                load_run_provenance(
                    xe_dir, "xe_high2", xe_quality="high", xe_denoise=2)

    def test_high2_reference_policy_log_is_accepted_without_trailing_text(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            path.write_text(
                "[MP6-AO] XeGTAO reference: full-resolution direct uint output, "
                "High, 2 spatial denoise passes\n"
                "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float\n",
                encoding="utf-8")
            policy = validate_xe_policy_log(path, quality="high", denoise=2)
        self.assertEqual(policy["quality"], "high")
        self.assertEqual(policy["denoise"], 2)

    def test_result_provenance_rejects_crash_missing_fields_and_phase_mismatch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral_dir = root / "neutral"
            xe_dir = root / "xe"
            neutral_dir.mkdir()
            xe_dir.mkdir()
            neutral_path = self._write_result(neutral_dir)
            self._write_result(xe_dir, role="xe_ultra3")
            neutral = load_run_provenance(neutral_dir, "neutral")
            xe = load_run_provenance(xe_dir, "xe_ultra3")
            validate_provenance_pair(neutral, xe)
            xe["phase"]["capture_delay"] = 121
            with self.assertRaises(ValueError):
                validate_provenance_pair(neutral, xe)
            broken = json.loads(neutral_path.read_text(encoding="utf-8"))
            broken["crash_marker"] = True
            neutral_path.write_text(json.dumps(broken), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_run_provenance(neutral_dir, "neutral")

    def test_animated_provenance_defaults_are_not_labeled_frozen(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            self._write_result(path, role="neutral")
            provenance = load_run_provenance(path, "neutral")
        self.assertEqual(provenance["freeze"], {
            "enabled": False,
            "geometry_at": 3300,
            "geometry_at_second": None,
        })
        self.assertFalse(provenance["phase"]["leaf_freeze"])
        self.assertIsNone(provenance["phase"]["geometry_at_second"])

    def test_freeze_marker_requires_exact_selection(self):
        good = {
            "asset": FREEZE_ASSET_TEXT,
            "root": FREEZE_ROOT,
            "resolvedModel": 17,
            "time": 7,
            "speed": 0,
        }
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            path.write_text("[LEAF-FREEZE] " + json.dumps(good) + "\n",
                            encoding="utf-8")
            record = parse_leaf_freeze_log(path)
        self.assertEqual(record, LeafFreezeRecord(
            FREEZE_ASSET_TEXT, FREEZE_ROOT, 17, 7.0, 0.0))
        for key, value in (("asset", "0x00D70007"),
                           ("root", "wrong_root"),
                           ("time", 6), ("speed", 1)):
            broken = dict(good, **{key: value})
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "game.log"
                path.write_text("[LEAF-FREEZE] " + json.dumps(broken) + "\n",
                                encoding="utf-8")
                with self.assertRaises(ValueError):
                    parse_leaf_freeze_log(path)

    def test_frozen_geometry_proof_checks_two_ticks_and_all_targets(self):
        freeze = LeafFreezeRecord(FREEZE_ASSET_TEXT, FREEZE_ROOT, 17, 7.0, 0.0)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral = root / "neutral"
            xe = root / "xe"
            self._write_freeze_fixture(neutral, role="neutral")
            self._write_freeze_fixture(xe, role="xe_ultra3")
            proof = validate_frozen_geometry_pair(
                neutral, xe, geometry_at=3300, geometry_at_second=3360,
                neutral_freeze=freeze, xe_freeze=freeze)
        self.assertTrue(proof["enabled"])
        self.assertEqual(proof["geometry_ticks"], [3300, 3360])
        self.assertEqual(proof["targets"], list(FREEZE_TARGET_NAMES))
        self.assertTrue(proof["local_vertices_unchanged"])
        self.assertTrue(proof["faces_unchanged"])
        self.assertTrue(proof["local_transform_unchanged"])
        self.assertEqual(proof["world_max_abs_delta"], 0.0)
        self.assertEqual(proof["records_per_run"]["neutral"]["3300"], 5)

    def test_frozen_geometry_proof_rejects_missing_second_tick_and_motion_change(self):
        freeze = LeafFreezeRecord(FREEZE_ASSET_TEXT, FREEZE_ROOT, 17, 7.0, 0.0)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral = root / "neutral"
            xe = root / "xe"
            self._write_freeze_fixture(neutral, role="neutral", second_tick=False)
            self._write_freeze_fixture(xe, role="xe_ultra3", second_tick=False)
            with self.assertRaises(ValueError):
                validate_frozen_geometry_pair(
                    neutral, xe, geometry_at=3300, geometry_at_second=3360,
                    neutral_freeze=freeze, xe_freeze=freeze)

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral = root / "neutral"
            xe = root / "xe"
            self._write_freeze_fixture(neutral, role="neutral", motion_speed=0.25)
            self._write_freeze_fixture(xe, role="xe_ultra3")
            with self.assertRaises(ValueError):
                validate_frozen_geometry_pair(
                    neutral, xe, geometry_at=3300, geometry_at_second=3360,
                    neutral_freeze=freeze, xe_freeze=freeze)

    def test_frozen_geometry_proof_rejects_local_or_world_drift(self):
        freeze = LeafFreezeRecord(FREEZE_ASSET_TEXT, FREEZE_ROOT, 17, 7.0, 0.0)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral = root / "neutral"
            xe = root / "xe"
            self._write_freeze_fixture(neutral, role="neutral", local_offset=0.01)
            self._write_freeze_fixture(xe, role="xe_ultra3")
            with self.assertRaises(ValueError):
                validate_frozen_geometry_pair(
                    neutral, xe, geometry_at=3300, geometry_at_second=3360,
                    neutral_freeze=freeze, xe_freeze=freeze)

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            neutral = root / "neutral"
            xe = root / "xe"
            self._write_freeze_fixture(neutral, role="neutral", world_offset=0.002)
            self._write_freeze_fixture(xe, role="xe_ultra3")
            with self.assertRaises(ValueError):
                validate_frozen_geometry_pair(
                    neutral, xe, geometry_at=3300, geometry_at_second=3360,
                    neutral_freeze=freeze, xe_freeze=freeze)


if __name__ == "__main__":
    unittest.main()
