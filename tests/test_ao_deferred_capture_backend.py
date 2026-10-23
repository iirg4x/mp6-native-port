"""No-device contract checks for the QA-only deferred AO capture backend."""

from pathlib import Path
import re
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "integration" / "ao_deferred_capture_backend.cpp"


class DeferredCaptureBackendSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_exports_and_limits_are_qa_only(self):
        self.assertIn('#include "webgpu/gpu.hpp"', self.source)
        self.assertIn('#include "gfx/render_worker.hpp"', self.source)
        self.assertRegex(
            self.source,
            r'extern\s+"C"\s+int\s+mp6_qa_deferred_capture_enqueue\s*\(',
        )
        self.assertRegex(
            self.source,
            r'extern\s+"C"\s+int\s+mp6_qa_deferred_capture_finish\s*\(',
        )
        self.assertRegex(self.source, r"kMaxFrames\s*=\s*128")
        self.assertRegex(self.source, r"kMaxPaddedBytes\s*=\s*64ull\s*\*\s*1024ull\s*\*\s*1024ull")
        self.assertIn("std::chrono::steady_clock", self.source)
        self.assertIn("kMapDeadlineNs", self.source)
        self.assertIn('m_error = "first_seq_not_zero"', self.source)

    def test_enqueue_is_deferred_and_copies_request(self):
        enqueue = self.source[self.source.index("extern \"C\" int mp6_qa_deferred_capture_enqueue") :]
        enqueue_body = enqueue[: enqueue.index("extern \"C\" int mp6_qa_deferred_capture_finish")]
        self.assertIn("std::string{outputDir}", enqueue_body)
        self.assertIn("request = std::move(request)", enqueue_body)
        self.assertIn("render_worker::enqueue_work", enqueue_body)
        self.assertNotIn("render_worker::synchronize", enqueue_body)
        executable_enqueue = re.sub(r"//.*", "", enqueue_body)
        self.assertNotIn("WaitAny", executable_enqueue)
        self.assertNotIn("fopen", enqueue_body)
        self.assertNotIn("fwrite", enqueue_body)
        self.assertNotIn("MapAsync", enqueue_body)

    def test_copy_and_map_lifecycles_are_separate(self):
        capture = self.source[self.source.index("void capture(FrameRequest request)") : self.source.index("void start_mapping()")]
        mapping = self.source[self.source.index("void start_mapping()") : self.source.index("bool has_error()")]
        self.assertIn("present_source()", capture)
        self.assertIn("CopyTextureToBuffer", capture)
        self.assertIn("g_queue.Submit", capture)
        self.assertNotIn("MapAsync", capture)
        self.assertIn("MapAsync", mapping)
        self.assertIn("std::make_shared<MapCompletion>", mapping)
        self.assertIn("completion = frame.completion", mapping)
        self.assertIn("m_frames.push_back", capture)
        self.assertIn("readback = std::move(readback)", capture)
        self.assertIn("mapReleased", self.source)

    def test_fail_closed_guards_and_single_sync(self):
        self.assertIn('m_error = "capture_dropped"', self.source)
        self.assertIn('m_error = "geometry_changed"', self.source)
        self.assertIn('m_error = "noncontiguous_present_index"', self.source)
        self.assertIn('m_error = "readback_budget"', self.source)
        self.assertIn('fail("unsupported_format")', self.source)
        self.assertIn('fail("no_present_source")', self.source)
        self.assertIn('m_error = "map_submit"', self.source)
        self.assertIn('"file_write"', self.source)
        self.assertIn('[AO-DEFERRED-CAPTURE] queued=%u captured=%u written=%u bytes=%llu error=%d', self.source)
        self.assertIn('[AO-DEFERRED-ERROR]', self.source)
        self.assertIn("std::filesystem::u8path", self.source)
        self.assertIn("_wfopen(path.c_str()", self.source)
        self.assertEqual(self.source.count("render_worker::synchronize();"), 1)
        self.assertIn("g_finished = true", self.source)
        self.assertIn("session->release_buffers()", self.source)

    def test_standard_dump_header_and_index_contract(self):
        self.assertIn('std::memcpy(header.data(), "MP6FDUMP", 8)', self.source)
        for offset, field in (
            (8, "1u"),
            (20, "kBytesPerPixel"),
            (80, "1u"),
        ):
            self.assertIn(f"put_u32(header, {offset}, {field})", self.source)
        self.assertIn("seq,present_index,tick,wall_ns,replay,width,height,src_width,src_height,", self.source)
        self.assertIn('"crop_x,crop_y,crop_w,crop_h,downscale,format\\n"', self.source)
        self.assertIn('"f%06u.mfd"', self.source)

    def test_header_serializer_shape_without_gpu(self):
        """The 96-byte little-endian layout is independently checked here."""
        header = bytearray(96)
        header[:8] = b"MP6FDUMP"
        fields32 = {
            8: 1,
            12: 7,
            16: 5,
            20: 4,
            24: 3,
            28: 11,
            32: 42,
            36: 1,
            56: 320,
            60: 240,
            64: 4,
            68: 6,
            72: 7,
            76: 5,
            80: 1,
        }
        for offset, value in fields32.items():
            struct.pack_into("<I", header, offset, value)
        struct.pack_into("<Q", header, 40, 1234)
        struct.pack_into("<Q", header, 48, 5678)
        self.assertEqual(len(header), 96)
        self.assertEqual(bytes(header[:8]), b"MP6FDUMP")
        self.assertEqual(struct.unpack_from("<I", header, 8)[0], 1)
        self.assertEqual(struct.unpack_from("<I", header, 80)[0], 1)
        self.assertEqual(struct.unpack_from("<Q", header, 40)[0], 1234)
        self.assertEqual(struct.unpack_from("<Q", header, 48)[0], 5678)
        self.assertEqual(header[84:], b"\0" * 12)


if __name__ == "__main__":
    unittest.main()
