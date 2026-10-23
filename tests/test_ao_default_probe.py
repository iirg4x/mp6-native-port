"""CPU-only contracts for the platform-default XeGTAO board-QA mode."""

from __future__ import annotations

import ast
from pathlib import Path
import unittest

from tests.integration.ao_default_probe import (
    DEFAULT_DEPTH_STORAGE_MARKER,
    DEFAULT_REFERENCE_MARKER,
    DEFAULT_SELECTION_MARKER,
    default_ao_error,
    validate_default_ao,
)


MODULE_PATH = Path(__file__).resolve().parents[1] / "tests" / "integration" / "ao_default_probe.py"
RUNNER_PATH = Path(__file__).resolve().parents[1] / "tests" / "integration" / "run_board_qa.py"


def _valid_log(*, counters="encoded=12 composited=12 foliage=3 xegtao=12"):
    return "\n".join((
        DEFAULT_SELECTION_MARKER,
        DEFAULT_DEPTH_STORAGE_MARKER,
        DEFAULT_REFERENCE_MARKER,
        f"[MP6-AO] {counters}",
    )) + "\n"


class DefaultXeGTAOProbeTests(unittest.TestCase):
    def test_import_surface_is_cpu_only(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        tree = ast.parse(source)
        imported = {
            alias.name.split(".", 1)[0]
            for node in tree.body
            if isinstance(node, (ast.Import, ast.ImportFrom))
            for alias in node.names
        }
        self.assertNotIn("numpy", imported)
        self.assertNotIn("wgpu", imported)
        self.assertNotIn("subprocess", imported)

    def test_runner_records_default_selection_without_importing_or_launching(self):
        source = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn("from tests.integration.ao_default_probe import", source)
        self.assertIn("elif args.implementation == 'default':", source)
        self.assertIn("default_selection = validate_default_ao(log, result, reason)", source)
        self.assertIn("report['default_selection'] = default_selection", source)

    def test_valid_platform_default_reference_evidence(self):
        result = validate_default_ao(_valid_log())
        self.assertEqual(result["selection"], "platform-default")
        self.assertEqual(result["reference"], "High2-full-r32")
        self.assertEqual(result["counter_records"], 1)
        self.assertEqual(result["counters"]["encoded"], 12)
        self.assertEqual(result["counters"]["composited"], 12)
        self.assertEqual(result["counters"]["xegtao"], 12)

    def test_default_mode_rejects_nonreference_policy_values(self):
        self.assertIsNone(default_ao_error("legacy", 0, "ultra", 3, "half", "r16float"))
        self.assertIn("--ao", default_ao_error("default", 0, "high", 2, "full", "r32float"))
        self.assertIn("--xegtao-quality", default_ao_error("default", 2, "ultra", 2, "full", "r32float"))
        self.assertIn("--xegtao-denoise", default_ao_error("default", 2, "high", 3, "full", "r32float"))
        self.assertIn("--xegtao-resolution", default_ao_error("default", 2, "high", 2, "half", "r32float"))
        self.assertIn("--xegtao-depth-storage", default_ao_error("default", 2, "high", 2, "full", "r16float"))
        self.assertIsNone(default_ao_error("default", 2, "high", 2, "full", "r32float"))

    def test_missing_selection_or_reference_markers_fail(self):
        for marker in (DEFAULT_SELECTION_MARKER, DEFAULT_REFERENCE_MARKER,
                       DEFAULT_DEPTH_STORAGE_MARKER):
            log = _valid_log().replace(marker + "\n", "")
            with self.assertRaises(RuntimeError):
                validate_default_ao(log)

    def test_duplicate_shutdown_or_disagreeing_counters_fail(self):
        with self.assertRaises(RuntimeError):
            validate_default_ao(_valid_log() + "[MP6-AO] encoded=12 composited=12 foliage=3 xegtao=12\n")
        with self.assertRaises(RuntimeError):
            validate_default_ao(_valid_log(counters="encoded=12 composited=11 foliage=3 xegtao=12"))
        with self.assertRaises(RuntimeError):
            validate_default_ao(_valid_log(counters="encoded=0 composited=0 foliage=0 xegtao=0"))

    def test_error_fallback_and_unclean_termination_fail(self):
        for marker in ("[AURORA ERROR] broken", "[MP6-AO] XeGTAO unavailable: using legacy",
                       "fallback to legacy"):
            with self.assertRaises(RuntimeError):
                validate_default_ao(_valid_log() + marker + "\n")
        with self.assertRaises(RuntimeError):
            validate_default_ao(_valid_log(), exit_code=1)
        with self.assertRaises(RuntimeError):
            validate_default_ao(_valid_log(), reason="wall-clock limit")

    def test_malformed_counter_is_not_accepted_as_clean_evidence(self):
        log = _valid_log().replace(
            "[MP6-AO] encoded=12 composited=12 foliage=3 xegtao=12",
            "[MP6-AO] encoded=12 composited=12 foliage=3",
        )
        with self.assertRaises(ValueError):
            validate_default_ao(log)


if __name__ == "__main__":
    unittest.main()
