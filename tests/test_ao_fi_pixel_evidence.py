"""CPU-only contract tests for the AO/FI native pixel evidence loader."""

from __future__ import annotations

import csv
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
import ao_fi_pixel_evidence as evidence  # noqa: E402


INDEX_FIELDS = [
    "seq", "present_index", "tick", "wall_ns", "replay", "width", "height",
    "src_width", "src_height", "crop_x", "crop_y", "crop_w", "crop_h",
    "downscale", "format",
]
EXE_SHA = "e" * 64
PIXEL_PROBE_SHA = "a" * 64
MATRIX_PROBE_SHA = "b" * 64


def _request(unshaded: bool, schedule: str, *, implementation: str = "default",
             quality: str = "high", denoise: int = 2) -> dict:
    start, end, stride = evidence.parse_schedule(schedule)
    return {
        "name": "fixture-unshaded" if unshaded else "fixture-shaded",
        "exe": "build/mp6native.exe",
        "cache_seed": "per-run-cache",
        "capsule": -1,
        "input_script": "fixture:pan",
        "route": "",
        "window_size": "1024x768",
        "widescreen": False,
        "results_geometry": False,
        "ao": 2,
        "implementation": implementation,
        "xegtao_quality": quality,
        "xegtao_denoise": denoise,
        "xegtao_resolution": "full",
        "xegtao_depth_storage": "r32float",
        "leaf_camera": True,
        "leaf_camera_mode": "pan",
        "leaf_freeze": True,
        "mobile_ao": False,
        "ao_cycle": None,
        "ao_resources": False,
        "resize_ao": False,
        "ao_fi_matrix_range": f"{start}:{end}",
        "ao_fi_matrix_limit": 256,
        "ao_fi_pixels": schedule,
        "unshaded_ao": unshaded,
        "raw_ao_depth": False,
        "uncached_settings": False,
        "frame_upload_mode": None,
        "foreground_ao": False,
        "water_ao": False,
        "legacy_foliage_viewport": False,
        "foliage_no_depth": False,
        "geometry_at": None,
        "geometry_at_second": None,
        "no_grounding": False,
        "no_lawn_lift": False,
        "no_connector_lift": False,
        "no_log_grounding": False,
        "no_tree_grounding": False,
        "no_foliage_mask": False,
        "legacy_foliage_blend": False,
        "aa": 0,
        "shadow_quality": 1,
        "unlocked_fps": True,
        "capture_delay": 0,
        "capture_frames": (end - start) // stride + 1,
        "capture_stride": 1,
        "capture_rect": [10, 20, 16, 8],
        "capture_trigger": "-",
        "tick_hz": 60,
        "ticks": end + 1,
    }


