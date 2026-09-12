import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent/'integration'))
from engine_benchmark_metrics import gpu_window, texture_upload_window

class EngineGpuMetrics(unittest.TestCase):
    def test_texture_upload_samples(self):
        self.assertIsNone(texture_upload_window('old report'))
        self.assertEqual(texture_upload_window('[ENGINE-TEXTURES] samples=4000 mean_upload_bytes=0.000')['mean_bytes'],0)
        self.assertEqual(texture_upload_window('[ENGINE-TEXTURES] samples=4000 mean_upload_bytes=14.125')['mean_bytes'],14.125)

    def test_reject_bad_texture_upload_samples(self):
        good='[ENGINE-TEXTURES] samples=4000 mean_upload_bytes=1.000'
        for log in (good+'\n'+good,good.replace('4000','3999'),good.replace('1.000','nan'),
                    good.replace('1.000','-1.0'),good+' trailing',good.replace('1.000','1.2.3')):
            with self.subTest(log=log):
                with self.assertRaises(ValueError):texture_upload_window(log)

    good='[ENGINE-GPU] status=2 samples=120 dropped=3 span=1.2 between=0.1\n[ENGINE-GPU-PASS] EFB avg=1.0 max=1.1\n'
    def test_completed_window(self):
        result=gpu_window(self.good)
        self.assertEqual(result['passes']['EFB']['average_ms'],1)
        self.assertEqual(result['dropped_frames'],3)
        self.assertIn('120',result['scope'])
    def test_reject_incomplete_or_duplicate(self):
        for log in ('',self.good.replace('status=2','status=0'),self.good.replace('samples=120','samples=119'),
                    self.good+self.good,self.good+'[ENGINE-GPU-PASS] EFB avg=1.0 max=1.1\n',
                    self.good.replace('max=1.1','max=0.9'),self.good.replace('between=0.1','between=2.0')):
            with self.subTest(log=log):
                with self.assertRaises(ValueError):gpu_window(log)

if __name__=='__main__':unittest.main()
