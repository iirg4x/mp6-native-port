"""Exercise upstream's portable board math and its actual native callers."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build
import test_board_native_abi as native_abi
from test_board_native_abi import patched

ROOT = Path(__file__).resolve().parents[1]


def portable_function(source, name):
    definitions = re.findall(
        r'^(?:static inline |static )?(?:float|void) ' + name +
        r'\([^;]+?\)\n\{.*?^\}', source, re.M | re.S)
    portable = [body for body in definitions if 'register ' not in body and 'asm {' not in body]
    if len(portable) != 1:
        raise AssertionError(f'Expected one portable definition for {name}, got {len(portable)}')
    return portable[0]


class BoardMath(unittest.TestCase):
    def test_opening_return_types_match_native_owners(self):
        declarations = []
        for name, owner in (('mbGuideModelGet', 'guide'), ('mbMasuFind_TypeListGet2', 'masu')):
            actual = re.search(r'^extern [^;]+?\b'+name+r'\([^;]+;',
                               patched('src/board/opening.c'), re.M).group()
            expected = re.search(r'^int '+name+r'\([^;]+?\)\n\{',
                                 patched('src/board/'+owner+'.c'), re.M).group()[:-1]+';'
            declarations.extend((actual, expected))
        prelude = 'typedef short s16; typedef int BOOL; typedef struct OMOBJ OMOBJ;\n'
        with tempfile.TemporaryDirectory(prefix='opening-api-', dir=ROOT/'build') as temporary:
            path = Path(temporary)/'opening-api.c'
            path.write_text(prelude+'\n'.join(declarations))
            for target in ('x86_64-windows-gnu', 'aarch64-freestanding'):
                result = subprocess.run([build.ZIG, 'cc', '-target', target, '-Werror',
                                         '-c', str(path), '-o', str(path.with_suffix('.o'))],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_opening_curve_callback_preserves_native_pointer_and_float_abi(self):
        source = patched('src/board/opening.c')
        self.assertNotIn('(OPENINGCURVEEVALFUNC)(u32)', source)
        self.assertNotIn('(*OPENINGCURVEEVALFUNC)()', source)
        self.assertIn('OpeningCurveLength(OpeningCurveEval,', source)
        self.assertIn('OpeningCurveNewton(OpeningCurveEval,', source)
        subject = native_abi.function(patched('src/board/math.c'), 'mbHermiteCalcSlope')
        subject += '\n' + re.search(r'typedef float \(\*OPENINGCURVEEVALFUNC\)[^;]+;', source).group()
        subject += '\n' + '\n'.join(native_abi.function(source, name) for name in
            ('OpeningCurveEval', 'OpeningCurveIntegrate', 'OpeningCurveNewton', 'OpeningCurveLength'))
        native_abi.BoardNativeABI.compile_run(self, subject, 'opening_curve_selftest.c')

    def test_portable_math_rotation_projection_and_bounds(self):
        source = patched('src/board/math.c')
        constants = re.findall(r'^#define MB_TRIG_[^\n]+$', source, re.M)
        # Offset expression macros are not used by the wrappers only: the four
        # direct trig accessors must exercise their actual multiline macros too.
        constants = [line for line in constants if not line.endswith('\\')]
        constants += re.findall(r'^#define MB_TRIG_[^\n]+\\\n[^\n]+', source, re.M)
        names = ['mbMathInit', 'mbMathClose', 'mbCosDeg', 'mbCosRad', 'mbSinDeg', 'mbSinRad']
        names += ['mbMtxRotTrig'+axis for axis in 'XYZ']
        names += ['mbMtxRotTrigScale'+axis for axis in 'XYZ']
        names += ['mbMtxRotAxisDeg', 'mbMtxRotAxisRad']
        names += ['mbMtxRot'+axis+unit for axis in 'XYZ' for unit in ('Deg', 'Rad')]
        names += ['mbMtxScaleRot'+axis+'Deg' for axis in 'XYZ']
        names += ['mbMtxRot', 'mbMtxTransCat', 'mbNormPosto2D',
                  'ObjectBBoxUpdate', 'MathMtxTranslationSet']
        subject = '\n'.join(constants)+'\nstatic float *cosTab;\n'
        subject += '\n\n'.join(portable_function(source, name) for name in names)
        native_abi.BoardNativeABI.compile_run(self, subject, 'board_math_selftest.c')
        broken = subject.replace('-HU_DISP_CENTERY * (src->y - 1.0f)',
                                 'HU_DISP_HEIGHT * (src->y - 1.0f)')
        self.assertNotEqual(subject, broken)
        native_abi.BoardNativeABI.compile_run(self, broken, 'board_math_selftest.c', expect_success=False)
        broken = subject.replace('mtx[2][3] = (float)tz;', 'mtx[2][3] = (float)ty;')
        self.assertNotEqual(subject, broken)
        native_abi.BoardNativeABI.compile_run(self, broken, 'board_math_selftest.c', expect_success=False)

    def test_axis_declarations_match_math_on_both_native_targets(self):
        owner = patched('src/board/math.c')
        declarations = []
        callers = {
            'mbMtxScaleRotXDeg': [patched('src/board/coin.c')],
            'mbMtxRotAxisDeg': [
                (Path(build.DECOMP)/'include/REL/w01Dll_world01.h').read_text(),
                (Path(build.DECOMP)/'include/game/board/guide.h').read_text(),
                patched('src/board/effect.c')],
            'mbMtxRotAxisRad': [patched('src/REL/w01Dll/world01.c')],
        }
        for name, sources in callers.items():
            definition = portable_function(owner, name).split('\n{', 1)[0]+';'
            declarations.append(definition)
            for source in sources:
                declarations.append(re.search(r'^(?:extern )?void '+name+r'\([^;]+;',
                                              source, re.M).group())
        prelude = ('typedef unsigned char u8;\ntypedef float Mtx[3][4];\n'
                   'typedef struct { float x, y, z; } HuVecF;\n')
        with tempfile.TemporaryDirectory(prefix='board-math-api-', dir=ROOT/'build') as temporary:
            path = Path(temporary)/'math-api.c'
            path.write_text(prelude+'\n'.join(declarations), encoding='utf-8')
            for target in ('x86_64-windows-gnu', 'aarch64-freestanding'):
                with self.subTest(target=target):
                    result = subprocess.run([build.ZIG, 'cc', '-target', target, '-Werror',
                                             '-c', str(path), '-o', str(path.with_suffix('.o'))],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
