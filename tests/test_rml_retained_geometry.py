"""Bounded checks for promotion of reused RmlUi geometry to GPU buffers."""

from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


TARGETS = {
    "lib/gfx/common.cpp",
    "lib/gfx/common.hpp",
    "lib/rmlui/WebGPURenderInterface.cpp",
    "lib/rmlui/WebGPURenderInterface.hpp",
    "lib/rmlui/pipeline.cpp",
    "lib/rmlui/pipeline.hpp",
}


@dataclass
class Resident:
    generation: int


@dataclass
class WorkerResident:
    """Small model of the CPU-published/worker-owned upload state."""

    gpu_ready: bool = False
    upload_attempts: int = 0


class WorkerUploadKernel:
    """Model the staging fallback until the render worker publishes buffers."""

    def __init__(self):
        self.resident = WorkerResident()
        self.staged_draws = 0

    def record_draw(self):
        # The recording thread only reads the publication flag. A not-ready
        # resident must carry staging ranges for this draw.
        if not self.resident.gpu_ready:
            self.staged_draws += 1
            return "staging"
        return "resident"

    def replay_on_worker(self, create_succeeds: bool):
        if not self.resident.gpu_ready:
            self.resident.upload_attempts += 1
            if create_succeeds:
                # Represents release-store publication after both GPU handles
                # have been assigned by the worker.
                self.resident.gpu_ready = True
        return self.resident.gpu_ready


class GeometryPromotionKernel:
    """Small model of frame hotness, device invalidation, and frame pinning."""

    invalid_frame = (1 << 32) - 1

    def __init__(self):
        self.last_frame = 0
        self.has_seen_frame = False
        self.resident: Resident | None = None
        self.pinned_frame = 0
        self.resident_pinned = False
        self.uploads = 0
        self.pins = 0
        self.cpu_mesh = ("vertices", "indices")

    def draw(self, frame: int, generation: int = 1):
        if frame != self.invalid_frame and (not self.has_seen_frame or frame != self.last_frame):
            reused = self.has_seen_frame
            self.last_frame = frame
            self.has_seen_frame = True
            if reused and self.resident is None:
                self.resident = Resident(generation)
                self.uploads += 1
                self.resident_pinned = False

        if self.resident is not None and self.resident.generation != generation:
            # Rebuild from the retained CPU mesh after device recreation.
            self.resident = Resident(generation)
            self.uploads += 1
            self.resident_pinned = False

        if self.resident is not None and frame != self.invalid_frame and (
            not self.resident_pinned or self.pinned_frame != frame
        ):
            self.pinned_frame = frame
            self.resident_pinned = True
            self.pins += 1
        return self.resident


