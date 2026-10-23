"""CPU-only admission and runtime-proof contracts for the MIP-offset runner."""

from __future__ import annotations

import ast
import math
from pathlib import Path
import re
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests" / "integration" / "run_board_qa.py"


def _helpers():
    source = RUNNER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    wanted = {
        "xegtao_depth_mip_offset_error",
        "validate_xegtao_depth_mip_offset",
        "validate_xegtao_mip_offset_runtime",
    }
    body = [node for node in tree.body if isinstance(node, (ast.Import, ast.ImportFrom))]
    body.extend(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id.startswith("_AO_")
                for target in node.targets)
    )
    body.extend(node for node in tree.body
                if isinstance(node, ast.FunctionDef) and node.name in wanted)
    namespace = {"math": math, "re": re}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(RUNNER), "exec"), namespace)
    return namespace


class MipOffsetRunnerTests(unittest.TestCase):
    def setUp(self):
        self.functions = _helpers()

    def request(self, **changes):
        values = dict(
            xegtao_depth_mip_offset=None, ao=2, implementation="default",
            xegtao_init_failure=False, xegtao_prepared_canonical=False,
            ao_cycle=None, frame_upload_mode=None, mobile_ao=False,
            ao_resources=False, resize_ao=False, raw_ao_depth=False,
            foreground_ao=False, water_ao=False, legacy_foliage_viewport=False,
            foliage_no_depth=False, no_grounding=False, no_lawn_lift=False,
            no_connector_lift=False, no_log_grounding=False, no_tree_grounding=False,
            no_foliage_mask=False, legacy_foliage_blend=False,
            unshaded_ao=False, ao_fi_pixels=None,
        )
        values.update(changes)
        return SimpleNamespace(**values)

    def test_absent_and_valid_values_are_admitted(self):
        check = self.functions["xegtao_depth_mip_offset_error"]
        self.assertIsNone(check(self.request()))
        self.assertIsNone(check(self.request(xegtao_depth_mip_offset=2.0)))
        self.assertIsNone(check(self.request(
            xegtao_depth_mip_offset=6.0, implementation="xegtao",
            unshaded_ao=True, ao_fi_pixels="3300:3420:6")))

    def test_nonfinite_and_out_of_range_values_are_rejected(self):
        check = self.functions["xegtao_depth_mip_offset_error"]
        for value in (float("nan"), float("inf"), float("-inf"), 1.999, 6.001, "offset", True):
            with self.subTest(value=value):
                self.assertIsNotNone(check(self.request(xegtao_depth_mip_offset=value)))

    def test_population_and_diagnostic_conflicts_are_rejected(self):
        check = self.functions["xegtao_depth_mip_offset_error"]
        for changes in (
            {"ao": 0}, {"implementation": "legacy"},
            {"xegtao_init_failure": True}, {"xegtao_prepared_canonical": True},
            {"ao_cycle": 3300}, {"frame_upload_mode": 1}, {"raw_ao_depth": True},
            {"foreground_ao": True}, {"water_ao": True}, {"no_grounding": True},
            {"resize_ao": True}, {"ao_resources": True},
        ):
            with self.subTest(changes=changes):
                self.assertIsNotNone(check(self.request(xegtao_depth_mip_offset=3.0, **changes)))

    def test_marker_proof_requires_one_exact_runtime_row(self):
        check = self.functions["validate_xegtao_depth_mip_offset"]
        valid = "[AO-MIP-OFFSET] requested=3.25 applied=3.25\n"
        proof = check(valid, 3.25)
        self.assertEqual(proof["records"], 1)
        self.assertAlmostEqual(proof["applied"], 3.25)
        self.assertIsNone(check("ordinary log\n", None))
        for log, expected in (
            (valid + valid, "exactly one"),
            ("[AO-MIP-OFFSET-ERROR] invalid\n", "ERROR"),
            ("[AO-MIP-OFFSET] requested=3.25 applied=2.0\n", "applied mismatch"),
            (valid, "without a request"),
        ):
            with self.subTest(expected=expected):
                requested = None if expected == "without a request" else 3.25
                with self.assertRaisesRegex(RuntimeError, expected):
                    check(log, requested)

    def test_explicit_xe_runtime_selection_and_counters_are_required(self):
        check = self.functions["validate_xegtao_mip_offset_runtime"]
        valid = "\n".join((
            "[MP6-AO] requested implementation: XeGTAO (environment override)",
            "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, 2 spatial denoise passes",
            "[MP6-AO] encoded=12 composited=12 foliage=0 xegtao=12",
        )) + "\n"
        result = check(valid)
        self.assertEqual(result["counters"]["xegtao"], 12)
        for mutation in (
            valid.replace("environment override", "platform default"),
            valid.replace("xegtao=12", "xegtao=0"),
            valid.replace("[MP6-AO] XeGTAO reference:", "[MP6-AO] XeGTAO experimental:", 1)
                         .replace("XeGTAO experimental:", "XeGTAO unavailable: using legacy", 1),
        ):
            with self.subTest(mutation=mutation):
                with self.assertRaises(RuntimeError):
                    check(mutation)

    def test_runner_wires_scrubbed_env_provenance_and_report(self):
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("parser.add_argument('--xegtao-depth-mip-offset'", source)
        self.assertIn("env['MP6_QA_AO_DEPTH_MIP_OFFSET'] = str(args.xegtao_depth_mip_offset)", source)
        self.assertIn("ready.get('ao_mip_offset_probe_sha256') != mip_offset_probe_hash", source)
        self.assertIn("report['ao_mip_offset'] = ao_mip_offset", source)
        self.assertIn("parser.error('--xegtao-depth-mip-offset requires the isolated board-QA executable')", source)
        self.assertIn("validate_xegtao_depth_mip_offset(log, None, result, reason)", source)
        self.assertLess(source.index("not k.startswith('MP6_')"),
                        source.index("env['MP6_QA_AO_DEPTH_MIP_OFFSET']"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
