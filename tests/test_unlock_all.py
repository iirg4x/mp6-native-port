"""The Unlock All mod: query-layer override that never reaches the save."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT=Path(__file__).resolve().parents[1]


class UnlockAll(unittest.TestCase):
    def test_override_contract_and_save_image(self):
        with tempfile.TemporaryDirectory(prefix='unlock-all-',dir=ROOT/'build') as temp:
            original=(Path(build.DECOMP)/'src/game/gamework.c').read_text(encoding='utf-8')
            patch=(ROOT/'compat/decomp/src/game/gamework.c.patch').read_text(encoding='utf-8')
            gamework=Path(temp)/'gamework.c'
            gamework.write_text(apply_patches.apply_unified_diff(original,patch,'gamework.c'),encoding='utf-8')
            exe=Path(temp)/'test.exe'
            result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-O2','-UNDEBUG',
                '-Dgetenv=mp6_test_getenv','tests/native/unlock_all_selftest.c',str(gamework),
                'src/os/unlock_all.c','src/os/save_endian.c','-o',str(exe)],
                cwd=ROOT,capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_store_is_host_owned_and_linked_everywhere(self):
        self.assertIn('src/os/unlock_all.c',build.HOST_STATE_SECTION_SOURCES)
        self.assertIn('src/os/unlock_all.c',build.PLATFORM_SOURCES_COMMON)
        store=(ROOT/'src/os/unlock_all.c').read_text()
        self.assertLess(store.index('#include "mp6_host_section.h"'),store.index('static int g_setting'))

    def test_setting_is_persisted_and_published(self):
        launcher=(ROOT/'src/gx/ui/launcher_core.cpp').read_text()
        self.assertIn('"game.unlock_all"',launcher)
        self.assertIn('mp6_unlock_all_set_enabled(g_cfg.unlockAll);',launcher)
        self.assertIn('.key = "Unlock All"',(ROOT/'src/gx/ui/settings.cpp').read_text())


if __name__=='__main__':unittest.main()
