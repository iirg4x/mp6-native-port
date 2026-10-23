"""CPU contracts for the opt-in AO admission QA transform and parser."""

from __future__ import annotations

import json
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "tests" / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

from ao_admission_probe import (  # noqa: E402
    ADMISSION_HELPER,
    ENV_NAME,
    FI_MARKER,
    MARKER,
    SUMMARY_MARKER,
    instrument_ao_admission,
    instrument_fi_admission,
    parse_admission_log,
)
from ao_mip_offset_probe import instrument_ao_mip_offset  # noqa: E402
from ao_resource_probe import instrument_ao_resources  # noqa: E402
from tools import build  # noqa: E402


def _counts(names: tuple[str, ...], **values: int) -> dict[str, int]:
    return {name: int(values.get(name, 0)) for name in names}


APPLY_NAMES = (
    "applyCalls", "disabledOrMissingView", "invalidView", "validApply",
    "poolExhausted", "taskRejected", "taskAccepted", "compositeRejected",
    "compositeAccepted",
)
FOLIAGE_NAMES = (
    "invalidCamera", "nested", "directAccepted", "seedPoolExhausted",
    "resolveFailed", "createFailed", "seedDrawRejected", "seedAccepted",
)
WORKER_NAMES = ("maskPresent", "maskDimensionDropped", "encoded", "composited")


def _healthy_summary(*, replay: bool = False, foliage: bool = False) -> dict:
    apply_real = _counts(APPLY_NAMES)
    apply_replay = _counts(APPLY_NAMES)
    bucket = apply_replay if replay else apply_real
    bucket.update(applyCalls=1, validApply=1, taskAccepted=1, compositeAccepted=1)
    foliage_real = _counts(FOLIAGE_NAMES)
    if foliage:
        foliage_real["directAccepted"] = 1
    worker_real = _counts(WORKER_NAMES, encoded=1, composited=1)
    return {
        "version": 1,
        "schema_version": 1,
        "enabled": 1,
        "apply": {"real": apply_real, "replay": apply_replay},
        "foliage": {
            "real": foliage_real,
            "replay": _counts(FOLIAGE_NAMES),
            "endResolved": 1 if foliage else 0,
            "endFailed": 0,
        },
        "worker": {"real": worker_real, "replay": _counts(WORKER_NAMES)},
        "failures": [],
    }


def _log(summary: dict) -> str:
    return SUMMARY_MARKER + " " + json.dumps(summary, separators=(",", ":"))


class AdmissionSourceTransformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ao_source = (ROOT / "src" / "gx" / "ambient_occlusion.cpp").read_text(
            encoding="utf-8"
        )
        cls.fi_source = (ROOT / "src" / "gx" / "frame_interp.c").read_text(
            encoding="utf-8"
        )

    def test_ao_transform_has_bounded_paths_and_snapshot(self):
        transformed = instrument_ao_admission(self.ao_source)
        self.assertIn(MARKER, transformed)
        self.assertIn("qa_ao_admission_enabled", transformed)
        self.assertEqual(transformed.count("[AO-ADMISSION]"), 1)
        self.assertEqual(transformed.count("qa_ao_emit_admission();"), 1)
        self.assertEqual(transformed.count("QA_W_ENCODED"), 2)  # enum + worker call
        self.assertEqual(transformed.count("QA_W_COMPOSITED"), 2)
        reservation = transformed.index("job->frame = g_frame;")
        snapshot = transformed.index("job->qaAdmissionCamera=view->camera;")
        task = transformed.index("push_encoder_task")
        self.assertLess(reservation, snapshot)
        self.assertLess(snapshot, task)
        seed = transformed.index("job->frame=g_frame; job->depth=targets.depth;")
        seed_snapshot = transformed.index("job->qaAdmissionCamera=camera;", seed)
        self.assertLess(seed, seed_snapshot)
        # The worker reads the reservation snapshot, never global frame/tick
        # state, and foliage uses the same bounded replay bucket.
        self.assertIn("qa_ao_worker_add(job.qaAdmissionReplay, QA_W_MASK)", transformed)
        self.assertIn("qa_ao_worker_add(job.qaAdmissionReplay, QA_W_ENCODED)", transformed)
        self.assertIn("qa_ao_end_add(qaReplay, true)", transformed)

    def test_fi_export_follows_unique_replay_declaration(self):
        transformed = instrument_fi_admission(self.fi_source)
        self.assertEqual(transformed.count(FI_MARKER), 1)
        self.assertEqual(transformed.count("static int s_inReplay;"), 1)
        self.assertLess(
            transformed.index("static int s_inReplay;") ,
            transformed.index("qa_ao_admission_is_replay(void)"),
        )
        with self.assertRaises(ValueError):
            instrument_fi_admission(transformed)

    def test_transforms_fail_closed_and_compose_in_builder_order(self):
        with self.assertRaises(ValueError):
            instrument_ao_admission(self.ao_source.replace(
                "    ++g_composited;\n", "", 1
            ))
        with self.assertRaises(ValueError):
            instrument_fi_admission(self.fi_source.replace("static int s_inReplay;", "", 1))
        resource = instrument_ao_resources(self.ao_source)
        admission = instrument_ao_admission(resource)
        composed = instrument_ao_mip_offset(admission)
        self.assertIn("MP6_QA_AO_RESOURCES", composed)
        self.assertIn(MARKER, composed)
        self.assertIn("MP6_QA_AO_DEPTH_MIP_OFFSET", composed)
        with self.assertRaises(ValueError):
            instrument_ao_admission(admission)

    def test_helper_latches_exact_environment_and_bounds_failures(self):
        self.assertIn('std::strcmp(value, "1") == 0', ADMISSION_HELPER)
        self.assertIn("static const bool enabled", ADMISSION_HELPER)
        self.assertIn("g_qaFailureCount", ADMISSION_HELPER)
        self.assertIn("std::min<unsigned>(g_qaFailureCount", ADMISSION_HELPER)
        self.assertIn("failure.frame", ADMISSION_HELPER)
        self.assertIn("failure.camera", ADMISSION_HELPER)