def _proof(seq: int, present: int, capture_tick: int, vi_tick: int,
           start: int, stride: int, camera_delta: int = 0,
           output_delta: int = 0) -> dict:
    phase = ((vi_tick - start) // stride) % 4
    key = {"valid": 1, "camera": 0, "model": 1, "generation": 2,
           "sub": 16384, "ordinal": 0}
    matrix = {
        "index": 7,
        "object": 3,
        "name": "fixture_matrix",
        "key": dict(key),
        "previousKey": dict(key),
        "currentBits": list(range(100, 112)),
        "previousBits": list(range(200, 212)),
        "outputBits": [value + output_delta for value in range(300, 312)],
        "cameraViewBits": list(range(400, 412)),
        "cameraBits": [value + camera_delta for value in range(500, 513)],
        "submitted": True,
    }
    return {
        "seq": seq,
        "presentIndex": present,
        "captureViTick": capture_tick,
        "viTick": vi_tick,
        "previousViTick": vi_tick - 1,
        "seal": capture_tick,
        "previousSeal": capture_tick - 1,
        "alpha": phase / 4.0,
        "interpolationEnabled": 1,
        "matrixCount": 1,
        "hintOverflow": 0,
        "previousHintOverflow": 0,
        "matrices": [matrix],
    }


def _policy_lines(implementation: str, quality: str, denoise: int,
                  encoded: int, *, storage: str = "r32float") -> list[str]:
    source = "platform default" if implementation == "default" else "environment override"
    kind = "reference" if quality == "high" and denoise == 2 else "experimental"
    suffix = "" if denoise == 1 else "es"
    return [
        f"[MP6-AO] requested implementation: XeGTAO ({source})",
        f"[MP6-AO] XeGTAO {kind}: full-resolution direct uint output, "
        f"{quality.title()}, {denoise} spatial denoise pass{suffix}",
        f"[MP6-AO] XeGTAO depth storage: requested={storage} actual={storage}",
        f"[MP6-AO] encoded={encoded} composited={encoded} foliage=0 xegtao={encoded}",
    ]


def _write_run(root: Path, *, unshaded: bool, schedule: str = "3300:3311:1",
               replay: int = 1, sha: str = EXE_SHA, camera_delta: int = 0,
               duplicate_first_proof: bool = False,
               csv_width_delta: int = 0, output_delta: int = 0,
               pixel_probe_sha: str = PIXEL_PROBE_SHA,
               matrix_probe_sha: str = MATRIX_PROBE_SHA,
               implementation: str = "default", quality: str = "high",
               denoise: int = 2, include_effective_policy: bool = False) -> Path:
    frames = root / "frames"
    frames.mkdir(parents=True)
    start, end, stride = evidence.parse_schedule(schedule)
    request = _request(unshaded, schedule, implementation=implementation,
                       quality=quality, denoise=denoise)
    rows = []
    proof_lines = []
    for seq, vi_tick in enumerate(range(start, end + 1, stride)):
        present = 1000 + seq
        capture_tick = vi_tick + 1
        row = {
            "seq": seq, "present_index": present, "tick": capture_tick,
            "wall_ns": 2_000_000 + seq, "replay": replay, "width": 4,
            "height": 2, "src_width": 32, "src_height": 32, "crop_x": 10,
            "crop_y": 20, "crop_w": 16, "crop_h": 8, "downscale": 4,
            "format": 3,
        }
        rows.append(row)
        header = bytearray(evidence.MFD_HEADER_BYTES)
        header[:8] = evidence.MFD_MAGIC
        struct.pack_into(
            "<8I", header, 8, evidence.MFD_VERSION, row["width"], row["height"],
            evidence.MFD_BPP, row["format"],
            row["seq"], row["present_index"], row["replay"],
        )
        struct.pack_into("<2Q", header, 40, row["tick"], row["wall_ns"])
        struct.pack_into(
            "<7I", header, 56, row["src_width"], row["src_height"], row["crop_x"],
            row["crop_y"], row["crop_w"], row["crop_h"], row["downscale"],
        )
        (frames / f"f{seq:06d}.mfd").write_bytes(bytes(header) + bytes(range(32)))
        proof = _proof(seq, present, capture_tick, vi_tick, start, stride,
                       camera_delta=camera_delta, output_delta=output_delta)
        proof_lines.append("[AO-FI-PIXEL] " + json.dumps(proof, separators=(",", ":")))
    if duplicate_first_proof:
        proof_lines.append(proof_lines[0])
    if implementation == "xegtao" or include_effective_policy:
        proof_lines[0:0] = _policy_lines(implementation, quality, denoise, len(rows))
    (root / "game.log").write_text("\n".join(proof_lines) + "\n", encoding="utf-8")
    with (frames / "index.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=INDEX_FIELDS, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            row = dict(row)
            row["width"] += csv_width_delta
            writer.writerow(row)
    result = {
        "request": request,
        "sha256": sha,
        "reason": "exited",
        "exit_code": 0,
        "crash_marker": False,
        "capture_count": len(rows),
        "ao_fi_matrix": {"probe_sha256": matrix_probe_sha},
        "ao_fi_pixels": {
            "schedule": schedule,
            "phases": list(evidence.FIXED_PHASES),
            "probe_sha256": pixel_probe_sha,
            "scope": "Sampled fixed-phase native replay pixels; not natural cadence or performance evidence",
        },
        "default_selection": {
            "implementation": "default",
            "selection": "platform-default",
            "reference": "High2-full-r32",
            "markers": {
                "selection": "[MP6-AO] requested implementation: XeGTAO (platform default)",
                "reference": "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes",
                "depth_storage": "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float",
            },
            "counters": {"encoded": len(rows), "composited": len(rows), "foliage": 0, "xegtao": len(rows)},
            "counter_records": 1,
            "exit_code": 0,
            "reason": "exited",
        },
    }
    if include_effective_policy:
        result["ao_fi_pixels"]["effective_policy"] = evidence.validate_ao_fi_pixel_policy(
            "\n".join(proof_lines), implementation, quality, denoise, 0, "exited")
    (root / "result.json").write_text(json.dumps(result), encoding="utf-8")
    return root


class AoFiPixelEvidenceTests(unittest.TestCase):
    def test_known_valid_paired_fixture_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "shaded", unshaded=False)
            right = _write_run(base / "unshaded", unshaded=True)
            shaded = evidence.load_capture(left)
            unshaded = evidence.load_capture(right)
            self.assertEqual(shaded["captured_schedule_ticks"], 12)
            self.assertEqual(shaded["unmatched_schedule_ticks"], 0)
            self.assertEqual(shaded["pairs"][0]["proof"]["matrices"][0]["currentBits"],
                             list(range(100, 112)))
            report = evidence.pair_captures(shaded, unshaded)
            self.assertEqual(report["shared_ticks"], 12)
            self.assertEqual(report["shared_phase_pairs"], 12)
            self.assertEqual(report["shared_phases"], list(evidence.FIXED_PHASES))
            self.assertEqual(len(report["pairs"]), 12)
            self.assertTrue(report["pairs"][0]["left_frame"].endswith("f000000.mfd"))
            self.assertIn("not pixel quality", report["scope"])

    def test_explicit_xegtao_full_r32_quality_and_denoise_policies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            for quality in ("low", "medium", "high", "ultra"):
                for denoise in (1, 2, 3):
                    with self.subTest(quality=quality, denoise=denoise):
                        run = _write_run(
                            base / f"{quality}-{denoise}", unshaded=False,
                            schedule="3300:3303:1", implementation="xegtao",
                            quality=quality, denoise=denoise,
                            include_effective_policy=True)
                        capture = evidence.load_capture(run)
                        self.assertEqual(capture["effective_policy"]["quality"], quality)
                        self.assertEqual(capture["effective_policy"]["denoise_passes"], denoise)
                        self.assertEqual(capture["effective_policy"]["depth_storage"], "r32float")

    def test_missing_explicit_policy_proof_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False, implementation="xegtao",
                             quality="ultra", denoise=3)
            with self.assertRaisesRegex(ValueError, "missing effective_policy"):
                evidence.load_capture(run)

    def test_metadata_without_actual_policy_log_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False, implementation="xegtao",
                             quality="ultra", denoise=3, include_effective_policy=True)
            log_path = run / "game.log"
            log = log_path.read_text(encoding="utf-8")
            log_path.write_text("\n".join(
                line for line in log.splitlines()
                if not line.startswith("[MP6-AO]")
            ) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "implementation selection"):
                evidence.load_capture(run)

    def test_effective_policy_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False, implementation="xegtao",
                             quality="ultra", denoise=3, include_effective_policy=True)
            result_path = run / "result.json"
            result = json.loads(result_path.read_text(encoding="utf-8"))
            result["ao_fi_pixels"]["effective_policy"]["quality"] = "high"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "effective_policy"):
                evidence.load_capture(run)

    def test_effective_policy_storage_fallback_rejected(self) -> None:
        for replacement, expected in (
            ("[MP6-AO] XeGTAO depth storage: requested=r32float actual=r16float",
             "depth storage"),
            ("fallback: legacy AO", "fallback"),
        ):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temporary:
                run = _write_run(Path(temporary), unshaded=False, implementation="xegtao",
                                 quality="ultra", denoise=3, include_effective_policy=True)
                log_path = run / "game.log"
                log = log_path.read_text(encoding="utf-8")
                if expected == "depth storage":
                    log = log.replace(
                        "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float",
                        replacement)
                else:
                    log = replacement + "\n" + log
                log_path.write_text(log, encoding="utf-8")
                with self.assertRaisesRegex((RuntimeError, ValueError), expected):
                    evidence.load_capture(run)

    def test_default_ultra_and_legacy_requests_rejected(self) -> None:
        for implementation, quality, expected in (
            ("default", "ultra", "default AO FI pixel request"),
            ("legacy", "high", "implementation"),
        ):
            with self.subTest(implementation=implementation), tempfile.TemporaryDirectory() as temporary:
                run = _write_run(Path(temporary), unshaded=False,
                                 implementation=implementation, quality=quality)
                with self.assertRaisesRegex(ValueError, expected):
                    evidence.load_capture(run)

    def test_invalid_explicit_request_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False, implementation="xegtao",
                             quality="ultra", denoise=3, include_effective_policy=True)
            result_path = run / "result.json"
            result = json.loads(result_path.read_text(encoding="utf-8"))
            result["request"]["xegtao_resolution"] = "half"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "xegtao_resolution"):
                evidence.load_capture(run)

    def test_pair_ignores_run_specific_policy_counts_but_rejects_semantic_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = evidence.load_capture(_write_run(
                base / "left", unshaded=False, implementation="xegtao",
                quality="ultra", denoise=3, include_effective_policy=True))
            right = evidence.load_capture(_write_run(
                base / "right", unshaded=True, implementation="xegtao",
                quality="ultra", denoise=3, include_effective_policy=True))
            right["effective_policy"] = dict(right["effective_policy"])
            right["effective_policy"]["encoded_passes"] = 2
            report = evidence.pair_captures(left, right)
            self.assertEqual(report["effective_policy"]["denoise_passes"], 3)

            right["effective_policy"]["quality"] = "high"
            with self.assertRaisesRegex(ValueError, "different effective AO FI policies"):
                evidence.pair_captures(left, right)

    def test_duplicate_proof_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                evidence.load_capture(_write_run(Path(temporary), unshaded=False,
                                                  duplicate_first_proof=True))

    def test_header_csv_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, "MFD/CSV metadata mismatch"):
                evidence.load_capture(_write_run(Path(temporary), unshaded=False,
                                                  csv_width_delta=1))

    def test_pose_mismatch_rejected_for_common_phase(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "left", unshaded=False)
            right = _write_run(base / "right", unshaded=True, camera_delta=1)
            with self.assertRaisesRegex(ValueError, "camera/matrix bits mismatch"):
                evidence.pair_captures(left, right)

    def test_output_bits_mismatch_rejected_for_common_phase(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "left", unshaded=False)
            right = _write_run(base / "right", unshaded=True, output_delta=1)
            with self.assertRaisesRegex(ValueError, "camera/matrix bits mismatch"):
                evidence.pair_captures(left, right)

    def test_disjoint_phase_schedule_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "left", unshaded=False)
            right = _write_run(base / "right", unshaded=True, schedule="3400:3411:1")
            with self.assertRaisesRegex(ValueError, "fixture inputs or phase schedule"):
                evidence.pair_captures(left, right)

    def test_non_replay_frame_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, "requires replay=1"):
                evidence.load_capture(_write_run(Path(temporary), unshaded=False, replay=0))

    def test_cross_run_hash_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "left", unshaded=False, sha="c" * 64)
            right = _write_run(base / "right", unshaded=True, sha="d" * 64)
            with self.assertRaisesRegex(ValueError, "different executable sha256"):
                evidence.pair_captures(left, right)

    def test_pixel_error_marker_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False)
            log = run / "game.log"
            log.write_text("[AO-FI-PIXEL-ERROR] rejected capture\n" +
                           log.read_text(encoding="utf-8"), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "AO-FI-PIXEL-ERROR"):
                evidence.load_capture(run)

    def test_missing_probe_hash_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False)
            result_path = run / "result.json"
            result = json.loads(result_path.read_text(encoding="utf-8"))
            del result["ao_fi_pixels"]["probe_sha256"]
            result_path.write_text(json.dumps(result), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "ao_fi_pixels.probe_sha256"):
                evidence.load_capture(run)

    def test_different_probe_hash_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            left = _write_run(base / "left", unshaded=False)
            right = _write_run(base / "right", unshaded=True, pixel_probe_sha="c" * 64)
            with self.assertRaisesRegex(ValueError, "different ao_fi_pixels_probe_sha256"):
                evidence.pair_captures(left, right)

    def test_fallback_selection_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False)
            result_path = run / "result.json"
            result = json.loads(result_path.read_text(encoding="utf-8"))
            result["default_selection"]["reference"] = "Legacy"
            result_path.write_text(json.dumps(result), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "High2-full-r32"):
                evidence.load_capture(run)

    def test_invalid_or_mismatched_previous_key_rejected(self) -> None:
        for mutation, expected in (
            (lambda row: row["matrices"][0]["key"].update(valid=0), "keys must both be valid"),
            (lambda row: row["matrices"][0]["previousKey"].update(sub=1),
             "previousKey does not equal key"),
        ):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temporary:
                run = _write_run(Path(temporary), unshaded=False)
                log_path = run / "game.log"
                lines = log_path.read_text(encoding="utf-8").splitlines()
                row = json.loads(lines[0].split("[AO-FI-PIXEL]", 1)[1].strip())
                mutation(row)
                lines[0] = "[AO-FI-PIXEL] " + json.dumps(row, separators=(",", ":"))
                log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, expected):
                    evidence.load_capture(run)

    def test_capture_count_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = _write_run(Path(temporary), unshaded=False)
            result_path = run / "result.json"
            result = json.loads(result_path.read_text(encoding="utf-8"))
            result["capture_count"] += 1
            result_path.write_text(json.dumps(result), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "capture_count"):
                evidence.load_capture(run)


if __name__ == "__main__":
    unittest.main()
