from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/integration'))
from math_profile_source import instrument

class MathProfileSource(unittest.TestCase):
    def test_instruments_all_bridge_wrappers_without_changing_calls(self):
        source=(ROOT/'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
        marker='    if (tEntry > 0) g_phSamples++;'
        source=source.replace(marker,marker+'\n    if (qaBoardAge>=1800 && qaBoardAge<5800) {\n        if (++count==4000) { fprintf(stderr,"test"); }\n    }')
        result=instrument(source)
        self.assertEqual(result.count('unsigned long long begin=0;'),19)
        self.assertIn('u32 result = C_MTXInverse(src, inv);',result)
        self.assertIn('f32 result = C_VECDotProduct(a, b);',result)
        self.assertIn('C_MTXMultVecArray(m, srcBase, dstBase, count);',result)
        self.assertIn('mp6_math_active = qaBoardAge>=1800 && qaBoardAge<5800;',result)
        self.assertEqual(result.count('mp6_math_report();'),1)

if __name__=='__main__': unittest.main()
