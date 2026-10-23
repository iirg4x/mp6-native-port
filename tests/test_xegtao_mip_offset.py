"""CPU contracts for the optional XeGTAO depth-mip offset seam."""

from __future__ import annotations

import inspect
from pathlib import Path
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "tests" / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

import test_xegtao_gpu as harness


class XeGTAOMipOffsetCPU(unittest.TestCase):
    def test_default_equals_explicit_upstream_value(self):
        self.assertEqual(
            harness._pack_constants(1280, 720),
            harness._pack_constants(1280, 720, depth_mip_offset=3.3),
        )
        self.assertEqual(harness._validated_depth_mip_offset(3.3), 3.3)

    def test_offset_changes_only_the_byte88_float(self):
        baseline = harness._pack_constants(1280, 720)
        changed = harness._pack_constants(1280, 720, depth_mip_offset=5.0)
        self.assertEqual(len(baseline), 96)
        self.assertEqual(len(changed), 96)
        self.assertEqual(baseline[:88], changed[:88])
        self.assertEqual(baseline[92:], changed[92:])
        self.assertEqual(
            struct.unpack_from("<f", baseline, 88)[0],
            struct.unpack("<f", struct.pack("<f", 3.3))[0],
        )
        self.assertEqual(struct.unpack_from("<f", changed, 88)[0], 5.0)
        self.assertNotEqual(baseline[88:92], changed[88:92])

    def test_validation_rejects_nonfinite_nonrange_and_nonnumeric_values(self):
        for bad in (float("nan"), float("inf"), float("-inf"), -0.001, 30.001, "offset"):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    harness._validated_depth_mip_offset(bad)
                with self.assertRaises(ValueError):
                    harness._pack_constants(32, 16, depth_mip_offset=bad)

    def test_boundary_values_are_accepted(self):
        self.assertEqual(harness._validated_depth_mip_offset(0), 0.0)
        self.assertEqual(harness._validated_depth_mip_offset(30), 30.0)

    @unittest.skipUnless(harness.np is not None, "optional NumPy dependency is unavailable")
    def test_render_validates_before_gpu_state_and_forwards_keyword(self):
        # Validation happens before quality/pipeline/device access, so a
        # lightweight uninitialized object is sufficient for this CPU seam.
        renderer = harness.Renderer.__new__(harness.Renderer)
        with self.assertRaises(ValueError):
            renderer.render(harness.np.ones((1, 1), dtype=harness.np.float32),
                            depth_mip_offset=31.0)
        signature = inspect.signature(harness.Renderer.render)
        self.assertEqual(signature.parameters["depth_mip_offset"].default, 3.3)
        source = inspect.getsource(harness.Renderer.render)
        self.assertIn("depth_mip_offset: float = 3.3", source)
        self.assertIn("_pack_constants(width, height, aspect, radius, depth_mip_offset)", source)


if __name__ == "__main__":  # pragma: no cover
    unittest.main(verbosity=2)
