"""CPU-only contracts for natural-alpha deferred AO/FI capture."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from tools import build
import ao_deferred_capture as capture
import ao_deferred_capture_source as deferred
import ao_fi_matrix_probe as matrix_probe
import ao_fi_pixel_evidence as pixel_evidence
import ao_fi_pixel_probe as pixel_probe
import build_ao_cadence_qa as cadence_builder


FRAME_SOURCE = ROOT / "src" / "gx" / "frame_interp.c"
DUMP_SOURCE = ROOT / "src" / "gx" / "frame_dump.c"


class DeferredCaptureSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.frame = FRAME_SOURCE.read_text(encoding="utf-8")
        cls.dump = DUMP_SOURCE.read_text(encoding="utf-8")

    def _pipeline(self) -> str:
        """Apply the production QA transforms in their required order."""

        matrix = matrix_probe.instrument_frame_interp(self.frame)
        pixels = pixel_probe.instrument_frame_interp(matrix)
        return deferred.instrument_interp(pixels)

    def test_interp_chain_is_natural_and_preserves_historical_matrix_probe(self) -> None:
        matrix = matrix_probe.instrument_frame_interp(self.frame)
        pixels = pixel_probe.instrument_frame_interp(matrix)
        transformed = deferred.instrument_interp(pixels)

        # The old fixed-phase override is not allowed to alter replay bytes in
        # this capture variant.  The old pixel probe itself remains untouched;
        # only this private deferred transform removes its call sites.
        self.assertIn("qa_ao_fi_pixel_override_alpha(cur,&alpha);", pixels)
        self.assertNotIn("qa_ao_fi_pixel_override_alpha(cur,&alpha);", transformed)
        self.assertIn("cur->qaViTick==qaAoFiPixelCapturedViTick ||", pixels)
        self.assertNotIn("cur->qaViTick==qaAoFiPixelCapturedViTick ||", transformed)

        self.assertIn("[AO-DEFERRED-FRAME]", transformed)
        self.assertNotIn("[AO-FI-PIXEL] {", transformed)
        self.assertIn("[AO-FI-MATRIX]", transformed)
        self.assertIn("qaDeferredMetadata[128]", transformed)
        self.assertIn("seq>=128", transformed)
        self.assertIn("alpha=natural", transformed)

        # Matrix attribution is historical evidence and must be byte-for-byte
        # unchanged.  A broad alpha replacement would silently corrupt it.
        start_marker = "static void qa_ao_fi_emit_one"
        end_marker = "static void qa_ao_fi_emit_real"
        original_start = pixel_probe.instrument_frame_interp(
            matrix_probe.instrument_frame_interp(self.frame)
        ).index(start_marker)
        original_end = pixel_probe.instrument_frame_interp(
            matrix_probe.instrument_frame_interp(self.frame)
        ).index(end_marker, original_start)
        expected = pixel_probe.instrument_frame_interp(
            matrix_probe.instrument_frame_interp(self.frame)
        )[original_start:original_end]
        actual_start = transformed.index(start_marker)
        actual_end = transformed.index(end_marker, actual_start)
        self.assertEqual(transformed[actual_start:actual_end], expected)
        self.assertIn(r'\"alpha\":%.9g,', expected)

        pixel_start = transformed.index("static void qa_ao_fi_pixel_emit")
        pixel_emit = transformed[pixel_start:]
        self.assertIn(r'\"alpha\":%.17g,', pixel_emit)

        # Real and replay metadata are captured at their respective present
        # boundaries, with the replay snapshot still observing natural alpha.
        real_emit = transformed.index(
            "qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);"
        )
        real_snapshot = transformed.index("qa_deferred_real_snapshot(", real_emit)
        self.assertLess(real_emit, real_snapshot)
        replay_emit = transformed.index("qa_ao_fi_emit_replay(cur,prev,alpha,interp);")
        replay_snapshot = transformed.index(
            "qa_ao_fi_pixel_snapshot(cur,prev,alpha,interp);", replay_emit
        )
        self.assertLess(replay_emit, replay_snapshot)

    def test_interp_and_dump_seams_reject_drift_and_reinstrumentation(self) -> None:
        transformed = self._pipeline()
        with self.assertRaisesRegex(ValueError, "already installed"):
            deferred.instrument_interp(transformed)

        real_anchor = "    qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);\n"
        self.assertEqual(self._pipeline().count(real_anchor), 1)
        drifted = pixel_probe.instrument_frame_interp(
            matrix_probe.instrument_frame_interp(self.frame)
        ).replace(real_anchor, "", 1)
        with self.assertRaisesRegex(ValueError, "expected once"):
            deferred.instrument_interp(drifted)

        dump_transformed = deferred.instrument_dump(self.dump)
        with self.assertRaisesRegex(ValueError, "already installed"):
            deferred.instrument_dump(dump_transformed)
        dump_drifted = self.dump.replace("    s_fdPresentIndex++;\n", "", 1)
        with self.assertRaisesRegex(ValueError, "expected once"):
            deferred.instrument_dump(dump_drifted)

    def test_frame_dump_queues_before_original_readback_and_flushes_after_finish(self) -> None:
        transformed = deferred.instrument_dump(self.dump)
        frontend_start = transformed.index("QA_DEFERRED_CAPTURE")
        present_start = transformed.index(
            "void mp6_frame_dump_present(int replayFrame)", frontend_start
        )
        frontend = transformed[frontend_start:present_start]

        self.assertIn("mp6_qa_deferred_capture_enqueue", frontend)
        self.assertIn("mp6_qa_deferred_capture_finish", frontend)
        self.assertIn("mp6_qa_deferred_metadata_store", frontend)
        self.assertIn("mp6_qa_deferred_metadata_flush", frontend)
        self.assertIn("fd_enabled()", frontend)
        self.assertLess(frontend.index("fd_enabled()"), frontend.index("s_fdWantCount"))
        self.assertNotIn("aurora_gx_debug_capture_present_frame", frontend)
        self.assertNotIn("mp6_fopen_utf8", frontend)
        self.assertNotIn("fwrite(", frontend)
        self.assertNotIn("synchronize", frontend)
        self.assertLess(
            frontend.index("mp6_qa_deferred_capture_finish()"),
            frontend.index("mp6_qa_deferred_metadata_flush()"),
        )

        queue_return = transformed.index(
            "if (qa_deferred_present(replayFrame)) return;", present_start
        )
        original_readback = transformed.index(
            "aurora_gx_debug_capture_present_frame(NULL, 0", present_start
        )
        self.assertLess(queue_return, original_readback)
        self.assertEqual(
            transformed.count("if (qa_deferred_present(replayFrame)) return;"), 1
        )

    def test_frontend_compiled_harness_bounds_and_defers_io(self) -> None:
        source = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long mp6_tick_count;
static char s_fdDir[64] = "frames";
static char s_fdTrigger[1] = {0};
static int s_fdWantCount;
static int s_fdDelay = 0, s_fdStride = 1, s_fdDownscale = 1;
static int s_fdRectX, s_fdRectY, s_fdRectW, s_fdRectH;
static int s_fdArmed, s_fdDone;
static unsigned s_fdPresentIndex;
static unsigned fdEnabledCalls, metadataCalls, enqueueCalls, finishCalls, flushCalls;

static int fd_enabled(void) {
    ++fdEnabledCalls;
    s_fdWantCount = 4;
    s_fdRectW = 16;
    s_fdRectH = 16;
    s_fdArmed = 1;
    return 1;
}

static int mp6_parse_i32_strict(const char *text, int minValue, int maxValue,
                                int *out) {
    char *end = NULL;
    long value;
    if (!text || !*text) return 0;
    value = strtol(text, &end, 10);
    if (*end || value < minValue || value > maxValue) return 0;
    *out = (int)value;
    return 1;
}
unsigned long long mp6_host_monotonic_ns(void) { return 1234ull; }
int mp6_qa_deferred_metadata_store(unsigned seq, unsigned present, int replay) {
    (void)seq; (void)present; (void)replay; ++metadataCalls; return 1;
}
int mp6_qa_deferred_capture_enqueue(const char *dir, unsigned seq,
                                    unsigned present, long tick, int replay,
                                    unsigned long long wall, unsigned x,
                                    unsigned y, unsigned w, unsigned h) {
    (void)dir; (void)seq; (void)present; (void)tick; (void)replay;
    (void)wall; (void)x; (void)y; (void)w; (void)h;
    ++enqueueCalls; return 1;
}
int mp6_qa_deferred_capture_finish(void) { ++finishCalls; return 1; }
void mp6_qa_deferred_metadata_flush(void) { ++flushCalls; }
''' + deferred.FRONTEND + r'''
int main(void) {
    unsigned i;
    assert(qa_deferred_present(0) == 1);
    assert(fdEnabledCalls == 1);
    assert(metadataCalls == 0 && enqueueCalls == 0);
    for (i = 0; i < 4; ++i) {
        mp6_tick_count = 3000 + (long)i;
        s_fdPresentIndex = 100 + i;
        assert(qa_deferred_present(i & 1) == 1);
    }
    assert(qaDeferredQueued == 4);
    assert(metadataCalls == 4 && enqueueCalls == 4);
    assert(finishCalls == 1 && flushCalls == 1 && s_fdDone == 1);
    mp6_tick_count = 3004;
    s_fdPresentIndex = 104;
    assert(qa_deferred_present(1) == 1);
    assert(metadataCalls == 4 && enqueueCalls == 4);
    assert(finishCalls == 1 && flushCalls == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-deferred-front-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            c_file = folder / "frontend.c"
            exe = folder / "frontend.exe"
            c_file.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "cc", "-std=c11", "-O2", "-UNDEBUG", str(c_file), "-o", str(exe)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            env = dict(os.environ)
            env["MP6_QA_DEFERRED_FROM"] = "3000"
            env["MP6_QA_DEFERRED_TO"] = "3003"
            result = subprocess.run(
                [str(exe)], capture_output=True, text=True, env=env, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_metadata_store_compiled_harness_copies_real_and_replay_rows_to_128(self) -> None:
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int value; } FiPosKey;
typedef struct { unsigned index; } QaAoFiHint;
typedef struct {
    int valid, interp;
    unsigned presentIndex, rowCount, matrixCount;
    long viTick, previousViTick, seal, previousSeal;
    double alpha;
} QaAoFiPixelPending;
typedef struct {
    uint32_t index;
    int submitted, hasOutput;
    unsigned char output[48];
} QaAoFiReplay;
typedef struct {
    int walkOk;
    long qaViTick, tick;
    unsigned qaHintCount, qaHintOverflow, posCount, keyCount;
    QaAoFiHint *qaHints;
    unsigned char *data;
    unsigned *posOff;
    FiPosKey *posKey;
} FiStream;

#define QA_AO_FI_HINT_MAX 4u
static QaAoFiPixelPending qaAoFiPixelPending;
static long mp6_tick_count;
static unsigned emitted, firstPresent, lastPresent;
static void qa_ao_fi_pixel_pending_reset(void) {
    memset(&qaAoFiPixelPending, 0, sizeof(qaAoFiPixelPending));
}
static int qa_ao_fi_pixel_selected_tick(long tick) { (void)tick; return 1; }
static int qa_ao_fi_select(const QaAoFiHint *hint, const FiPosKey *key,
                           const FiPosKey *current, unsigned currentCount,
                           const FiPosKey *previous, unsigned previousCount) {
    (void)hint; (void)key; (void)current; (void)currentCount;
    (void)previous; (void)previousCount; return 1;
}
static int qa_ao_fi_pixel_append(const FiStream *cur, const FiStream *prev,
                                 const QaAoFiReplay *replay) {
    (void)cur; (void)prev; (void)replay;
    if (qaAoFiPixelPending.rowCount >= QA_AO_FI_HINT_MAX) return 0;
    ++qaAoFiPixelPending.rowCount;
    return 1;
}
static void qa_ao_fi_pixel_emit(unsigned seq, unsigned present) {
    (void)seq;
    if (!emitted) firstPresent = present;
    lastPresent = present;
    ++emitted;
}
''' + deferred.DEFERRED_METADATA + r'''
int main(void) {
    unsigned i;
    for (i = 0; i < 128; ++i) {
        qaAoFiPixelPending.valid = 1;
        qaAoFiPixelPending.interp = (int)(i & 1u);
        qaAoFiPixelPending.presentIndex = 100 + i;
        qaAoFiPixelPending.rowCount = 1;
        qaAoFiPixelPending.matrixCount = 1;
        qaAoFiPixelPending.alpha = (double)i / 128.0;
        assert(mp6_qa_deferred_metadata_store(i, 100 + i, (int)(i & 1u)));
    }
    assert(qaDeferredMetadataCount == 128);
    assert(!qaDeferredMetadataFailed);
    qaAoFiPixelPending.valid = 1;
    assert(!mp6_qa_deferred_metadata_store(128, 228, 0));
    assert(qaDeferredMetadataCount == 128 && qaDeferredMetadataFailed);
    mp6_qa_deferred_metadata_flush();
    assert(emitted == 128 && firstPresent == 100 && lastPresent == 227);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ao-deferred-metadata-", dir=ROOT / "build") as folder:
            folder = Path(folder)
            c_file = folder / "metadata.c"
            exe = folder / "metadata.exe"
            c_file.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "cc", "-std=c11", "-O2", "-UNDEBUG", str(c_file), "-o", str(exe)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(exe)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class DeferredCaptureAdmissionTests(unittest.TestCase):
    @staticmethod
    def _args(**changes):
        values = dict(
            deferred_capture="3000:3003",
            exe="build/board-cadence-qa/release/mp6native.exe",
            ticks=4000,
            ao_fi_matrix_range="2999:3003",
            ao_fi_pixels=None,
            unlocked_fps=True,
            tick_hz=60,
            leaf_camera=True,
            leaf_freeze=True,
            implementation="default",
            ao=2,
            aa=0,
            xegtao_resolution="full",
            xegtao_depth_storage="r32float",
            capture_frames=4,
            capture_stride=1,
            capture_delay=0,
            capture_trigger="-",
            capture_rect=(0, 0, 16, 16),
            xegtao_init_failure=False,
            xegtao_prepared_canonical=False,
            resize_ao=False,
            raw_ao_depth=False,
            foreground_ao=False,
            water_ao=False,
            no_grounding=False,
            no_lawn_lift=False,
            no_connector_lift=False,
            no_log_grounding=False,
            no_tree_grounding=False,
            no_foliage_mask=False,
            legacy_foliage_viewport=False,
            foliage_no_depth=False,
            legacy_foliage_blend=False,
            ao_cycle=None,
            scene_camera=None,
            xegtao_depth_mip_offset=None,
        )
        values.update(changes)
        return type("Args", (), values)()

    def test_valid_admission_and_required_rejections(self) -> None:
        self.assertIsNone(capture.deferred_capture_error(self._args()))
        cases = (
            (
                self._args(ao_fi_matrix_range="3000:3003"),
                "preceding-tick matrix coverage",
            ),
            (
                self._args(ao_fi_pixels="3000:3003:1"),
                "preceding-tick matrix coverage and no fixed phases",
            ),
            (
                self._args(capture_frames=128, capture_rect=(0, 0, 1024, 1024)),
                "64 MiB readback budget",
            ),
        )
        for args, expected in cases:
            with self.subTest(expected=expected):
                message = capture.deferred_capture_error(args)
                self.assertIsNotNone(message)
                self.assertIn(expected, message)


def _write_mfd(path: Path, seq: int, present: int, tick: int, replay: int) -> None:
    header = bytearray(pixel_evidence.MFD_HEADER_BYTES)
    header[:8] = pixel_evidence.MFD_MAGIC
    struct.pack_into("<8I", header, 8, 1, 4, 4, 4, 1, seq, present, replay)
    struct.pack_into("<2Q", header, 40, tick, 100000 + seq)
    struct.pack_into("<7I", header, 56, 4, 4, 0, 0, 4, 4, 1)
    path.write_bytes(bytes(header) + bytes(4 * 4 * 4))


def _matrix(index: int, output_delta: int = 0) -> dict:
    key = dict(valid=1, camera=0, model=1, generation=1, sub=0x4000, ordinal=0)
    current = [index] + [0] * 11
    output = [index + output_delta] + [0] * 11
    return dict(
        index=index,
        object=7,
        name="flower_a",
        key=key,
        previousKey=dict(key),
        currentBits=current,
        previousBits=list(current),
        outputBits=output,
        cameraViewBits=[0] * 12,
        cameraBits=[0] * 13,
        submitted=True,
    )


def _write_capture_fixture(
    directory: Path,
    *,
    present_indices: list[int] | None = None,
    alphas: list[float] | None = None,
    matrix_count: int = 1,
    summary_queued: int = 4,
    summary_requested: int = 4,
) -> str:
    frames = directory / "frames"
    frames.mkdir(parents=True)
    present_indices = present_indices or [100, 101, 102, 103]
    alphas = alphas or [0.0, 0.0, 0.125, 0.375]
    rows = [",".join(pixel_evidence.INDEX_FIELDS)]
    proofs = []
    for seq, (present, alpha) in enumerate(zip(present_indices, alphas)):
        tick = 3000 + seq
        replay = int(seq >= 2)
        wall_ns = 100000 + seq
        rows.append(
            ",".join(str(value) for value in (
                seq, present, tick, wall_ns, replay, 4, 4, 4, 4,
                0, 0, 4, 4, 1, 1
            ))
        )
        _write_mfd(frames / f"f{seq:06d}.mfd", seq, present, tick, replay)
        proofs.append(dict(
            seq=seq,
            presentIndex=present,
            captureViTick=tick,
            viTick=tick - 1,
            previousViTick=tick - 2,
            seal=seq + 2,
            previousSeal=seq + 1,
            alpha=alpha,
            interpolationEnabled=replay,
            matrixCount=matrix_count,
            hintOverflow=0,
            previousHintOverflow=0,
            matrices=[_matrix(seq, 1 if replay else 0)],
        ))
    (frames / "index.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    log_lines = [
        f"[AO-DEFERRED-CAPTURE] queued={summary_queued} captured={summary_queued} "
        f"written={summary_queued} bytes=256 error=0",
        f"[AO-DEFERRED-FINISH] requested={summary_requested} queued={summary_queued} complete=1",
        "[AO-DEFERRED-METADATA] count=4 error=0 alpha=natural",
    ]
    log_lines.extend(
        "[AO-DEFERRED-FRAME] " + json.dumps(proof, separators=(",", ":"))
        for proof in proofs
    )
    log = "\n".join(log_lines) + "\n"
    (directory / "game.log").write_text(log, encoding="utf-8")
    return log


class DeferredCaptureEvidenceTests(unittest.TestCase):
    def test_validator_accepts_real_and_naturally_phased_replay_rows(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ao-deferred-evidence-", dir=ROOT / "build") as folder:
            directory = Path(folder)
            log = _write_capture_fixture(directory)
            result = capture.validate_deferred_capture(log, directory, 4, 0, "exited")
            self.assertEqual(result["frames"], 4)
            self.assertEqual(result["real_frames"], 2)
            self.assertEqual(result["replay_frames"], 2)
            self.assertEqual(result["natural_alpha_min"], 0.125)
            self.assertEqual(result["natural_alpha_max"], 0.375)

    def test_device_shutdown_markers_before_completion_fail_after_completion_are_allowed(self) -> None:
        markers = (
            "device lost: normal shutdown",
            "surface present failed: normal shutdown",
        )
        for marker in markers:
            with self.subTest(marker=marker), tempfile.TemporaryDirectory(
                prefix="ao-deferred-device-", dir=ROOT / "build"
            ) as folder:
                directory = Path(folder)
                log = _write_capture_fixture(directory)
                with self.assertRaisesRegex(ValueError, "before completion"):
                    capture.validate_deferred_capture(
                        marker + "\n" + log, directory, 4, 0, "exited"
                    )
                result = capture.validate_deferred_capture(
                    log + marker + "\n", directory, 4, 0, "exited"
                )
                self.assertEqual(result["frames"], 4)

    def test_validator_rejects_missing_file_nonconsecutive_present_forced_phases_metadata_and_summary(self) -> None:
        cases = {
            "missing": "deferred file/index count mismatch",
            "present": "nonconsecutive/nonmonotonic presented frames",
            "phases": "real and naturally phased replay frames",
            "matrix": "matrix inventory mismatch",
            "summary": "incomplete deferred capture",
        }
        for case, expected in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix="ao-deferred-evidence-", dir=ROOT / "build"
            ) as folder:
                directory = Path(folder)
                if case == "present":
                    log = _write_capture_fixture(directory, present_indices=[100, 101, 105, 106])
                elif case == "phases":
                    log = _write_capture_fixture(directory, alphas=[0.0, 0.0, 0.25, 0.5])
                elif case == "matrix":
                    log = _write_capture_fixture(directory, matrix_count=2)
                elif case == "summary":
                    log = _write_capture_fixture(directory, summary_queued=3)
                else:
                    log = _write_capture_fixture(directory)
                    (directory / "frames" / "f000002.mfd").unlink()
                with self.assertRaisesRegex(ValueError, expected):
                    capture.validate_deferred_capture(log, directory, 4, 0, "exited")


class DeferredCadenceBuilderTests(unittest.TestCase):
    def test_builder_uses_separate_output_and_wires_private_transforms(self) -> None:
        source = (ROOT / "tests" / "integration" / "build_ao_cadence_qa.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("OUTPUT = ROOT / 'build/board-cadence-qa'", source)
        self.assertIn("protected = [ROOT/'build/release/mp6native.exe', ROOT/'build/board-qa/release/mp6native.exe']", source)
        self.assertIn("for name, expected in before.items():", source)
        self.assertIn("protected executable changed", source)

        transformed = cadence_builder.transformed_builder()
        self.assertIn("OUTPUT = ROOT / 'build/board-cadence-qa'", transformed)
        self.assertNotIn("OUTPUT = ROOT / 'build/board-qa'", transformed)
        self.assertIn("contents = deferred_interp(contents)", transformed)
        self.assertIn("deferred_dump(Path(source).read_text(encoding='utf-8'))", transformed)
        self.assertIn("ao_deferred_capture_backend.cpp", transformed)

    def test_builder_hash_guard_detects_protected_executable_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ao-deferred-builder-", dir=ROOT / "build") as folder:
            root = Path(folder)
            protected = root / "build" / "release" / "mp6native.exe"
            protected.parent.mkdir(parents=True)
            protected.write_bytes(b"canonical")
            mutation = (
                "from pathlib import Path\n"
                f"Path({str(protected)!r}).write_bytes(b'mutated')\n"
                "raise SystemExit(0)\n"
            )
            with mock.patch.object(cadence_builder, "ROOT", root), \
                 mock.patch.object(cadence_builder, "transformed_builder", return_value=mutation):
                with self.assertRaisesRegex(RuntimeError, "protected executable changed"):
                    cadence_builder.main()
            self.assertEqual(hashlib.sha256(protected.read_bytes()).hexdigest(),
                             hashlib.sha256(b"mutated").hexdigest())


if __name__ == "__main__":
    unittest.main(verbosity=2)
