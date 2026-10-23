"""CPU-only source contracts for the opt-in AO/FI matrix probe."""

from pathlib import Path
import re
import sys
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(ROOT))

import ao_fi_matrix_probe as probe
from tools import build


FRAME_SOURCE = ROOT / "src" / "gx" / "frame_interp.c"
FOLIAGE_SOURCE = ROOT / "include" / "mp6_ao_foliage_draw.h"
HSF_SOURCES = (
    ROOT / "build" / "patched-src" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "verify-patched-src" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "deps" / "marioparty6" / "src" / "game" / "hsfdraw.c",
    ROOT.parent / "decomp-build" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "board-qa" / "game_hsfdraw_probe.c",
)


def _hsf_source():
    """Load a prepared HSF source, adding the private-header fixture seam.

    The original decomp source predates the foliage header include.  The
    transform is intentionally tested against a prepared source when one is
    available, but the same fixture prepend keeps the source contract useful
    for a clean checkout with only an extracted HSF translation unit.
    """
    for path in HSF_SOURCES:
        if path.is_file():
            text = path.read_text(encoding="utf-8")
            if '#include "mp6_ao_foliage_draw.h"' not in text:
                text = '#include "mp6_ao_foliage_draw.h"\n' + text
            return text
    raise unittest.SkipTest("prepared HSF source unavailable")


class AoFiMatrixProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frame = FRAME_SOURCE.read_text(encoding="utf-8")
        cls.foliage = FOLIAGE_SOURCE.read_text(encoding="utf-8")
        cls.hsf = _hsf_source()

    def test_hsf_transform_is_root_attributed_and_preserves_matrix_loads(self):
        transformed = probe.instrument_hsfdraw(self.hsf)

        self.assertIn("QA_AO_FI_MATRIX_HSF", transformed)
        self.assertIn('strcmp(hsf->root->name,"b01_m002")', transformed)
        self.assertNotIn("model==32", transformed)
        self.assertNotIn("model == 32", transformed)
        self.assertEqual(transformed.count("qa_ao_fi_draw_hint(drawObj);"), 2)
        self.assertEqual(
            transformed.count("GXLoadPosMtxImm(drawObj->matrix, GX_PNMTX0);"),
            self.hsf.count("GXLoadPosMtxImm(drawObj->matrix, GX_PNMTX0);"),
        )
        self.assertEqual(self.hsf.count('#include "mp6_ao_foliage_draw.h"'), 1)

    def test_foliage_transform_observes_the_same_draw_matrix_without_changing_it(self):
        transformed = probe.instrument_foliage(self.foliage)

        self.assertIn("QA_AO_FI_MATRIX_FOLIAGE", transformed)
        self.assertIn("qa_ao_fi_draw_hint(draw);", transformed)
        self.assertEqual(transformed.count("qa_ao_fi_draw_hint(draw);"), 1)
        self.assertEqual(
            transformed.count("GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);"),
            self.foliage.count("GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);"),
        )

    def test_frame_transform_has_bounded_window_and_matrix_attribution_fields(self):
        transformed = probe.instrument_frame_interp(self.frame)

        for option in (
            "MP6_QA_AO_FI_MATRIX",
            "MP6_QA_AO_FI_MATRIX_FROM",
            "MP6_QA_AO_FI_MATRIX_TO",
            "MP6_QA_AO_FI_MATRIX_LIMIT",
        ):
            with self.subTest(option=option):
                self.assertIn(option, transformed)
        self.assertIn('\\"assetRoot\\":\\"b01_m002\\"', transformed)
        self.assertIn("qaAoFiTo-qaAoFiFrom<=120", transformed)
        self.assertIn("qaAoFiLimit<=1024", transformed)
        self.assertIn("qaAoFiEmitted<qaAoFiLimit", transformed)

        # The fields are emitted as escaped C string literals.  Check the
        # actual JSON names rather than merely checking that diagnostics exist.
        for field in (
            "previous",
            "current",
            "output",
            "objectMismatch",
            "pairReason",
            "rewriteReason",
            "submitted",
            "contextMismatch",
            "groupMainCurrent",
            "groupAuxCurrent",
            "groupMainPrevious",
            "groupAuxPrevious",
        ):
            with self.subTest(field=field):
                self.assertIn('\\"' + field + '\\":', transformed)
        for state in (
            '"real-verbatim"',
            '"byte-equal"',
            '"rewritten"',
            '"rewritten-scale-held"',
            '"camera-cut"',
            '"group-count-change"',
        ):
            with self.subTest(state=state):
                self.assertIn(state, transformed)

        self.assertIn("qa_ao_fi_emit_real", transformed)
        self.assertIn("qa_ao_fi_emit_replay", transformed)
        self.assertIn("qa_ao_fi_record(cur,prev,streamPosIdx,qaReason", transformed)

    def test_plan_and_full_replay_resets_are_both_scoped(self):
        transformed = probe.instrument_frame_interp(self.frame)
        plan = transformed[
            transformed.index("static void fi_replay_plan_debug_reset(void)"):
            transformed.index("static uint32_t fi_replay_plan_replay(")
        ]
        full = transformed[
            transformed.index("static uint32_t fi_build_replay("):
            transformed.index("int mp6_fi_idle_present(")
        ]
        self.assertEqual(transformed.count("qaAoFiReplayCount=0;"), 2)
        self.assertEqual(plan.count("qaAoFiReplayCount=0;"), 1)
        self.assertEqual(full.count("qaAoFiReplayCount=0;"), 1)
        self.assertIn("qa_ao_fi_record(cur,prev,streamPosIdx,qaReason", transformed)
        self.assertIn("rewrittenResult?payload:cur->data+c.offset+5", transformed)

    def test_group_domains_are_observational_and_leave_pair_guard_unchanged(self):
        transformed = probe.instrument_frame_interp(self.frame)

        self.assertIn("qa_ao_fi_group_domains", transformed)
        self.assertIn(
            "other->sub>=0x8000u && other->sub<0xC000u", transformed
        )
        self.assertIn(
            "/* Immediate main is 0xFFFF, NOT auxiliary despite its high bit. */",
            transformed,
        )
        self.assertIn("else (*mainCount)++;", transformed)
        self.assertIn(
            "qa_ao_fi_group_domains(key,cur->posKey,cur->keyCount",
            transformed,
        )
        self.assertIn(
            "qa_ao_fi_group_domains(key,prev->posKey,prev->keyCount",
            transformed,
        )

        # The diagnostic census must not alter the production group guard or
        # its pairing decisions.  The helper is inserted after this function.
        start_marker = "static void fi_pair_stream(FiStream *cur, const FiStream *prev)"
        end_marker = "/* =======================================================================\n * TRS decompose / advance / recompose + normal-matrix recompute."
        original_start = self.frame.index(start_marker)
        transformed_start = transformed.index(start_marker)
        original_end = self.frame.index(end_marker, original_start)
        transformed_end = transformed.index(end_marker, transformed_start)
        self.assertEqual(
            transformed[transformed_start:transformed_end],
            self.frame[original_start:original_end],
        )

    def test_selector_compiles_and_admits_only_exact_authored_aux_groups(self):
        selector_start = probe.HELPERS.index("/* Row admission keeps")
        selector_end = probe.HELPERS.index(
            "static const QaAoFiHint *qa_ao_fi_find", selector_start
        )
        selector = probe.HELPERS[selector_start:selector_end]
        source = r'''
#include <assert.h>
#include <stdint.h>

typedef struct {
    int16_t model;
    int8_t camera;
    uint8_t valid;
    uint32_t generation;
    uint16_t ordinal;
    uint16_t sub;
} FiPosKey;
typedef struct {
    int valid, model, object, selected;
} QaAoFiHint;

static uint16_t fi_key_object_group(const FiPosKey *key) {
    if (key->sub >= 0x4000u && key->sub < 0xC000u)
        return (uint16_t)(0x4000u + (key->sub & 0x3FFFu));
    return 0;
}
static int fi_key_same_group(const FiPosKey *a, const FiPosKey *b) {
    return a->valid && b->valid && a->model == b->model &&
        a->camera == b->camera && a->generation == b->generation &&
        fi_key_object_group(a) == fi_key_object_group(b);
}
''' + selector + r'''

static FiPosKey make_key(int model, int camera, uint32_t generation,
                         uint16_t sub, int valid = 1) {
    FiPosKey key = {(int16_t)model, (int8_t)camera, (uint8_t)valid,
                    generation, 0, sub};
    return key;
}

int main() {
    QaAoFiHint object7 = {1, 9, 7, 0};
    QaAoFiHint otherModelHint = {1, 10, 7, 0};
    QaAoFiHint selectedFlower = {1, 9, 7, 1};
    FiPosKey main7 = make_key(9, 2, 4, 0x4007u);
    FiPosKey aux7 = make_key(9, 2, 4, 0x8007u);
    FiPosKey main8 = make_key(9, 2, 4, 0x4008u);
    FiPosKey aux8 = make_key(9, 2, 4, 0x8008u);
    FiPosKey otherModel = make_key(10, 2, 4, 0x8007u);
    FiPosKey otherCamera = make_key(9, 3, 4, 0x8007u);
    FiPosKey otherGeneration = make_key(9, 2, 5, 0x8007u);
    FiPosKey invalidAux = make_key(9, 2, 4, 0x8007u, 0);
    FiPosKey legacy = make_key(9, 2, 4, 0xFFFFu);
    FiPosKey currentWithAux[] = {main7, aux7};
    FiPosKey currentAuxOnly[] = {aux7};
    FiPosKey currentNoAux[] = {main7};
    FiPosKey wrongObject[] = {main7, aux8};
    FiPosKey wrongModel[] = {main7, otherModel};
    FiPosKey wrongCamera[] = {main7, otherCamera};
    FiPosKey wrongGeneration[] = {main7, otherGeneration};
    FiPosKey invalidGroup[] = {main7, invalidAux};
    FiPosKey legacyGroup[] = {legacy, aux7};

    /* Both the authored visible key and its AO representation are selected. */
    assert(qa_ao_fi_select(&object7, &main7, currentWithAux, 2, 0, 0));
    assert(qa_ao_fi_select(&object7, &aux7, currentAuxOnly, 1, 0, 0));
    /* A different authored object or model/camera/generation is not a match. */
    assert(!qa_ao_fi_select(&otherModelHint, &main7, currentWithAux, 2, 0, 0));
    assert(!qa_ao_fi_select(&object7, &main7, wrongObject, 2, 0, 0));
    assert(!qa_ao_fi_select(&object7, &main7, wrongModel, 2, 0, 0));
    assert(!qa_ao_fi_select(&object7, &main7, wrongCamera, 2, 0, 0));
    assert(!qa_ao_fi_select(&object7, &main7, wrongGeneration, 2, 0, 0));
    assert(!qa_ao_fi_select(&object7, &main7, invalidGroup, 2, 0, 0));
    /* Legacy 0xFFFF keys remain whole-model-only and cannot admit a row. */
    assert(!qa_ao_fi_select(&object7, &legacy, legacyGroup, 2, 0, 0));
    /* An AO key that exists only in N-1 still retains the live main row. */
    assert(qa_ao_fi_select(&object7, &main7, currentNoAux, 1,
                           currentAuxOnly, 1));
    assert(!qa_ao_fi_select(&object7, &main7, currentNoAux, 1, 0, 0));
    /* The original five named flowers continue to pass unchanged. */
    assert(qa_ao_fi_select(&selectedFlower, &legacy, legacyGroup, 2, 0, 0));
    assert(qa_ao_fi_select(&selectedFlower, &invalidAux, 0, 0, 0, 0));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-fi-selector-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            cpp = folder / "selector.cpp"
            exe = folder / "selector.exe"
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

    def test_record_cap_is_global_across_real_and_successful_replay_rows(self):
        transformed = probe.instrument_frame_interp(self.frame)

        self.assertIn("qaAoFiEmitted<qaAoFiLimit", transformed)
        self.assertEqual(transformed.count("qaAoFiEmitted++"), 1)
        self.assertIn(
            "qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);",
            transformed,
        )
        self.assertIn(
            "qa_ao_fi_emit_replay(cur,prev,alpha,interp);",
            transformed,
        )
        self.assertEqual(
            transformed.count("qa_ao_fi_emit_one(cur,prev"), 2
        )
        self.assertLess(
            transformed.index("aurora_end_frame();\n    fi_def_restore();\n    qa_ao_fi_emit_replay"),
            transformed.index("/* MP6_FRAME_DUMP"),
        )

    def test_json_helpers_use_private_buffer_and_flush_only_at_range_or_exit(self):
        transformed = probe.instrument_frame_interp(self.frame)
        json_start = transformed.index("static void qa_ao_fi_json_string")
        json_end = transformed.index("static void qa_ao_fi_emit_one", json_start)
        json_helpers = transformed[json_start:json_end]

        for facade in ("qa_ao_fi_fputs", "qa_ao_fi_fputc", "qa_ao_fi_fprintf"):
            with self.subTest(facade=facade):
                self.assertIn(facade, json_helpers)
        for direct in ("fputs", "fputc", "fprintf"):
            with self.subTest(direct=direct):
                self.assertIsNone(
                    re.search(r"(?<!qa_ao_fi_)\b" + direct + r"\(", json_helpers)
                )

        self.assertIn("if (qaAoFiConfig && mp6_tick_count>qaAoFiTo)", transformed)
        self.assertIn("atexit(qa_ao_fi_shutdown)", transformed)
        emit_start = transformed.index("static void qa_ao_fi_emit_one")
        emit_end = transformed.index("static void qa_ao_fi_emit_real", emit_start)
        self.assertNotIn("qa_ao_fi_flush(", transformed[emit_start:emit_end])

    def test_fixed_buffer_facade_rolls_back_rows_and_flushes_exactly_once(self):
        source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define QA_AO_FI_BUFFER_CAP 64u
''' + probe.BUFFER + r'''

static void fill(char *text, size_t count, char value) {
    memset(text, (unsigned char)value, count);
    text[count] = '\0';
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "overflow")) {
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fputs("HEAD\n", NULL);
        assert(qa_ao_fi_row_end());
        assert(qaAoFiBufferUsed == 5 && qaAoFiBufferRows == 1);
        assert(qa_ao_fi_row_begin());
        for (unsigned i = 0; i < 59; ++i) qa_ao_fi_fputc('X', NULL);
        qa_ao_fi_fputc('!', NULL);
        assert(qaAoFiBufferOverflow);
        assert(!qa_ao_fi_row_end());
        assert(qaAoFiBufferUsed == 5 && qaAoFiDroppedRows == 1);
        assert(!memcmp(qaAoFiBuffer, "HEAD\n", 5));
        qa_ao_fi_fputs("blocked", NULL);
        qa_ao_fi_fputc('B', NULL);
        qa_ao_fi_fprintf(NULL, "%s", "blocked");
        assert(!qa_ao_fi_row_begin());
        assert(qaAoFiBufferUsed == 5);
        qa_ao_fi_flush("overflow", 42);
        qa_ao_fi_flush("duplicate", 43);
        assert(qaAoFiBufferClosed && qaAoFiBufferUsed == 0);
    } else if (!strcmp(argv[1], "partial")) {
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fputs("HEAD\n", NULL);
        assert(qa_ao_fi_row_end());
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fputs("PARTIAL", NULL);
        assert(qaAoFiBufferUsed == 12 && qaAoFiRowOpen);
        qa_ao_fi_flush("partial", 7);
        qa_ao_fi_flush("duplicate", 8);
        assert(qaAoFiBufferClosed && qaAoFiBufferUsed == 0);
    } else if (!strcmp(argv[1], "fputs")) {
        char text[65];
        fill(text, 64, 'P');
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fputs(text, NULL);
        assert(qaAoFiBufferUsed == 64 && !qaAoFiBufferOverflow);
        assert(qa_ao_fi_row_end());
        qa_ao_fi_flush("fputs", 1);
    } else if (!strcmp(argv[1], "fputc")) {
        assert(qa_ao_fi_row_begin());
        for (unsigned i = 0; i < 64; ++i) qa_ao_fi_fputc('C', NULL);
        assert(qaAoFiBufferUsed == 64 && !qaAoFiBufferOverflow);
        assert(qa_ao_fi_row_end());
        qa_ao_fi_flush("fputc", 2);
    } else if (!strcmp(argv[1], "vsnprintf")) {
        char accepted[64], rejected[65];
        fill(accepted, 63, 'V');
        fill(rejected, 64, 'W');
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fprintf(NULL, "%s", accepted);
        assert(qaAoFiBufferUsed == 63 && !qaAoFiBufferOverflow);
        assert(qa_ao_fi_row_end());
        assert(qa_ao_fi_row_begin());
        qa_ao_fi_fprintf(NULL, "%s", rejected);
        assert(qaAoFiBufferOverflow);
        assert(!qa_ao_fi_row_end());
        assert(qaAoFiBufferUsed == 63 && qaAoFiBufferRows == 1);
        qa_ao_fi_flush("vsnprintf", 3);
    } else {
        return 2;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-fi-buffer-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            cpp = folder / "buffer.cpp"
            exe = folder / "buffer.exe"
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG", str(cpp), "-o", str(exe)],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            expected = {
                "overflow": "HEAD\n[AO-FI-MATRIX-STATUS] {\"flush\":\"overflow\",\"viTick\":42,\"records\":1,\"bytes\":5,\"writtenBytes\":5,\"droppedRows\":1,\"overflow\":1}\n",
                "partial": "HEAD\n[AO-FI-MATRIX-STATUS] {\"flush\":\"partial\",\"viTick\":7,\"records\":1,\"bytes\":5,\"writtenBytes\":5,\"droppedRows\":1,\"overflow\":0}\n",
                "fputs": "P" * 64 + "[AO-FI-MATRIX-STATUS] {\"flush\":\"fputs\",\"viTick\":1,\"records\":1,\"bytes\":64,\"writtenBytes\":64,\"droppedRows\":0,\"overflow\":0}\n",
                "fputc": "C" * 64 + "[AO-FI-MATRIX-STATUS] {\"flush\":\"fputc\",\"viTick\":2,\"records\":1,\"bytes\":64,\"writtenBytes\":64,\"droppedRows\":0,\"overflow\":0}\n",
                "vsnprintf": "V" * 63 + "[AO-FI-MATRIX-STATUS] {\"flush\":\"vsnprintf\",\"viTick\":3,\"records\":1,\"bytes\":63,\"writtenBytes\":63,\"droppedRows\":1,\"overflow\":1}\n",
            }
            for mode, expected_stderr in expected.items():
                with self.subTest(mode=mode):
                    result = subprocess.run(
                        [str(exe), mode],
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual(result.stderr, expected_stderr)
                    self.assertEqual(result.stderr.count("[AO-FI-MATRIX-STATUS]"), 1)

    def test_hints_are_one_shot_even_on_early_returns_or_missing_identity(self):
        hsf = probe.instrument_hsfdraw(self.hsf)
        helper = hsf.split("static void qa_ao_fi_draw_hint", 1)[1]
        self.assertLess(helper.index("qa_ao_fi_hint_clear();"),
                        helper.index("if (!qa_ao_fi_enabled()"))
        frame = probe.instrument_frame_interp(self.frame)
        capture = frame.split("void mp6_fi_stream_note_pos_mtx(void)", 1)[1]
        capture = capture.split("static uint32_t rd_be32", 1)[0]
        self.assertLess(capture.index("qaAoFiPending.valid=0;"),
                        capture.index("if (s_inReplay || s_cur < 0) return;"))
        self.assertIn("if (qaHasHint && qa_ao_fi_enabled())", capture)
        self.assertNotIn("key.model==qaHint.model", capture)
        self.assertEqual(capture.count("s->posKey[s->keyCount++] = key;"), 1)

    def test_replay_records_require_actual_raw_submission_and_successful_present(self):
        frame = probe.instrument_frame_interp(self.frame)
        replay = frame.split("int mp6_fi_idle_present(", 1)[1]
        self.assertIn("qa_ao_fi_submitted(offset,marker->offset-offset);", replay)
        self.assertIn("qa_ao_fi_submitted(offset,len-offset);", replay)
        self.assertLess(replay.index("aurora_end_frame();"),
                        replay.index("qa_ao_fi_emit_replay(cur,prev,alpha,interp);"))
        self.assertIn("r->submitted?r->output:NULL,r->submitted", frame)
        self.assertIn("r->outputOffset-offset<=size", frame)
        self.assertEqual(frame.count("aurora_gx_submit_raw("),
                         self.frame.count("aurora_gx_submit_raw("))
        for math_call in ("fi_write_mtx(payload, &O);", "fi_advance(ta, tb, alpha,"):
            self.assertEqual(frame.count(math_call), self.frame.count(math_call))

    def test_each_transform_rejects_missing_or_duplicate_exact_anchors(self):
        hsf_anchor = '#include "mp6_ao_foliage_draw.h"'
        hsf_load = "GXLoadPosMtxImm(drawObj->matrix, GX_PNMTX0);"
        foliage_load = "    GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);"
        frame_anchors = (
            "} FiPosKey;",
            "    long tick;           /* seal ordinal (diagnostics) */",
            "    s->keyCount = 0;",
            "void mp6_fi_stream_note_pos_mtx(void)\n{",
            "    s->posKey[s->keyCount++] = key;",
            "static void fi_set_active(int on)",
            "    fi_pair_stream(s, s_prev >= 0 ? &s_streams[s_prev] : NULL);",
            "    s_dbgScaleHold = 0;",
            "        if (emitCopyClear) s_dbgCopyClear++;",
            "        s_dbgPosSeen++;",
            "            if (state == FI_PAIR_EQUAL) s_dbgVerbatimEq++;",
            "                    s_dbgSnapDecompose++;",
            "                        s_dbgSnapGate++;",
            "                        rewrittenResult = 1;",
            "        if (!rewrittenResult && slot < 10) {",
            "            if (marker->offset > offset && !skipFoliage)\n"
            "                aurora_gx_submit_raw(s_replayBuf + offset, marker->offset - offset);",
            "        if (len > offset && !skipFoliage) aurora_gx_submit_raw(s_replayBuf + offset, len - offset);",
            "    aurora_end_frame();\n    fi_def_restore();\n    /* MP6_FRAME_DUMP",
        )

        cases = (
            ("hsf include", probe.instrument_hsfdraw, self.hsf, hsf_anchor),
            ("hsf load", probe.instrument_hsfdraw, self.hsf, hsf_load),
            ("foliage load", probe.instrument_foliage, self.foliage, foliage_load),
        )
        for name, transform, source, anchor in cases:
            with self.subTest(name=name, state="missing"):
                with self.assertRaisesRegex(ValueError, "AO FI probe seam"):
                    transform(source.replace(anchor, "", 1))
            with self.subTest(name=name, state="duplicate"):
                with self.assertRaisesRegex(ValueError, "AO FI probe seam"):
                    transform(source.replace(anchor, anchor + "\n" + anchor, 1))

        for anchor in frame_anchors:
            with self.subTest(name="frame", anchor=anchor, state="missing"):
                with self.assertRaisesRegex(ValueError, "AO FI probe seam"):
                    probe.instrument_frame_interp(self.frame.replace(anchor, "", 1))
            with self.subTest(name="frame", anchor=anchor, state="duplicate"):
                with self.assertRaisesRegex(ValueError, "AO FI probe seam"):
                    duplicate = self.frame.replace(anchor, anchor + "\n" + anchor, 1)
                    probe.instrument_frame_interp(duplicate)

    def test_reinstrumentation_is_rejected_and_inputs_remain_unchanged(self):
        cases = (
            (probe.instrument_hsfdraw, self.hsf),
            (probe.instrument_foliage, self.foliage),
            (probe.instrument_frame_interp, self.frame),
        )
        source_bytes = {
            FRAME_SOURCE: FRAME_SOURCE.read_bytes(),
            FOLIAGE_SOURCE: FOLIAGE_SOURCE.read_bytes(),
        }
        source_bytes.update(
            (path, path.read_bytes()) for path in HSF_SOURCES if path.is_file()
        )
        for transform, source in cases:
            with self.subTest(transform=transform.__name__):
                transformed = transform(source)
                self.assertNotEqual(transformed, source)
                with self.assertRaisesRegex(ValueError, "already installed"):
                    transform(transformed)
        for path, before in source_bytes.items():
            with self.subTest(path=path):
                self.assertEqual(path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
