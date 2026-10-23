"""Focused contract tests for the bounded desktop AO pipeline report."""

from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

import ao_desktop_pipeline_report as report


def _log(*, condition="on", aa=1, positive=True, seed=True, composite=True):
    game = "0.020000 0.010000 0.012000 0.006000 1.000 1.000" if positive else "0 0 0 0 0 0"
    lines = [
        "[ENGINE-BENCH] ticks=4000 game=1.200000 submit=0.300000 seal=0.020000 post=0.010000 frame=1.530000",
        "[FRAME-PROFILE] frames=4000 frameAdmission=0.000100 stagingAdmission=0.000100 encode=0.200000 aoEncode=0.010000 aoCompositeRecord=0.002000 aoFoliageSeedRecord=0.003000 acquire=0.005000 finish=0.001000 submit=0.200000 present=0.100000 queueDelayMax=0.020000",
        "[FRAME-AO-RECORD-SCOPE] callbacks=complete",
        f"[FRAME-AO-GAME] renderInclusive={game.split()[0]} renderExclusive={game.split()[1]} faceDrawInclusive={game.split()[2]} faceDrawExclusive={game.split()[3]} renderCalls={game.split()[4]} faceDrawCalls={game.split()[5]}",
        "[FRAME-WORK] passes=4.000 gxDraws=20.000 sourceVertices=100.000 indices=200.000 drawElements=300.000 copies=2.000 copyBytes=4096.000",
        "[FRAME-PASS] scene",
    ]
    lines.append("[ENGINE-GPU] status=2 samples=120 dropped=0 span=1.800000 between=0.200000")
    if condition == "on":
        lines[0:0] = [
            "[MP6-AO] requested implementation: XeGTAO (environment override)",
            "[MP6-AO] shader/scheduling path: optimized",
            "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float",
            "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes",
            "[AURORA INFO: aurora::gpu] [MP6-INLINE-AO] feature=supported",
            "[MP6-INLINE-AO] scope=nested-efb timestamps=supported",
        ]
        rows = [
            ("AO depth snapshot", 0.10), ("AO foreground mask", 0.20),
            ("AO depth preparation", 0.30), ("XeGTAO canonical depth", 0.40),
            ("XeGTAO depth MIPs", 0.50), ("XeGTAO normals", 0.60),
            ("XeGTAO main", 0.70), ("XeGTAO denoise", 0.80),
            ("XeGTAO final denoise", 0.90),
        ]
        lines.extend(f"[ENGINE-GPU-PASS] {name} avg={value:.6f} max={value + 0.1:.6f}"
                     for name, value in rows)
        if composite:
            lines.append("[ENGINE-GPU-PASS] AO composite inline avg=0.050000 max=0.150000")
        if aa == 1 and seed:
            lines.append("[ENGINE-GPU-PASS] AO foliage seed inline avg=0.060000 max=0.160000")
    else:
        lines.extend([
            "[ENGINE-GPU-PASS] EFB avg=1.000000 max=1.100000",
            "[ENGINE-GPU-PASS] Present + touch UI avg=0.100000 max=0.200000",
        ])
    return "\n".join(lines) + "\n"


