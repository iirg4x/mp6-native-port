"""CPU-only contracts for the prepared-canonical benchmark evidence gate."""

from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/integration"))

from ao_preparation_benchmark import (  # noqa: E402
    CURRENT_XE_STAGES,
    MATRIX_SCHEDULE,
    _board_command,
    aggregate_medians,
    matrix_schedule,
    parse_engine_gpu_block,
    parse_matrix_run,
    read_board_result,
    validate_native_evidence,
    validate_stage_gate,
)

NATIVE_POLICY = (
    "[MP6-AO] requested implementation: XeGTAO (environment override)\n"
    "[MP6-AO] shader/scheduling path: optimized\n"
    "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float\n"
    "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes\n"
)


def _profile():
    return (
        "[FRAME-AO-RECORD-SCOPE] callbacks=complete\n"
        "[FRAME-PROFILE] frames=4000 frameAdmission=0.1 stagingAdmission=0.2 "
        "encode=0.3 aoEncode=0.04 aoCompositeRecord=0.02 aoFoliageSeedRecord=0.01 "
        "acquire=0.1 finish=0.1 submit=0.1 present=0.1 queueDelayMax=0.1\n"
        "[FRAME-WORK] passes=1 gxDraws=2 sourceVertices=3 indices=4 "
        "drawElements=5 copies=6 copyBytes=7\n"
        "[FRAME-PASS] scene\n"
    )


def _gpu(names, header="[ENGINE-GPU] status=2 samples=120 dropped=0 span=10.000000 between=0.100000"):
    return header + "\n" + "\n".join(
        f"[ENGINE-GPU-PASS] {name} avg=1.000000 max=2.000000" for name in names
    )


def _log(condition):
    if condition == "off":
        names = ("EFB",)
        preparation = ""
    elif condition == "viewz":
        names = CURRENT_XE_STAGES + ("XeGTAO canonical depth",)
        preparation = ""
    else:
        names = CURRENT_XE_STAGES
        preparation = "[MP6-AO] XeGTAO preparation: canonical\n"
    return (
        "[ENGINE-BENCH] ticks=4000 game=1.000000 submit=2.000000 seal=0.100000 "
        "post=0.200000 frame=3.000000\n" + (NATIVE_POLICY if condition != "off" else "")
        + preparation + _gpu(names) + "\n" + _profile()
    )


