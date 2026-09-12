import unittest
from abba_report import groups,parse
def rows():
    return [dict(phase=i,mode=int(i%4 in (1,2)),valid=1,frames=216,draws=21600,
        hits=18000 if i%4 in (1,2) else 0,builds=0,encode_ns=21600000,
        first_ns=1+i*2000000000,last_ns=1+i*2000000000+1075000000) for i in range(8)]
class ReportTests(unittest.TestCase):
    def test_groups(self):
        self.assertEqual(len(groups(rows(),0,7)),2)
        self.assertEqual(groups(rows(),0,7)[0]['presents_percent_change'],0)
        self.assertEqual(groups(rows(),0,7)[0]['baseline']['presents_per_second'],200)
    def test_no_partial(self):
        self.assertEqual(len(groups(rows(),1,7)),1)
        self.assertEqual(len(groups(rows(),0,6)),1)
        self.assertEqual(groups(rows()[1:7],1,6),[])
    def test_pause_or_failed_present(self):
        for change in ({'valid':0},{'frames':215},{'draws':0},{'last_ns':0}):
            data=rows(); data[1].update(change)
            self.assertEqual(len(groups(data,0,7)),1)
    def test_overlap(self):
        data=rows(); data[1]['first_ns']=data[0]['last_ns']
        self.assertEqual(len(groups(data,0,7)),1)
    def test_duplicate_and_invalid_mode(self):
        line='[MP6-BUNDLE-ABBA] phase=0 mode=0 valid=1 frames=216 draws=1 hits=0 builds=0 encode_ns=1 first_ns=1 last_ns=2'
        self.assertEqual(len(parse(line)),1)
        with self.assertRaises(ValueError): parse(line+'\n'+line)
        with self.assertRaises(ValueError): parse(line.replace('mode=0','mode=1'))
        with self.assertRaises(ValueError): parse(line.replace('hits=0','hits=1'))
if __name__ == '__main__': unittest.main()
