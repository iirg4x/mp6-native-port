"""Recovered Star math/flags and explicit-position native ABI regressions."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]


def source():
    dep = Path(apply_patches.require_pinned_decomp())
    original = (dep/'src/board/star.c').read_text()
    patch = (ROOT/'compat/decomp/src/board/star.c.patch').read_text()
    return original, apply_patches.apply_unified_diff(original, patch)


class BoardStarNative(unittest.TestCase):
    def compile_run(self, subject, fixture, expected=0):
        with tempfile.TemporaryDirectory(prefix='star-native-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            (folder/'star_subject.inc').write_text(subject)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG',
                '-I'+str(folder), '-I'+str(ROOT/'include'),
                str(ROOT/'tests/native'/fixture), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            if expected == 0:
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            else:
                self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
                self.assertIn('Assertion failed:', result.stderr)

    def test_recovered_awards_clamp_and_keep_pacing_and_display_rules(self):
        _, text = source()
        subject = '\n'.join(function(text, name) for name in
            ('StarAdd', 'mbStarAddProcExec', 'mbStarAddDispExec', 'mbStarAddExec'))
        self.compile_run(subject, 'board_star_award_selftest.c')
        self.compile_run(subject.replace('999 - mbPlayerStarGet(playerNo)',
                                         '998 - mbPlayerStarGet(playerNo)'),
                         'board_star_award_selftest.c', expected=1)

    def test_positioned_awards_do_not_read_host_pointer_as_vector(self):
        original, text = source()
        def subject(s):
            result = []
            for kind in ('Star', 'Ztar'):
                body = function(s, 'mb'+kind+'GetMain')
                call = re.findall(r'objNo = '+kind+r'PlayerCreate\(playerNo, [^;]+;', body)
                selected = [c for c in call if ', pos)' in c or '(HuVecF *)&pos' in c]
                self.assertEqual(len(selected), 1)
                result.append('static int test_'+kind+'(int playerNo, HuVecF *pos) {\n'
                              'int objNo;\n'+selected[0]+'\nreturn objNo;\n}')
            return '\n'.join(result)
        self.compile_run(subject(text), 'board_star_position_selftest.c')
        # The oracle must reject both unadapted host-pointer-address calls.
        for name in ('Star', 'Ztar'):
            mutant = subject(text).replace(name+'PlayerCreate(playerNo, pos)',
                                           name+'PlayerCreate(playerNo, (HuVecF *)&pos)')
            self.compile_run(mutant, 'board_star_position_selftest.c', expected=1)
        self.assertIn('(HuVecF *)&pos', original)

    def test_flags_constants_and_guide_signature_follow_owners(self):
        original, text = source()
        for name in ('mbStarGetMain', 'mbZtarGetMain'):
            body = function(text, name)
            self.assertIn('_SetFlag(FLAG_BOARD_STAR_RESET)', body)
            self.assertIn('_ClearFlag(FLAG_BOARD_STAR_RESET)', body)
            self.assertNotIn('FLAG_BOARD_TURN_NOSTART', body)
        self.assertIn('int mbGuideModelGet(OMOBJ *obj);', text)
        provider = (Path(build.DECOMP)/'src/board/guide.c').read_text()
        self.assertRegex(provider, r'\bint mbGuideModelGet\(OMOBJ \*obj\)')
        self.assertIn('void mbStarMasuNextSet(int masuId)', original)
        self.assertIn('void mbStarMasuNextSet(int masuId);',
            (Path(build.DECOMP)/'include/REL/w01Dll_world01.h').read_text())
        self.assertFalse((ROOT/'src/os/board_constants.c').exists())
        self.assertNotIn('src/os/board_constants.c', (ROOT/'tools/build.py').read_text())
        self.assertNotRegex(original, r'lbl_802C(?:36E[048]|3774|37A0|381[0-3])')

    def test_particle_alpha_preserves_total_hardware_conversion(self):
        _, text = source()
        body = function(text, 'StarObjEffHook')
        expression = re.search(r'dataP->color.a = ([^;]+);', body).group(1)
        self.compile_run('static u8 alpha(float weight) { return '+expression+'; }',
                         'board_star_alpha_selftest.c')


if __name__ == '__main__':
    unittest.main()