class AdmissionParserTests(unittest.TestCase):
    def test_valid_real_and_replay_records(self):
        parsed = parse_admission_log(_log(_healthy_summary()))
        self.assertEqual(parsed["totals"]["validApply"], 1)
        replay = _healthy_summary(replay=True)
        replay["worker"]["real"]["encoded"] = 0
        replay["worker"]["real"]["composited"] = 0
        replay["worker"]["replay"]["encoded"] = 1
        replay["worker"]["replay"]["composited"] = 1
        parsed = parse_admission_log(_log(replay))
        self.assertEqual(parsed["totals"]["validApply"], 1)

    def test_valid_foliage_direct_path_is_conserved(self):
        parsed = parse_admission_log(_log(_healthy_summary(foliage=True)))
        self.assertEqual(parsed["totals"]["endResolved"], 1)

    def test_rejects_malformed_duplicate_or_multiple_summaries(self):
        with self.assertRaises(ValueError):
            parse_admission_log("[AO-ADMISSION] {not-json}")
        line = _log(_healthy_summary())
        with self.assertRaises(ValueError):
            parse_admission_log(line + "\n" + line)
        duplicate = line.replace('"version":1', '"version":1,"version":1', 1)
        with self.assertRaises(ValueError):
            parse_admission_log(duplicate)

    def test_rejects_failures_and_conservation_mismatches(self):
        failure = _healthy_summary()
        failure["apply"]["real"]["invalidView"] = 1
        with self.assertRaises(ValueError):
            parse_admission_log(_log(failure))
        mismatch = _healthy_summary()
        mismatch["apply"]["real"]["applyCalls"] = 2
        with self.assertRaises(ValueError):
            parse_admission_log(_log(mismatch))
        bucket_mismatch = _healthy_summary()
        bucket_mismatch["apply"]["real"]["disabledOrMissingView"] = 1
        with self.assertRaises(ValueError):
            parse_admission_log(_log(bucket_mismatch))
        sample = _healthy_summary()
        sample["failures"] = [{"frame": 1, "tick": 2, "camera": -1, "replay": 0,
                                "reason": "disabled"}]
        with self.assertRaises(ValueError):
            parse_admission_log(_log(sample))

    def test_rejects_bad_schema_and_mask_drop_overcount(self):
        bad = _healthy_summary()
        bad["schema_version"] = 2
        with self.assertRaises(ValueError):
            parse_admission_log(_log(bad))
        bad = _healthy_summary()
        bad["enabled"] = True
        with self.assertRaises(ValueError):
            parse_admission_log(_log(bad))
        bad = _healthy_summary()
        bad["worker"]["real"]["maskPresent"] = 1
        bad["worker"]["real"]["maskDimensionDropped"] = 1
        with self.assertRaises(ValueError):
            parse_admission_log(_log(bad))


class AdmissionHelperCompileTests(unittest.TestCase):
    def test_compiled_helper_emits_a_parseable_summary(self):
        with tempfile.TemporaryDirectory(prefix="ao-admission-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            source = folder / "admission.cpp"
            executable = folder / "admission.exe"
            source.write_text(
                "#include <algorithm>\n"
                "#include <array>\n"
                "#include <atomic>\n"
                "#include <cstdint>\n"
                "#include <cstdio>\n"
                "#include <cstdlib>\n"
                "#include <cstring>\n"
                "extern \"C\" int qa_ao_admission_is_replay(void) { return 0; }\n"
                "extern \"C\" long mp6_tick_count = 0;\n"
                + ADMISSION_HELPER
                + r'''
int main() {
    qa_ao_admission_add(g_qaApply, QA_REAL, QA_APPLY_CALLS);
    qa_ao_admission_add(g_qaApply, QA_REAL, QA_VALID);
    qa_ao_admission_add(g_qaApply, QA_REAL, QA_TASK_ACCEPTED);
    qa_ao_admission_add(g_qaApply, QA_REAL, QA_COMPOSITE_ACCEPTED);
    qa_ao_worker_add(QA_REAL, QA_W_ENCODED);
    qa_ao_worker_add(QA_REAL, QA_W_COMPOSITED);
    qa_ao_emit_admission();
}
''',
                encoding="utf-8",
            )
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", str(source), "-o", str(executable)],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = os.environ.copy()
            env[ENV_NAME] = "1"
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, env=env, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(parse_admission_log(result.stderr)["totals"]["validApply"], 1)
        bad = _healthy_summary()
        bad["worker"]["real"]["maskDimensionDropped"] = 1
        with self.assertRaises(ValueError):
            parse_admission_log(_log(bad))


if __name__ == "__main__":  # pragma: no cover
    unittest.main(verbosity=2)
