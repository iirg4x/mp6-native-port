"""Android link binding, exported ABI and save-state layout identity."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin'
BIND = '-Wl,-Bsymbolic-non-weak-functions'


class AndroidLinkPolicy(unittest.TestCase):
    def test_graphical_and_headless_binding_policy(self):
        for windowed in (False,True):
            flags = build.android_game_link_flags(windowed)
            self.assertEqual(flags.count(BIND),1)
            self.assertIn('-Wl,-z,max-page-size=16384',flags)
            self.assertNotIn('-Wl,-Bsymbolic',flags)
            self.assertNotIn('-Wl,-Bsymbolic-functions',flags)
            self.assertEqual('-Wl,-u,JNI_OnLoad' in flags,windowed)

    def test_layout_stamp_includes_link_policy_order_and_every_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            obj, aurora, nod = [root/name for name in ('game.o','aurora.a','nod.a')]
            for path in (obj,aurora,nod):
                path.write_bytes(path.name.encode())
            def stamp(flags=None, items=None, driver='clang++'):
                return build.android_link_stamp(driver,[str(obj)],flags or [BIND],
                    items or [str(aurora),str(nod),'-llog'])
            initial = stamp()
            self.assertEqual(initial,stamp())
            self.assertRegex(initial,r'^[0-9a-f]{40}$')
            self.assertNotEqual(initial,stamp(flags=['-Wl,-Bsymbolic-functions']))
            self.assertNotEqual(initial,stamp(items=[str(nod),str(aurora),'-llog']))
            self.assertNotEqual(initial,stamp(driver='different-clang++'))
            for path in (obj,aurora,nod):
                old = path.read_bytes()
                path.write_bytes(old+b'changed')
                self.assertNotEqual(initial,stamp(),path.name)
                path.write_bytes(old)
            obj.unlink()
            with self.assertRaises(OSError):
                stamp()

    @unittest.skipUnless((BIN/'clang.exe').exists(),'local Android NDK required')
    def test_real_aarch64_linker_preserves_weak_import_data_and_exports(self):
        with tempfile.TemporaryDirectory(prefix='android-binding-',dir=ROOT/'build') as temp:
            root = Path(temp)
            obj = root/'fixture.o'
            subprocess.run([str(BIN/'clang.exe'),'-target',build.ANDROID_TRIPLE,
                '-O0','-fPIC','-c',str(ROOT/'tests/native/android_link_binding.c'),'-o',str(obj)],
                check=True,capture_output=True,timeout=30)
            results = {}
            for bound in (False,True):
                elf = root/('bound.so' if bound else 'baseline.so')
                flags = build.android_game_link_flags(False)
                if not bound:
                    flags.remove(BIND)
                subprocess.run([str(BIN/'clang.exe'),'-target',build.ANDROID_TRIPLE,
                    '-shared',str(obj),'-o',str(elf),*flags],
                    check=True,capture_output=True,timeout=30)
                def read(*args):
                    return subprocess.check_output([str(BIN/'llvm-readelf.exe'),*args,str(elf)],text=True)
                relocs = read('--relocs','--wide')
                dynamic = read('--dyn-syms','--wide')
                results[bound] = relocs
                for name in ('strong_function','weak_function','public_data','JNI_OnLoad'):
                    self.assertRegex(dynamic,r'(?m)^\s*\d+:.*\b(?:FUNC|OBJECT)\s+(?:GLOBAL|WEAK)\s+DEFAULT\s+\d+\s+'+name+r'$')
                self.assertRegex(relocs,r'R_AARCH64_JUMP_SLOT[^\n]+\bweak_function\b')
                self.assertRegex(relocs,r'R_AARCH64_JUMP_SLOT[^\n]+\bputs\b')
                self.assertRegex(relocs,r'R_AARCH64_GLOB_DAT[^\n]+\bpublic_data\b')
                self.assertRegex(relocs,r'R_AARCH64_GLOB_DAT[^\n]+\bweak_function\b')
            self.assertRegex(results[False],r'R_AARCH64_JUMP_SLOT[^\n]+\bstrong_function\b')
            self.assertRegex(results[False],r'R_AARCH64_GLOB_DAT[^\n]+\bstrong_function\b')
            self.assertNotRegex(results[True],r'R_AARCH64_(?:JUMP_SLOT|GLOB_DAT)[^\n]+\bstrong_function\b')


if __name__=='__main__':
    unittest.main()
