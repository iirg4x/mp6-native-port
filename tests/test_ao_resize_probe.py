import json
import unittest

from tests.integration.ao_resize_probe import (
    RESIZE_DRIVER,
    TARGET_SIZES,
    parse_resize_log,
    resize_ao_error,
    resize_probe,
    validate_resize_ao_evidence,
)
from tests.integration.ao_resource_probe import V2_PROVENANCE


_SOURCE = """void VIWaitForRetrace(void)\n{\n    mp6_dispatch_aurora_events(aurora_update());\n    aurora_begin_frame();\n}\n"""


def _resource_census(sizes=TARGET_SIZES):
    changes = [
        {"target_width": width, "target_height": height}
        for width, height in sizes
    ]
    width, height = sizes[-1]
    return {
        "schema_version": 2,
        "provenance": V2_PROVENANCE,
        "record": {"target_width": width, "target_height": height},
        "changes": changes,
    }


def _resize_log(*, include_fi=False, rows=None):
    if rows is None:
        rows = [
            {"phase": "armed", "tick": 100, "seq": 1},
            {
                "phase": "request",
                "step": 0,
                "tick": 160,
                "requested": [1281, 721],
                "hidden": 1,
            },
            {
                "phase": "applied",
                "step": 0,
                "tick": 161,
                "requested": [1281, 721],
                "logical": [1281, 721],
                "pixels": [1281, 721],
                "hidden": 1,
            },
            {
                "phase": "request",
                "step": 1,
                "tick": 280,
                "requested": [960, 540],
                "hidden": 1,
            },
            {
                "phase": "applied",
                "step": 1,
                "tick": 281,
                "requested": [960, 540],
                "logical": [960, 540],
                "pixels": [960, 540],
                "hidden": 1,
            },
            {
                "phase": "request",
                "step": 2,
                "tick": 400,
                "requested": [1281, 721],
                "hidden": 1,
            },
            {
                "phase": "applied",
                "step": 2,
                "tick": 401,
                "requested": [1281, 721],
                "logical": [1281, 721],
                "pixels": [1281, 721],
                "hidden": 1,
            },
        ]
    lines = ["[AO-RESIZE] " + json.dumps(row, separators=(",", ":")) for row in rows]
    if include_fi:
        lines.insert(0, "[MP6-FI] capture armed (unlocked fps ON)")
    return "\n".join(lines) + "\n"


class ResizeProbeTransformTests(unittest.TestCase):
    def test_transform_is_bounded_and_runs_before_aurora_event_pump(self):
        transformed = resize_probe(_SOURCE)

        self.assertIn("MP6_AO_RESIZE_QA", transformed)
        self.assertIn("SDL_WINDOW_HIDDEN", transformed)
        self.assertIn("mp6_aspect_window()", transformed)
        self.assertIn("SDL_SetWindowSize", transformed)
        self.assertNotIn("SDL_ShowWindow", transformed)
        self.assertNotIn("SDL_RaiseWindow", transformed)
        self.assertEqual(transformed.count("qa_resize_ao_tick();"), 1)
        self.assertLess(
            transformed.index("qa_resize_ao_tick();"),
            transformed.index("mp6_dispatch_aurora_events(aurora_update());"),
        )

    def test_transform_rejects_missing_duplicate_or_reinstrumented_source(self):
        with self.assertRaises(ValueError):
            resize_probe("void VIWaitForRetrace(void) {}\n")
        with self.assertRaises(ValueError):
            resize_probe(_SOURCE.replace(
                "    mp6_dispatch_aurora_events(aurora_update());\n",
                "    mp6_dispatch_aurora_events(aurora_update());\n"
                "    mp6_dispatch_aurora_events(aurora_update());\n",
            ))
        with self.assertRaises(ValueError):
            resize_probe(_SOURCE + "/* MP6_AO_RESIZE_QA */\n")

    def test_disabled_mode_is_guarded_and_has_no_import_side_effects(self):
        transformed = resize_probe(_SOURCE)
        self.assertIn(
            'const char *enabled = getenv("MP6_QA_RESIZE_AO");',
            transformed,
        )
        self.assertIn("if (!enabled || enabled[0] == '\\0' || enabled[0] == '0')", transformed)
        self.assertNotIn("build_board_qa", RESIZE_DRIVER)

    def test_transform_can_be_removed_without_changing_original_source(self):
        transformed = resize_probe(_SOURCE)
        declaration = "static void qa_resize_ao_tick(void);\n\n"
        call = "    qa_resize_ao_tick();\n"
        restored = transformed.removesuffix(RESIZE_DRIVER)
        restored = restored.replace(declaration, "", 1)
        restored = restored.replace(call, "", 1)
        self.assertEqual(restored, _SOURCE)


class ResizeProbeEvidenceTests(unittest.TestCase):
    def test_valid_evidence_requires_exact_schedule_and_v2_census(self):
        evidence = validate_resize_ao_evidence(
            _resize_log(include_fi=True),
            _resource_census(),
            require_fi=True,
        )
        self.assertEqual(evidence["schedule"], [list(size) for size in TARGET_SIZES])
        self.assertEqual(evidence["resource_targets"], [list(size) for size in TARGET_SIZES])
        self.assertTrue(evidence["fi"])

    def test_missing_or_malformed_resize_rows_fail(self):
        valid_lines = _resize_log().splitlines()
        with self.assertRaises(ValueError):
            validate_resize_ao_evidence(
                "\n".join(valid_lines[:-1]) + "\n",
                _resource_census(),
            )
        with self.assertRaises(ValueError):
            parse_resize_log("[AO-RESIZE] {not-json}\n")

    def test_out_of_order_requests_and_wrong_applied_pixels_fail(self):
        lines = _resize_log().splitlines()
        rows = [json.loads(line[len("[AO-RESIZE] "):]) for line in lines]
        rows[1], rows[3] = rows[3], rows[1]
        with self.assertRaises(ValueError):
            validate_resize_ao_evidence(_resize_log(rows=rows), _resource_census())

        rows = [json.loads(line[len("[AO-RESIZE] "):]) for line in lines]
        rows[-1]["pixels"] = [1280, 721]
        with self.assertRaises(ValueError):
            validate_resize_ao_evidence(_resize_log(rows=rows), _resource_census())

    def test_unexpected_census_size_and_missing_fi_proof_fail(self):
        bad_sizes = ((1281, 721), (1024, 576), (1281, 721))
        with self.assertRaises(ValueError):
            validate_resize_ao_evidence(
                _resize_log(),
                _resource_census(bad_sizes),
            )
        with self.assertRaises(ValueError):
            validate_resize_ao_evidence(
                _resize_log(),
                _resource_census(),
                require_fi=True,
            )

    def test_cli_admission_is_opt_in_and_xegtao_only(self):
        self.assertIsNone(resize_ao_error(False, "640x480", 0, "neutral"))
        self.assertIsNone(resize_ao_error(True, "1281x721", 1, "xegtao"))
        self.assertIn("--window-size 1281x721", resize_ao_error(True, "640x480", 1, "xegtao"))
        self.assertIn("--ao", resize_ao_error(True, "1281x721", 0, "xegtao"))
        self.assertIn("xegtao", resize_ao_error(True, "1281x721", 1, "neutral"))


if __name__ == "__main__":
    unittest.main()
