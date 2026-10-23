"""CPU contracts for QA radius-only changes and bounded seam masks."""
import sys
import struct
from pathlib import Path
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parent/"integration"))
from test_xegtao_gpu import np, _pack_constants, _validated_radius
from analyze_xegtao_radius import segment_band, stats


class RadiusContracts(unittest.TestCase):
    def test_default_is_unchanged_and_only_radius_word_changes(self):
        reference = _pack_constants(1280,720,1.59)
        self.assertEqual(reference,_pack_constants(1280,720,1.59,radius=32))
        changed = _pack_constants(1280,720,1.59,radius=16)
        self.assertEqual(len(changed),96)
        self.assertEqual(changed[:56],reference[:56])
        self.assertEqual(changed[60:],reference[60:])
        self.assertEqual(struct.unpack_from("<f",changed,56)[0],16)

    def test_invalid_radius_rejected(self):
        for value in (0,-1,float('nan'),float('inf'),1e100,1e-100,'wide'):
            with self.subTest(value=value),self.assertRaises(ValueError):
                _validated_radius(value)

    def test_segment_mask_has_fixed_footprint_and_trim(self):
        mask=segment_band((21,21),(0,10),(20,10),2,trim=.1)
        self.assertEqual(int(mask.sum()),17*5)
        self.assertTrue(mask[8,2])
        self.assertFalse(mask[7,2])
        self.assertFalse(mask[10,1])
        np.testing.assert_array_equal(mask,segment_band((21,21),(20,10),(0,10),2,trim=.1))

    def test_metric_rejects_empty_or_invalid_data(self):
        values=np.ones((3,3),dtype=np.float32)
        with self.assertRaises(ValueError):
            stats(values,np.zeros((3,3),dtype=bool))
        values[1,1]=np.nan
        with self.assertRaises(ValueError):
            stats(values,np.ones((3,3),dtype=bool))


if __name__=='__main__':
    unittest.main()
