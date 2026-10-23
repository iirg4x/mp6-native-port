"""Pure-Python selfchecks for the QA-only AO buffer admission/receipt parser."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tests.integration.ao_buffer_capture import (
    EXPECTED_EXECUTABLE,
    buffer_capture_error,
    validate_buffer_capture,
)


class AoBufferCaptureTests(unittest.TestCase):
    def _args(self, **overrides):
        values = dict(
            ao_buffers_tick=3000,
            ticks=5000,
            tick_hz=60,
            unlocked_fps=False,
            exe=str(EXPECTED_EXECUTABLE),
            ao=2,
            implementation="xegtao",
            xegtao_quality="high",
            xegtao_denoise=2,
            xegtao_resolution="full",
            xegtao_depth_storage="r32float",
            xegtao_batch_compute=False,
            xegtao_prepared_canonical=False,
            xegtao_init_failure=False,
            resize_ao=False,
            ao_cycle=None,
            raw_ao_depth=False,
            unshaded_ao=False,
            foreground_ao=False,
            water_ao=False,
            ao_fi_matrix_range=None,
            ao_fi_pixels=None,
            deferred_capture=None,
            save_at=None,
            load_at=None,
            state_path=None,
            capture_frames=0,
            gpu_validation=False,
        )
        values.update(overrides)
        return type("Args", (), values)()

    def _fixture(self):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        executable = root / EXPECTED_EXECUTABLE
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"synthetic native executable")
        source = root / "tests" / "integration" / "ao_buffer_capture.hpp"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"synthetic backend header")
        generated = root / "build" / "ao-buffer-qa" / "generated_capture.cpp"
        generated.parent.mkdir(parents=True, exist_ok=True)
        generated.write_bytes(b"synthetic generated source")
        binary_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
        source_hashes = {"tests/integration/ao_buffer_capture.hpp":
                         hashlib.sha256(source.read_bytes()).hexdigest()}
        generated_hashes = {"generated_capture.cpp":
                            hashlib.sha256(generated.read_bytes()).hexdigest()}
        ready = {
            "ao_buffers": {
                "executable": str(executable),
                "exe": str(executable),
                "exe_sha256": binary_hash,
                "sources": source_hashes,
                "source_hashes": source_hashes,
                "private_generated_sources": generated_hashes,
                "private_generated_hashes": generated_hashes,
                "protected_executables": {},
                "protected_hashes": {},
            }
        }
        ready_path = root / "build" / "ao-buffer-qa" / "build-ready.json"
        ready_path.write_text(json.dumps(ready, sort_keys=True) + "\n", encoding="utf-8")

        capture = root / "run" / "ao-buffers"
        capture.mkdir(parents=True)
        jobs = []
        stages = (
            ("clean_depth", 0, "r32float"),
            ("canonical_depth", 0, "r32float"),
            ("depth_mip", 0, "r32float"),
            ("depth_mip", 1, "r32float"),
            ("depth_mip", 2, "r32float"),
            ("depth_mip", 3, "r32float"),
            ("depth_mip", 4, "r32float"),
            ("normals", 0, "r32uint"),
            ("raw_ao", 0, "r32uint"),
            ("edges", 0, "r32float"),
            ("final_ao", 0, "r32uint"),
        )
        for job_index in range(2):
            width, height = 32, 16
            planes = []
            for stage, mip, texture_format in stages:
                plane_width, plane_height = width >> mip, height >> mip
                byte_count = plane_width * plane_height * 4
                name = f"j{job_index:03d}_{stage}_m{mip}.bin"
                (capture / name).write_bytes(bytes([job_index + mip]) * byte_count)
                planes.append({
                    "stage": stage,
                    "mip": mip,
                    "width": plane_width,
                    "height": plane_height,
                    "format": texture_format,
                    "file": name,
                    "bytes": byte_count,
                })
            jobs.append({
                "frame": 101 + job_index,
                "tick": 3000 + job_index,
                "camera": 0,
                "width": width,
                "height": height,
                "params_bits": list(range(20)),
                "constants_bits": list(range(24)),
                "adapter_bits": list(range(12)),
                "planes": planes,
            })
        index = {"version": 1, "jobs": jobs, "complete": True}
        (capture / "index.json").write_text(
            json.dumps(index, separators=(",", ":")) + "\n", encoding="utf-8"
        )
        return temporary, root, executable, binary_hash, capture.parent

    def test_disabled_capture_is_inert(self):
        args = self._args(ao_buffers_tick=None, exe="not-an-executable")
        self.assertIsNone(buffer_capture_error(args, Path("C:/does-not-exist")))
        self.assertIsNone(validate_buffer_capture(
            args, Path("C:/does-not-exist"), Path("C:/does-not-exist"),
            "wrong.exe", "bad", 7, "timeout", "",
        ))

    def test_admission_requires_canonical_paced_configuration(self):
        root = Path("C:/synthetic-repo")
        for overrides, needle in (
            ({"ao_buffers_tick": 2999}, "3000"),
            ({"ao_buffers_tick": 4999}, "ticks-1"),
            ({"tick_hz": 0}, "paced"),
            ({"unlocked_fps": True}, "unlocked"),
            ({"ao": 1}, "ao=2"),
            ({"implementation": "legacy"}, "implementation"),
            ({"xegtao_quality": "ultra"}, "quality"),
            ({"xegtao_batch_compute": True}, "batch"),
            ({"ao_cycle": 3000}, "ao_cycle"),
            ({"ao_fi_pixels": "3000:3001:1"}, "ao_fi_pixels"),
            ({"save_at": 3000}, "save_at"),
        ):
            with self.subTest(overrides=overrides):
                error = buffer_capture_error(self._args(**overrides), root)
                self.assertIsNotNone(error)
                self.assertIn(needle, error)

    def test_admission_allows_validation_and_screenshot_capture(self):
        root = Path("C:/synthetic-repo")
        for overrides in (
            {"gpu_validation": True},
            {"capture_frames": 1},
        ):
            with self.subTest(overrides=overrides):
                self.assertIsNone(buffer_capture_error(self._args(**overrides), root))

    def test_valid_bundle_returns_hashes_and_diagnostic_scope(self):
        temporary, root, executable, binary_hash, directory = self._fixture()
        self.addCleanup(temporary.cleanup)
        evidence = validate_buffer_capture(
            self._args(), root, directory, executable, binary_hash, 0, "exited",
            "[AO-BUFFERS] jobs=2 planes=22 complete=1 bytes=8192\n",
        )
        self.assertEqual(evidence["frames"], [101, 102])
        self.assertEqual(len(evidence["plane_sha256"]), 22)
        self.assertEqual(evidence["dimensions"], {"width": 32, "height": 16})
        self.assertIn("Diagnostic-only", evidence["scope"])
        self.assertEqual(evidence["binary_sha256"], binary_hash)

    def test_validator_rejects_native_error_and_shape_drift(self):
        temporary, root, executable, binary_hash, directory = self._fixture()
        self.addCleanup(temporary.cleanup)
        args = self._args()
        with self.assertRaisesRegex(ValueError, "AO-BUFFERS-ERROR"):
            validate_buffer_capture(
                args, root, directory, executable, binary_hash, 0, "exited",
                "[AO-BUFFERS-ERROR] map_failure\n",
            )
        with self.assertRaisesRegex(ValueError, "completion marker"):
            validate_buffer_capture(
                args, root, directory, executable, binary_hash, 0, "exited",
                "[AO-BUFFERS] jobs=2 planes=21 complete=1 bytes=8192\n",
            )
        index_path = directory / "ao-buffers" / "index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["jobs"][0]["planes"][1]["format"] = "r32uint"
        index_path.write_text(json.dumps(index) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "format mismatch"):
            validate_buffer_capture(
                args, root, directory, executable, binary_hash, 0, "exited",
                "[AO-BUFFERS] jobs=2 planes=22 complete=1 bytes=8192\n",
            )

    def test_validator_rejects_outside_extra_files_and_unclean_exit(self):
        temporary, root, executable, binary_hash, directory = self._fixture()
        self.addCleanup(temporary.cleanup)
        args = self._args()
        with self.assertRaisesRegex(ValueError, "did not exit cleanly"):
            validate_buffer_capture(
                args, root, directory, executable, binary_hash, 1, "exited",
                "[AO-BUFFERS] jobs=2 planes=22 complete=1 bytes=8192\n",
            )
        (directory / "ao-buffers" / "unexpected.bin").write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "unexpected file"):
            validate_buffer_capture(
                args, root, directory, executable, binary_hash, 0, "exited",
                "[AO-BUFFERS] jobs=2 planes=22 complete=1 bytes=8192\n",
            )


if __name__ == "__main__":
    unittest.main()
