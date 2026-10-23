"""CPU-only checks for the shared XeGTAO Hilbert lookup table."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "tests" / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

from test_xegtao_gpu import (  # noqa: E402
    XE_HILBERT_AREA,
    XE_HILBERT_WIDTH,
    hilbert_index,
    hilbert_lut_values,
    np as _np,
)


_UINT32_MASK = 0xFFFFFFFF


def _unsigned_reference(pos_x: int, pos_y: int) -> int:
    """Independent direct oracle mirroring XeGTAO's uint arithmetic."""

    x = int(pos_x) & _UINT32_MASK
    y = int(pos_y) & _UINT32_MASK
    result = 0
    level = XE_HILBERT_WIDTH // 2
    while level:
        x_bit = 1 if x & level else 0
        y_bit = 1 if y & level else 0
        result = (result + level * level * ((3 * x_bit) ^ y_bit)) & _UINT32_MASK
        if not y_bit:
            if x_bit:
                x = (XE_HILBERT_WIDTH - 1 - x) & _UINT32_MASK
                y = (XE_HILBERT_WIDTH - 1 - y) & _UINT32_MASK
            x, y = y, x
        level //= 2
    return result


class XeGTAOHilbertLut(unittest.TestCase):
    @unittest.skipUnless(_np is not None, "NumPy is unavailable")
    def test_lut_is_a_4096_entry_permutation(self):
        self.assertEqual(XE_HILBERT_WIDTH, 64)
        self.assertEqual(XE_HILBERT_AREA, 4096)
        lut = hilbert_lut_values()
        self.assertEqual(lut.shape, (XE_HILBERT_WIDTH, XE_HILBERT_WIDTH))
        self.assertEqual(lut.dtype.kind, "u")
        values = [int(value) for value in lut.reshape(-1)]
        self.assertEqual(len(values), XE_HILBERT_AREA)
        self.assertEqual(set(values), set(range(XE_HILBERT_AREA)))

    def test_hilbert_index_matches_independent_unsigned_reference_and_fixtures(self):
        for y in range(XE_HILBERT_WIDTH):
            for x in range(XE_HILBERT_WIDTH):
                with self.subTest(x=x, y=y):
                    self.assertEqual(hilbert_index(x, y), _unsigned_reference(x, y))

        # Independent anchors catch an accidental self-consistent permutation
        # (for example, swapping the row/column convention in both helpers).
        expected = {
            (0, 0): 0,
            (1, 0): 1,
            (0, 1): 3,
            (1, 1): 2,
            (2, 0): 14,
            (32, 0): 3754,
            (63, 0): 4095,
            (0, 63): 1365,
            (63, 63): 2730,
            (7, 13): 108,
            (40, 25): 3371,
        }
        for (x, y), value in expected.items():
            with self.subTest(fixture=(x, y)):
                self.assertEqual(hilbert_index(x, y), value)

    @unittest.skipUnless(_np is not None, "NumPy is unavailable")
    def test_lut_values_match_the_cpu_index(self):
        lut = hilbert_lut_values()
        for y in range(XE_HILBERT_WIDTH):
            for x in range(XE_HILBERT_WIDTH):
                with self.subTest(x=x, y=y):
                    self.assertEqual(int(lut[y, x]), hilbert_index(x, y))

    def test_direct_unsigned_reference_is_periodic_beyond_lut_width(self):
        # Odd viewport dimensions exercise the same coordinates the GPU main
        # pass reduces with ``pixCoord % XE_HILBERT_WIDTH`` before Load().
        for width, height in ((65, 67), (127, 73), (257, 145)):
            for y in range(height):
                for x in range(width):
                    with self.subTest(size=(width, height), x=x, y=y):
                        self.assertEqual(
                            hilbert_index(x, y),
                            _unsigned_reference(x % XE_HILBERT_WIDTH, y % XE_HILBERT_WIDTH),
                        )


if __name__ == "__main__":
    unittest.main()
