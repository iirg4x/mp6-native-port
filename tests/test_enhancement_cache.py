"""Cross-platform settings lookup cache: no change to live preference semantics."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]


class EnhancementCache(unittest.TestCase):
    def test_native_lookup_and_live_update_contract(self):
        with tempfile.TemporaryDirectory(prefix='enh-cache-',dir=ROOT/'build') as temp:
            exe=Path(temp)/'test.exe'
            result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-O2','-UNDEBUG',
                '-Dgetenv=mp6_test_getenv','tests/native/enhancement_cache_selftest.c',
                'src/enh/mp6_enhancements.c','-o',str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_both_boot_paths_opt_in_and_cached_values_are_host_owned(self):
        source=(ROOT/'src/main_native.c').read_text()
        self.assertEqual(source.count('mp6_enh_cache_environment();'),2)
        self.assertIn('src/enh/mp6_enhancements.c',build.HOST_STATE_SECTION_SOURCES)
        enh=(ROOT/'src/enh/mp6_enhancements.c').read_text()
        self.assertLess(enh.index('#include "mp6_host_section.h"'),enh.index('static Mp6EnhValues g_resolved'))


if __name__=='__main__':unittest.main()
