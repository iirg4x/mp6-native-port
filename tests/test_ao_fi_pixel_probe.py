"""CPU-only contracts for the opt-in AO/FI pixel probe."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(ROOT))

import ao_fi_matrix_probe as matrix_probe
import ao_fi_pixel_probe as probe
from tools import build


FRAME_SOURCE = ROOT / "src" / "gx" / "frame_interp.c"
DUMP_SOURCE = ROOT / "src" / "gx" / "frame_dump.c"


class AoFiPixelProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frame = FRAME_SOURCE.read_text(encoding="utf-8")
        cls.matrix = matrix_probe.instrument_frame_interp(cls.frame)
        cls.dump = DUMP_SOURCE.read_text(encoding="utf-8")

    def test_interp_requires_matrix_probe_and_rejects_reinstrumentation(self):
        with self.assertRaisesRegex(ValueError, "requires the AO FI matrix"):
            probe.instrument_frame_interp(self.frame)
        transformed = probe.instrument_frame_interp(self.matrix)
        with self.assertRaisesRegex(ValueError, "already installed"):
            probe.instrument_frame_interp(transformed)

    def test_interp_phase_is_before_replay_and_pending_is_scoped(self):
        transformed = probe.instrument_frame_interp(self.matrix)
        self.assertIn("QA_AO_FI_PIXEL:", transformed)
        self.assertIn("MP6_QA_AO_FI_PIXELS", transformed)
        self.assertIn("qa_ao_fi_pixel_override_alpha(cur,&alpha);", transformed)
        self.assertLess(
            transformed.index("qa_ao_fi_pixel_override_alpha(cur,&alpha);"),
            transformed.index("len = fi_build_replay(cur, prev, interp, alpha);"),
        )
        self.assertIn(
            "qa_ao_fi_emit_replay(cur,prev,alpha,interp);\n"
            "    qa_ao_fi_pixel_snapshot(cur,prev,alpha,interp);",
            transformed,
        )
        self.assertIn(
            "mp6_frame_dump_present(1); }\n    qa_ao_fi_pixel_pending_reset();",
            transformed,
        )
        # Matrix pairing/admission and replay cadence anchors remain present.
        for anchor in (
            "fi_pair_stream(s, s_prev >= 0 ? &s_streams[s_prev] : NULL);",
            "qa_ao_fi_record(cur,prev,streamPosIdx,qaReason",
            "if (mp6_fi_replay_limit_reached(paced, s_replaysThisWindow))",
            "aurora_try_begin_frame()",
            "aurora_end_frame();",
        ):
            self.assertIn(anchor, transformed)
        self.assertEqual(
            transformed.count("qa_ao_fi_emit_replay(cur,prev,alpha,interp);"), 1
        )

    def test_dump_hook_is_after_index_before_armed_and_commits_after_csv(self):
        transformed = probe.instrument_frame_dump(self.dump)
        self.assertIn("QA_AO_FI_PIXEL_DUMP", transformed)
        self.assertEqual(transformed.count("qa_ao_fi_pixel_present_begin("), 2)
        begin = transformed.index("qaAoFiPixelCapture=qa_ao_fi_pixel_present_begin")
        index = transformed.index("s_fdPresentIndex++;")
        armed = transformed.index("if (!s_fdArmed || s_fdDone) return;", index)
        self.assertLess(index, begin)
        self.assertLess(begin, armed)
        commit = transformed.index("qa_ao_fi_pixel_present_commit(")
        written = transformed.index("s_fdWritten++;", commit)
        self.assertLess(commit, written)
        self.assertIn("qaAoFiPixelCsvOk=(fflush(s_fdIndex)==0 && !ferror(s_fdIndex));", transformed)
        self.assertIn("if (qa_ao_fi_pixel_present_mode() && !qaAoFiPixelCapture) return;", transformed)
        self.assertIn("qaAoFiPixelPending.captured", probe.PIXEL_HELPERS)
        self.assertIn("qaAoFiPixelPending.presentIndex!=presentIndex", probe.PIXEL_HELPERS)
        self.assertIn("qaAoFiPixelCapturedViTick", probe.PIXEL_HELPERS)
        self.assertIn("qa_ao_fi_pixel_duplicate(replay->index)", probe.PIXEL_HELPERS)
        self.assertIn("qaAoFiPixelPending.matrixCount", probe.PIXEL_HELPERS)
        self.assertIn("previousHintOverflow", probe.PIXEL_HELPERS)
        snapshot = probe.PIXEL_HELPERS[
            probe.PIXEL_HELPERS.index("static void qa_ao_fi_pixel_snapshot"):
            probe.PIXEL_HELPERS.index("static void qa_ao_fi_pixel_override_alpha")
        ]
        self.assertNotIn("qa_ao_fi_enabled()", snapshot)
        frame = probe.instrument_frame_interp(self.matrix)
        self.assertIn('mp6_tick_count<=qaAoFiTo+1;', frame)
        self.assertIn('qaAoFiEmitted>=qaAoFiLimit) return;', frame)
        self.assertEqual(
            transformed.count("aurora_gx_debug_capture_present_frame("),
            self.dump.count("aurora_gx_debug_capture_present_frame("),
        )
        self.assertEqual(transformed.count("s_fdPresentIndex++;"), 1)
        with self.assertRaisesRegex(ValueError, "already installed"):
            probe.instrument_frame_dump(transformed)

    def test_phase_parser_compiles_disabled_malformed_range_and_phase_cases(self):
        source = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + probe.PIXEL_PHASE_HELPER + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "disabled")) {
        assert(!qa_ao_fi_pixel_selected_tick(10));
    } else if (!strcmp(argv[1], "valid")) {
        assert(qa_ao_fi_pixel_selected_tick(10));
        assert(qa_ao_fi_pixel_selected_tick(12));
        assert(qa_ao_fi_pixel_selected_tick(14));
        assert(qa_ao_fi_pixel_selected_tick(16));
        assert(!qa_ao_fi_pixel_selected_tick(11));
        assert(qa_ao_fi_pixel_phase(10) == 0.0);
        assert(qa_ao_fi_pixel_phase(12) == 0.25);
        assert(qa_ao_fi_pixel_phase(14) == 0.5);
        assert(qa_ao_fi_pixel_phase(16) == 0.75);
    } else if (!strcmp(argv[1], "range")) {
        assert(!qa_ao_fi_pixel_selected_tick(0));
    } else if (!strcmp(argv[1], "dedup")) {
        assert(qa_ao_fi_pixel_selected_tick(10));
        assert(qa_ao_fi_pixel_selected_tick(10));
        assert(qa_ao_fi_pixel_phase(10) == 0.0);
    } else {
        return 2;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-fi-pixel-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            cpp = folder / "phase.cpp"
            exe = folder / "phase.exe"
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG", str(cpp), "-o", str(exe)],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            cases = (
                ("disabled", None),
                ("valid", "10:16:2"),
                ("range", "0:121:1"),
                ("dedup", "10:10:1"),
            )
            for name, value in cases:
                env = dict(os.environ)
                if value is None:
                    env.pop("MP6_QA_AO_FI_PIXELS", None)
                else:
                    env["MP6_QA_AO_FI_PIXELS"] = value
                with self.subTest(case=name):
                    result = subprocess.run(
                        [str(exe), name], capture_output=True, text=True,
                        env=env, timeout=10,
                    )
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
            env = dict(os.environ)
            env["MP6_QA_AO_FI_PIXELS"] = "10:16"
            result = subprocess.run(
                [str(exe), "disabled"], capture_output=True, text=True,
                env=env, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("AO-FI-PIXEL-ERROR", result.stderr)

    def test_pending_commit_compiles_fail_closed_csv_and_same_tick_dedup(self):
        types = probe.PIXEL_TYPES
        begin_start = probe.PIXEL_HELPERS.index(
            "int qa_ao_fi_pixel_present_begin"
        )
        json_start = probe.PIXEL_HELPERS.index(
            "static void qa_ao_fi_pixel_json_string", begin_start
        )
        begin = probe.PIXEL_HELPERS[begin_start:json_start]
        commit_start = probe.PIXEL_HELPERS.index(
            "void qa_ao_fi_pixel_present_commit"
        )
        commit = probe.PIXEL_HELPERS[commit_start:]
        source = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>

#define QA_AO_FI_HINT_MAX 4u
typedef struct {
    int16_t model;
    int8_t camera;
    uint8_t valid;
    uint32_t generation;
    uint16_t ordinal;
    uint16_t sub;
} FiPosKey;
''' + types + r'''
static unsigned emitted;
static int qa_ao_fi_pixel_configure(void) { return 1; }
static void qa_ao_fi_pixel_emit(unsigned seq, unsigned presentIndex) {
    (void)seq; (void)presentIndex; ++emitted;
}
''' + begin + commit + r'''
int main(void) {
    qaAoFiPixelPending.valid=1;
    qaAoFiPixelPending.interp=1;
    qaAoFiPixelPending.rowCount=1;
    qaAoFiPixelPending.viTick=42;
    assert(qa_ao_fi_pixel_present_begin(0, 7) == 0);
    assert(qa_ao_fi_pixel_present_begin(1, 7) == 1);
    qa_ao_fi_pixel_present_commit(3, 7, 0);
    assert(!qaAoFiPixelPending.captured && emitted == 0);
    qa_ao_fi_pixel_present_commit(3, 7, 1);
    assert(qaAoFiPixelPending.captured && emitted == 1);
    assert(qaAoFiPixelCapturedViTick == 42);
    assert(qa_ao_fi_pixel_present_begin(1, 7) == 0);
    qa_ao_fi_pixel_present_commit(4, 7, 1);
    assert(emitted == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-fi-pixel-commit-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            cpp = folder / "commit.cpp"
            exe = folder / "commit.exe"
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG", str(cpp), "-o", str(exe)],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(exe)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
