"""CPU-only admission and wiring contracts for the AO QA probe."""

from __future__ import annotations

import ast
import math
from pathlib import Path
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests" / "integration" / "run_board_qa.py"


def _helpers():
    tree = ast.parse(RUNNER.read_text(encoding="utf-8"))
    functions = [node for node in tree.body
                 if isinstance(node, ast.FunctionDef) and
                 node.name in {"ao_admission_error", "foreground_ao_error",
                                "validate_foreground_ao"}]
    namespace = {"math": math}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(RUNNER), "exec"), namespace)
    return namespace


class AoAdmissionRunnerTests(unittest.TestCase):
    def setUp(self):
        self.functions = _helpers()
        self.check = self.functions["ao_admission_error"]
        self.foreground_check = self.functions["foreground_ao_error"]
        self.validate_foreground = self.functions["validate_foreground_ao"]

    def request(self, **changes):
        values = dict(
            ao_admission=False,
            ao=2,
            implementation="default",
            ao_cycle=None,
            xegtao_init_failure=False,
            unlocked_fps=True,
            leaf_camera=True,
            scene_camera=None,
            foreground_ao=False,
            water_ao=False,
        )
        values.update(changes)
        return SimpleNamespace(**values)

    def test_absent_and_default_or_legacy_modes_are_admitted(self):
        self.assertIsNone(self.check(self.request()))
        for implementation in ("default", "legacy"):
            with self.subTest(implementation=implementation):
                self.assertIsNone(self.check(self.request(
                    ao_admission=True, implementation=implementation,
                    unlocked_fps=True, leaf_camera=True,
                    scene_camera=[0.0, 1.0, 2.0, 0.0, 0.0, 0.0, 60.0],
                    foreground_ao=True)))

    def test_population_and_lifecycle_constraints_are_rejected(self):
        for changes in (
            {"ao": 0}, {"implementation": "xegtao"},
            {"ao_cycle": 3300}, {"xegtao_init_failure": True},
        ):
            with self.subTest(changes=changes):
                self.assertIsNotNone(self.check(self.request(ao_admission=True, **changes)))

    def test_foreground_and_water_masks_are_not_combined(self):
        self.assertIsNone(self.foreground_check(self.request(foreground_ao=False, water_ao=True)))
        self.assertIsNotNone(self.foreground_check(self.request(foreground_ao=True, water_ao=True)))

    def test_foreground_proof_is_exact_for_xe_and_absent_for_legacy(self):
        marker = "[AO-FOREGROUND] implementation=xegtao mode=premultiplied-rgb\n"
        proof = self.validate_foreground(marker, "default", True)
        self.assertEqual(proof["records"], 1)
        self.assertIsNone(self.validate_foreground("ordinary log\n", "legacy", True))
        self.assertIsNone(self.validate_foreground("ordinary log\n", "default", False))
        cases = (
            ("ordinary log\n", "default", True),
            (marker + marker, "default", True),
            (marker, "default", False),
            ("[AO-FOREGROUND-ERROR] broken\n", "default", True),
        )
        for log, implementation, enabled in cases:
            with self.subTest(log=log, implementation=implementation, enabled=enabled):
                with self.assertRaises(RuntimeError):
                    self.validate_foreground(log, implementation, enabled)

    def test_runner_wires_scrubbed_env_source_gate_and_report(self):
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("parser.add_argument('--ao-admission'", source)
        self.assertIn("env['MP6_QA_AO_ADMISSION'] = '1'", source)
        self.assertIn("ready.get('ao_admission_probe_sha256') != admission_probe_hash", source)
        self.assertIn("from tests.integration.ao_admission_probe import parse_admission_log", source)
        self.assertIn("ao_admission = parse_admission_log(log)", source)
        self.assertIn("report['ao_admission'] = dict(", source)
        self.assertIn("parser.error('--ao-admission requires the isolated board-QA executable')", source)
        self.assertIn("unexpected AO admission probe marker without --ao-admission", source)
        self.assertIn("ready.get('ao_foreground_probe_sha256') != foreground_probe_hash", source)
        self.assertIn("parser.error('--foreground-ao requires the isolated board-QA executable')", source)
        self.assertIn("validate_foreground_ao(", source)
        self.assertIn("report['foreground_ao'] = dict(", source)
        self.assertLess(source.index("not k.startswith('MP6_')"),
                        source.index("env['MP6_QA_AO_ADMISSION']"))
        self.assertLess(source.index("if (error := ao_admission_error(args))"),
                        source.index("directory.mkdir(", source.index("if (error := ao_admission_error(args))")))

    def test_depth_mip_override_excludes_concurrent_admission(self):
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("args.ao_admission", source[source.index("def xegtao_depth_mip_offset_error"):])


if __name__ == "__main__":
    unittest.main(verbosity=2)