class DesktopAoPipelineReportTests(unittest.TestCase):
    def test_schedule_and_command_contract(self):
        self.assertEqual(
            report.schedule_names("ao-cost-verified-20261006", 0),
            tuple("ao-cost-verified-20261006-aa0-" + token for token in report.SCHEDULE),
        )
        command = report.board_command(Path("engine.exe"), "p-aa1-on-1", 1, "on")
        self.assertIn("--gpu-timings", command)
        self.assertIn("--stop-round", command)
        self.assertEqual(command[command.index("--stop-round") + 1], "0")
        self.assertEqual(command[command.index("--ticks") + 1], "200000")
        self.assertEqual(command[command.index("--implementation") + 1], "xegtao")
        self.assertEqual(command[command.index("--aa") + 1], "1")
        self.assertNotIn("--scene-camera", command)
        warmup = report.board_command(Path("engine.exe"), "p-aa1-on-warmup", 1, "on")
        measured = report.board_command(
            Path("engine.exe"), "p-aa1-on-1", 1, "on",
            cache_seed="build/board-qa-runs/p-aa1-on-warmup/gpu-cache",
        )
        self.assertNotIn("--cache-seed", warmup)
        self.assertIn("--cache-seed", measured)

    def test_on_rows_and_nested_accounting(self):
        parsed = report.parse_run_log(_log(), aa=1, condition="on")
        self.assertEqual((parsed["gpu_status"], parsed["gpu_sample_count"],
                          parsed["gpu_dropped_frames"]), (2, 120, 0))
        self.assertIn("AO depth snapshot", parsed["gpu_passes"])
        self.assertIn("AO foreground mask", parsed["gpu_passes"])
        self.assertTrue(parsed["gpu_passes"]["AO composite inline"]["nested"])
        self.assertEqual(parsed["gpu_passes"]["AO foliage seed inline"]["nested_in"],
                         "AO foreground mask")
        expected = sum(parsed["gpu_passes"][name]["average_ms"]
                       for name in report.INDEPENDENT_GPU_ROWS)
        self.assertAlmostEqual(parsed["independent_ao_gpu_sum_ms"], expected)
        self.assertNotAlmostEqual(parsed["independent_ao_gpu_sum_ms"],
                                  expected + 0.05 + 0.06)

    def test_medians_retain_game_fields_components_and_same_aa_delta(self):
        on_first = report.parse_run_log(_log(), aa=1, condition="on")
        on_second = copy.deepcopy(on_first)
        on_second["cpu_frame_ms"] += 0.2
        on_second["ao_game_average"]["renderInclusive"] += 0.004
        on_second["gpu_all_passes"]["XeGTAO main"]["average_ms"] += 0.2
        on_median = report._median_rows([on_first, on_second])

        off_first = report.parse_run_log(_log(condition="off", positive=False),
                                         aa=1, condition="off")
        off_second = copy.deepcopy(off_first)
        off_median = report._median_rows([off_first, off_second])

        self.assertEqual(set(report.AO_GAME_FIELDS),
                         set(on_median["ao_game_average"]))
        self.assertAlmostEqual(on_median["ao_game_render_inclusive_ms"], 0.022)
        self.assertAlmostEqual(
            on_median["gpu_pass_medians"]["AO composite inline"]["average_ms"],
            0.05)
        self.assertIn("AO foliage seed inline", on_median["gpu_nested_rows"])

        delta = report._same_aa_on_minus_off(on_median, off_median)
        self.assertAlmostEqual(delta["ao_game_average"]["renderInclusive"], 0.022)
        self.assertAlmostEqual(
            delta["gpu_pass_deltas"]["XeGTAO main"]["average_ms"], 0.8)
        self.assertIn("AO composite inline", delta["gpu_nested_pass_deltas"])
        self.assertIn("AO-on minus AO-off", delta["scope"])

    def test_aa0_does_not_require_seed_but_does_require_composite(self):
        parsed = report.parse_run_log(_log(aa=0, seed=False), aa=0, condition="on")
        self.assertNotIn("AO foliage seed inline", parsed["gpu_passes"])
        with self.assertRaisesRegex(ValueError, "AA0.*seed"):
            report.parse_run_log(
                _log(aa=0, seed=False) +
                "[ENGINE-GPU-PASS] AO foliage seed inline avg=0.060000 max=0.160000\n",
                aa=0, condition="on")
        with self.assertRaisesRegex(ValueError, "composite"):
            report.parse_run_log(_log(aa=0, seed=False, composite=False), aa=0, condition="on")

    def test_missing_aa1_seed_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "AA1.*seed"):
            report.parse_run_log(_log(aa=1, seed=False), aa=1, condition="on")

    def test_all_zero_on_game_marker_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "renderCalls"):
            report.parse_run_log(_log(positive=False), aa=1, condition="on")

    def test_unbalanced_game_scope_marker_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unbalanced"):
            report.parse_run_log(_log() + "[FRAME-AO-GAME-ERROR] unbalanced scope\\n",
                                 aa=1, condition="on")

    def test_off_still_requires_marker_and_rejects_ao_rows(self):
        parsed = report.parse_run_log(_log(condition="off", positive=False), aa=0, condition="off")
        self.assertEqual(parsed["ao_game_average"]["renderCalls"], 0)
        missing_marker = _log(condition="off", positive=False).replace("[FRAME-AO-GAME]", "[FRAME-AO-GAME-MISSING]")
        with self.assertRaisesRegex(ValueError, "ao_game_average"):
            report.parse_run_log(missing_marker, aa=0, condition="off")
        with self.assertRaisesRegex(ValueError, "AO GPU rows"):
            report.parse_run_log(_log(condition="off", positive=False) +
                                 "[ENGINE-GPU-PASS] AO depth snapshot avg=0.1 max=0.2\n",
                                 aa=0, condition="off")
        with self.assertRaisesRegex(ValueError, "AO GPU rows"):
            report.parse_run_log(_log(condition="off", positive=False) +
                                 "[ENGINE-GPU-PASS] GTAO avg=0.1 max=0.2\n",
                                 aa=0, condition="off")

    def test_gpu_dropped_frames_are_rejected(self):
        bad = _log().replace("samples=120 dropped=0", "samples=120 dropped=1")
        with self.assertRaisesRegex(ValueError, "dropped"):
            report.parse_run_log(bad, aa=1, condition="on")

    def test_request_guard_enforces_exact_recipe_and_matching_seed(self):
        request = {
            "name": "p-aa1-on-1", "aa": 1, "seconds": 90, "ticks": 200000,
            "stop_round": 0, "widescreen": True, "window_size": "1920x1080",
            "ao": 2, "implementation": "xegtao", "xegtao_quality": "high",
            "xegtao_denoise": 2, "xegtao_resolution": "full",
            "xegtao_depth_storage": "r32float", "gpu_timings": True,
            "input_script": report.INPUT_SCRIPT, "scene_camera": None,
            "capsule": -1, "route": "", "xegtao_depth_mip_offset": None,
            "leaf_camera_mode": "pan", "ao_fi_matrix_limit": 256,
            "shadow_quality": 1, "tick_hz": 0, "capture_stride": 4,
            "capture_trigger": "capsule.begin",
            "cache_seed": "build/board-qa-runs/p-aa1-on-warmup/gpu-cache",
        }
        report._request_guard(
            request, executable=Path("engine.exe"), name="p-aa1-on-1", aa=1,
            condition="on", warmup=False,
            cache_seed=report.ROOT / "build/board-qa-runs/p-aa1-on-warmup/gpu-cache",
        )
        request["stop_round"] = 7
        with self.assertRaisesRegex(RuntimeError, "stop_round"):
            report._request_guard(
                request, executable=Path("engine.exe"), name="p-aa1-on-1", aa=1,
                condition="on", warmup=False, cache_seed="seed",
            )


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
