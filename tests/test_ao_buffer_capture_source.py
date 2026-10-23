"""CPU-only contracts for the private ordered AO buffer capture build."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests" / "integration"))

import ao_buffer_capture_source as capture  # noqa: E402
import ao_admission_probe as admission  # noqa: E402
import build_ao_buffer_qa as builder  # noqa: E402


AMBIENT = ROOT / "src" / "gx" / "ambient_occlusion.cpp"
NATIVE = ROOT / "src" / "gx" / "xegtao" / "native.hpp"
BACKEND = ROOT / "tests" / "integration" / "ao_buffer_capture.hpp"


class AoBufferSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ambient = AMBIENT.read_text(encoding="utf-8")
        cls.native = NATIVE.read_text(encoding="utf-8")

    def test_native_transform_has_single_copy_seam_and_absolute_includes(self) -> None:
        transformed = capture.instrument_native(self.native, NATIVE.parent)
        self.assertEqual(transformed.count("TextureUsage::CopySrc"), 1)
        self.assertIn("QA_AO_BUFFER_CAPTURE_NATIVE", transformed)
        self.assertNotIn('#include "adapter_shader.hpp"', transformed)
        self.assertIn(str((NATIVE.parent / "adapter_shader.hpp").resolve()).replace("\\", "/"), transformed)
        with self.assertRaisesRegex(ValueError, "already installed"):
            capture.instrument_native(transformed, NATIVE.parent)

    def test_native_aliases_are_captured_before_next_role_overwrites(self) -> None:
        transformed = capture.instrument_native(self.native, NATIVE.parent)
        expected = [
            '"canonical_depth"',
            '"depth_mip", 0',
            '"depth_mip", 1',
            '"depth_mip", 2',
            '"depth_mip", 3',
            '"depth_mip", 4',
            '"normals"',
            '"raw_ao"',
            '"edges"',
            '"final_ao"',
        ]
        positions = [transformed.index(token) for token in expected]
        self.assertEqual(positions, sorted(positions))
        main = transformed.index('"XeGTAO main"')
        raw = transformed.index('"raw_ao"')
        edges = transformed.index('"edges"')
        final = transformed.index('"XeGTAO final denoise"')
        self.assertLess(main, raw)
        self.assertLess(raw, edges)
        self.assertLess(edges, final)

    def test_ambient_transform_metadata_and_finish_order(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ao-buffer-source-") as folder:
            native = Path(folder) / "xegtao" / "native.hpp"
            transformed = capture.instrument_ambient(self.ambient, native, BACKEND)
        self.assertIn("QA_AO_BUFFER_CAPTURE_AMBIENT", transformed)
        self.assertIn("qaTick", transformed)
        self.assertIn("qaCamera", transformed)
        self.assertIn("mp6::qa::ao_buffers::Metadata qaMetadata", transformed)
        self.assertIn("std::memcpy(qaMetadata.params.data()", transformed)
        self.assertIn("std::memcpy(qaMetadata.constants.data()", transformed)
        self.assertIn("std::memcpy(qaMetadata.adapter.data()", transformed)
        begin = transformed.index("mp6::qa::ao_buffers::begin_job")
        clean = transformed.index('"clean_depth"', begin)
        encode = transformed.index("g_xegtao.encode", begin)
        end = transformed.index("mp6::qa::ao_buffers::end_job", encode)
        self.assertLess(begin, clean)
        self.assertLess(clean, encode)
        self.assertLess(encode, end)
        sync = transformed.index("gfx::synchronize();")
        finish = transformed.index("mp6::qa::ao_buffers::finish();")
        self.assertLess(sync, finish)
        # MapAsync belongs only to the external backend header.  It must never
        # be copied into a production/renderer translation unit.
        self.assertNotIn("MapAsync", transformed)
        with self.assertRaisesRegex(ValueError, "already installed"):
            capture.instrument_ambient(transformed, native, BACKEND)

    def test_ambient_transform_follows_real_admission_transform_and_finishes_outside_guard(self) -> None:
        """Exercise the same base AO transform used by build_board_qa.py.

        The admission probe adds a conditional synchronization/summary block
        to shutdown.  Buffer readback must remain after the real synchronization
        boundary and outside that conditional, otherwise an enabled admission
        run can return before the capture backend is finalized.
        """

        admitted = admission.instrument_ao_admission(self.ambient)
        with tempfile.TemporaryDirectory(prefix="ao-buffer-admission-") as folder:
            transformed = capture.instrument_ambient(
                admitted, Path(folder) / "xegtao" / "native.hpp", BACKEND
            )
        self.assertIn(admission.MARKER, transformed)
        self.assertIn("QA_AO_BUFFER_CAPTURE_AMBIENT", transformed)
        self.assertEqual(transformed.count("mp6::qa::ao_buffers::finish();"), 1)
        shutdown = transformed.index('extern "C" void mp6_ao_shutdown(void)')
        finish = transformed.index("mp6::qa::ao_buffers::finish();", shutdown)
        admission_sync = transformed.index("if (qaAdmission) {", shutdown)
        regular_sync = transformed.index("if (initialized && !qaAdmission) gfx::synchronize();", shutdown)
        diag = transformed.index('if (std::getenv("MP6_AO_DIAG"))', shutdown)
        self.assertLess(admission_sync, regular_sync)
        self.assertLess(regular_sync, finish)
        self.assertLess(finish, diag)

        # Locate the conditional's matching brace and prove finish is not
        # nested in it.  This avoids relying only on textual ordering.
        brace = transformed.index("{", admission_sync)
        depth = 0
        close = None
        for index in range(brace, len(transformed)):
            if transformed[index] == "{":
                depth += 1
            elif transformed[index] == "}":
                depth -= 1
                if depth == 0:
                    close = index
                    break
        self.assertIsNotNone(close)
        self.assertFalse(brace < finish < close)

    def test_builder_transform_smoke_and_exact_seams(self) -> None:
        transformed = builder.transformed_builder()
        self.assertIn("OUTPUT = ROOT / 'build/ao-buffer-qa'", transformed)
        self.assertNotIn("OUTPUT = ROOT / 'build/board-qa'", transformed)
        self.assertIn("instrument_ambient(contents, ao_buffer_native_path, ao_buffer_header_path)", transformed)
        self.assertIn("third_party/aurora/lib", transformed)
        self.assertIn("ambient_occlusion.cpp", transformed)
        with self.assertRaisesRegex(ValueError, "expected once"):
            # The helper is deliberately public enough for a drift test while
            # keeping the actual build wrapper's mutation local.
            builder._replace_once("x x", "x", "y", "drift")

    def test_output_isolation_rejects_all_protected_trees(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ao-buffer-root-") as folder:
            root = Path(folder)
            for name in ("release", "board-qa", "board-cadence-qa"):
                with self.subTest(name=name):
                    protected = root / "build" / name
                    protected.mkdir(parents=True)
                    with self.assertRaisesRegex(ValueError, "aliases protected"):
                        builder._assert_output_isolated(root, protected)
            builder._assert_output_isolated(root, root / "build" / "ao-buffer-qa")


if __name__ == "__main__":
    unittest.main(verbosity=2)