class RmlRetainedGeometryTests(unittest.TestCase):
    def test_first_use_is_staging_then_second_frame_promotes_once(self):
        kernel = GeometryPromotionKernel()
        self.assertIsNone(kernel.draw(0))
        self.assertEqual(kernel.uploads, 0)

        resident = kernel.draw(1)
        self.assertIsNotNone(resident)
        self.assertEqual(kernel.uploads, 1)
        self.assertEqual(kernel.pins, 1)

        # Multiple draws in the same frame reuse the same immutable buffers and
        # retain one strong frame-packet reference, not one per draw.
        self.assertIs(kernel.draw(1), resident)
        self.assertEqual(kernel.uploads, 1)
        self.assertEqual(kernel.pins, 1)

        self.assertIs(kernel.draw(2), resident)
        self.assertEqual(kernel.uploads, 1)
        self.assertEqual(kernel.pins, 2)

    def test_sentinel_frame_uses_staging_instead_of_unpinned_resident(self):
        kernel = GeometryPromotionKernel()
        kernel.draw(0)
        kernel.draw(1)
        self.assertIsNotNone(kernel.resident)
        self.assertEqual(kernel.pins, 1)
        # UINT32_MAX is a gfx sentinel, not a normal frame identity. It must
        # not suppress the strong frame-packet pin or route a raw pointer.
        kernel.draw(kernel.invalid_frame)
        self.assertEqual(kernel.pins, 1)

    def test_device_generation_rebuilds_from_cpu_source(self):
        kernel = GeometryPromotionKernel()
        kernel.draw(0)
        old = kernel.draw(1, generation=7)
        self.assertEqual(old.generation, 7)
        self.assertEqual(kernel.cpu_mesh, ("vertices", "indices"))

        rebuilt = kernel.draw(2, generation=8)
        self.assertIsNot(rebuilt, old)
        self.assertEqual(rebuilt.generation, 8)
        self.assertEqual(kernel.uploads, 2)

    def test_worker_upload_publishes_after_first_staged_draw(self):
        kernel = WorkerUploadKernel()
        self.assertEqual(kernel.record_draw(), "staging")
        self.assertFalse(kernel.resident.gpu_ready)
        self.assertTrue(kernel.replay_on_worker(create_succeeds=True))
        self.assertEqual(kernel.resident.upload_attempts, 1)
        self.assertEqual(kernel.record_draw(), "resident")
        self.assertEqual(kernel.staged_draws, 1)

    def test_worker_upload_failure_keeps_staging_fallback(self):
        kernel = WorkerUploadKernel()
        self.assertEqual(kernel.record_draw(), "staging")
        self.assertFalse(kernel.replay_on_worker(create_succeeds=False))
        self.assertEqual(kernel.record_draw(), "staging")
        self.assertTrue(kernel.replay_on_worker(create_succeeds=True))
        self.assertEqual(kernel.record_draw(), "resident")
        self.assertEqual(kernel.resident.upload_attempts, 2)
        self.assertEqual(kernel.staged_draws, 2)

    def test_patch_hunks_and_generated_paths_are_exact(self):
        generated_root = ROOT / "third_party/aurora"
        cpp = (generated_root / "lib/rmlui/WebGPURenderInterface.cpp").read_text(encoding="utf-8")
        pipeline = (generated_root / "lib/rmlui/pipeline.cpp").read_text(encoding="utf-8")
        common = (generated_root / "lib/gfx/common.cpp").read_text(encoding="utf-8")
        self.assertIn("create_resident_geometry", cpp)
        self.assertIn("gfx::current_frame()", cpp)
        self.assertIn("gfx::retain_frame_resource", cpp)
        self.assertIn("m_deviceGeneration", cpp)
        self.assertIn("++m_deviceGeneration", cpp)
        self.assertIn("hasSeenFrame", cpp)
        self.assertIn("data.residentGeometry", pipeline)
        self.assertIn("retainedFrameResources", common)

    def test_worker_upload_source_keeps_gpu_creation_off_recording_thread(self):
        render_interface = (
            ROOT / "third_party/aurora/lib/rmlui/WebGPURenderInterface.cpp"
        ).read_text(encoding="utf-8")
        pipeline = (
            ROOT / "third_party/aurora/lib/rmlui/pipeline.cpp"
        ).read_text(encoding="utf-8")

        create_start = render_interface.index("create_resident_geometry")
        create_end = render_interface.index("// UniformBlock", create_start)
        cpu_promotion = render_interface[create_start:create_end]
        self.assertNotIn("CreateBuffer", cpu_promotion)
        self.assertNotIn("mappedAtCreation", cpu_promotion)
        self.assertIn("vertexBytes", cpu_promotion)
        self.assertIn("std::memcpy", cpu_promotion)

        upload_start = pipeline.index("bool ensure_resident_geometry")
        upload_end = pipeline.index("constexpr std::string_view vertexSource", upload_start)
        worker_upload = pipeline[upload_start:upload_end]
        self.assertIn("render_worker::is_worker_thread()", worker_upload)
        self.assertIn("mappedAtCreation", worker_upload)
        self.assertIn("gpuReady.store(true, std::memory_order_release)", worker_upload)
        self.assertIn("gpuReady.load(std::memory_order_acquire)", pipeline)
        self.assertIn("residentReady", render_interface)

if __name__ == "__main__":
    unittest.main()