class PreparedCanonicalBenchmark(unittest.TestCase):
    def test_effective_policy_requires_exact_full_high2_r32_before_gpu(self):
        log = _log("canonical")
        policy = parse_matrix_run(log, "canonical")["effective_policy"]
        self.assertEqual(policy["effective_quality"], "high")
        self.assertEqual(policy["effective_resolution"], "full")
        self.assertEqual(policy["actual_depth_storage"], "r32float")
        for bad in (
            log.replace(NATIVE_POLICY, ""),
            log.replace(NATIVE_POLICY, NATIVE_POLICY * 2),
            log.replace("High, 2", "Ultra, 2"),
            log.replace("full-resolution direct", "half-resolution bilateral"),
            log.replace("reference:", "experimental:"),
            log.replace("actual=r32float", "actual=r16float"),
            log.replace("High, 2 spatial denoise passes", "High, 1 spatial denoise pass"),
            log.replace(NATIVE_POLICY, "") + "\n" + NATIVE_POLICY,
            log + "\n[MP6-AO] XeGTAO",
        ):
            with self.subTest(log=bad), self.assertRaises(ValueError):
                parse_matrix_run(bad, "canonical")

    def test_warmup_has_same_native_policy_and_preparation_contract(self):
        for condition in ("off", "viewz", "canonical"):
            log = _log(condition).replace("status=2 samples=120", "status=0 samples=0")
            policy = validate_native_evidence(log, condition)
            self.assertEqual(policy is None, condition == "off")
            if condition != "off":
                with self.assertRaises(ValueError):
                    validate_native_evidence(log.replace("High, 2", "Low, 2"), condition)
        with self.assertRaises(ValueError):
            validate_native_evidence(_log("canonical"), "viewz")

    def test_off_rejects_implementation_path_and_execution_rows(self):
        for row in (
            "[MP6-AO] requested implementation: XeGTAO (environment override)",
            "[MP6-AO] shader/scheduling path: optimized",
            "[MP6-AO] encoded=1 composited=1 foliage=0 xegtao=1",
            '[MP6-GPU] pass="XeGTAO main" avg=1',
        ):
            with self.subTest(row=row), self.assertRaises(ValueError):
                parse_matrix_run(_log("off") + "\n" + row, "off")

    def test_runtime_error_rejection_preserves_only_known_card_miss(self):
        expected = "[AURORA ERROR: aurora::card] Failed to open file: MARIPA6"
        parse_matrix_run(_log("canonical") + "\n" + expected, "canonical")
        for row in (
            "[AURORA ERROR: aurora::gpu] validation failed",
            "[AURORA ERROR: aurora::gpu] Queue submission failed",
            expected + "OTHER",
            "[MP6-CRASH] process failure",
        ):
            with self.subTest(row=row), self.assertRaises(ValueError):
                parse_matrix_run(_log("canonical") + "\n" + row, "canonical")

    def test_missing_complete_callback_scope_is_rejected(self):
        with self.assertRaises(ValueError):
            parse_matrix_run(_log("canonical").replace(
                "[FRAME-AO-RECORD-SCOPE] callbacks=complete\n", ""), "canonical")

    def test_commands_share_current_frame_binary_and_only_canonical_has_opt_in(self):
        class Args:
            startup_ticks = 200000

        commands = {
            condition: _board_command(ROOT, "build/engine-benchmark/current-frame/release/mp6native.exe",
                                      "matrix-" + condition, condition, Args(), timed=True)
            for condition in ("off", "viewz", "canonical")
        }
        self.assertEqual({command[command.index("--exe") + 1]
                          for command in commands.values()},
                         {"build/engine-benchmark/current-frame/release/mp6native.exe"})
        self.assertNotIn("--xegtao-prepared-canonical", commands["off"])
        self.assertNotIn("--xegtao-prepared-canonical", commands["viewz"])
        self.assertEqual(commands["canonical"].count("--xegtao-prepared-canonical"), 1)
        self.assertIn("--gpu-timings", commands["off"])

    def test_schedule_has_symmetric_depth_path_order(self):
        self.assertEqual(MATRIX_SCHEDULE, ("off", "viewz", "canonical", "canonical", "viewz", "off"))
        self.assertEqual(matrix_schedule(2), MATRIX_SCHEDULE * 2)
        with self.assertRaises(ValueError):
            matrix_schedule(0)

    def test_strict_gpu_block_requires_completed_window_and_exact_lines(self):
        parsed = parse_engine_gpu_block(_gpu(("EFB",)))
        self.assertEqual(parsed["sample_count"], 120)
        self.assertEqual(parsed["dropped_frames"], 0)
        for malformed in (
            _gpu(("EFB",), "[ENGINE-GPU] status=2 samples=119 dropped=0 span=10 between=1"),
            _gpu(("EFB",)).replace("avg=1.000000 max=2.000000", "avg=1.000000 max=2.000000 suffix=1"),
            _gpu(("EFB",)) + "\n[ENGINE-GPU-PASS] EFB avg=1 max=2 suffix=1",
        ):
            with self.subTest(log=malformed):
                with self.assertRaises(ValueError):
                    parse_engine_gpu_block(malformed)

    def test_stage_gate_distinguishes_viewz_and_canonical(self):
        viewz = parse_matrix_run(_log("viewz"), "viewz")
        canonical = parse_matrix_run(_log("canonical"), "canonical")
        off = parse_matrix_run(_log("off"), "off")
        self.assertIn("XeGTAO canonical depth", viewz["stage_gate"]["required"])
        self.assertNotIn("XeGTAO canonical depth", canonical["stage_gate"]["required"])
        self.assertEqual(off["named_ao_stage_sum_ms"], 0)
        with self.assertRaises(ValueError):
            parse_matrix_run(_log("canonical").replace(
                "[MP6-AO] XeGTAO preparation: canonical\n", ""), "canonical")
        with self.assertRaises(ValueError):
            parse_matrix_run(_log("viewz").replace(
                "[ENGINE-GPU-PASS] XeGTAO canonical depth avg=1.000000 max=2.000000\n", ""), "viewz")

    def test_canonical_preparation_must_precede_gpu_block(self):
        late = _log("canonical").replace(
            "[MP6-AO] XeGTAO preparation: canonical\n", ""
        )
        late += "\n[MP6-AO] XeGTAO preparation: canonical"
        with self.assertRaises(ValueError):
            parse_matrix_run(late, "canonical")

    def test_off_forbids_native_xegtao_evidence(self):
        with self.assertRaises(ValueError):
            parse_matrix_run(_log("off") + "\n[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float", "off")

    def test_complete_profile_fields_are_reported_separately(self):
        row = parse_matrix_run(_log("canonical"), "canonical")
        self.assertEqual(row["cpu_frame_ms"], 3.0)
        self.assertEqual(row["worker_encode_ms"], 0.3)
        self.assertEqual(row["ao_encode_ms"], 0.04)
        self.assertEqual(row["ao_composite_record_ms"], 0.02)
        self.assertEqual(row["ao_foliage_seed_record_ms"], 0.01)
        self.assertIn("not complete AO cost", row["named_ao_stage_scope"])

    def test_medians_require_all_repeated_condition_rows(self):
        rows = [parse_matrix_run(_log(condition), condition)
                for condition in MATRIX_SCHEDULE * 2]
        medians = aggregate_medians(rows, 2)
        self.assertEqual(medians["off"]["cpu_frame_ms"], 3.0)
        with self.assertRaises(ValueError):
            aggregate_medians(rows[:-1], 2)

    def test_board_result_hash_and_completion_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "result.json").write_text(
                '{"sha256":"' + "a" * 64 + '","exit_code":0,"reason":"exited",'
                '"crash_marker":false,"capture_count":0}\n', encoding="utf-8"
            )
            self.assertEqual(read_board_result(path)["sha256"], "a" * 64)
            (path / "result.json").write_text(
                '{"sha256":"' + "a" * 64 + '","exit_code":0,"reason":"wall-clock limit",'
                '"crash_marker":false,"capture_count":0}\n', encoding="utf-8"
            )
            with self.assertRaises(RuntimeError):
                read_board_result(path)


if __name__ == "__main__":
    unittest.main()
