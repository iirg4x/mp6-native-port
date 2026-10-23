"""Run Banana Shake's actual native allocation and cross-owner contracts."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M675NativeAbi(unittest.TestCase):
    def test_native_allocations_callbacks_and_shared_records(self):
        apply_patches.require_pinned_decomp()
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            owners = ('camera', 'collision', 'effects', 'gameplay', 'main', 'player', 'score')
            sources = {u: (patched / f'src/REL/m675Dll/{u}.c').read_text(encoding='utf-8')
                       for u in owners}
            declarations = ''
            for owner, names in (
                ('camera', ('M675FogWork', 'M675AudioEntry', 'M675AudioWork')),
                ('player', ('M675PlayerConfig', 'M675PlayerWork')),
                ('score', ('ScoreDisplayEntry', 'ScoreDisplayWork')),
                ('effects', ('M675EffectCollection',)),
            ):
                for name in names:
                    declarations += re.search(r'typedef struct ' + name + r'(?:_s)? \{.*?^\} '
                                              + name + ';', sources[owner], re.M | re.S).group() + '\n'
            (directory / 'm675_types.inc').write_text(declarations, encoding='utf-8')
            functions = {
                'collision': ('fn_1_F118', 'fn_1_10170', 'fn_1_10200', 'fn_1_10290',
                              'fn_1_10310', 'fn_1_1039C', 'fn_1_F4FC', 'fn_1_FE84'),
                'effects': ('fn_1_D550', 'fn_1_E918'),
                'camera': ('fn_1_10A0', 'fn_1_11A0', 'fn_1_12EC', 'fn_1_132C',
                           'fn_1_303C', 'fn_1_2F38', 'fn_1_2FD4', 'fn_1_3164', 'fn_1_4470',
                           'fn_1_232C', 'fn_1_235C', 'fn_1_1FC8', 'fn_1_4BB4'),
                'main': ('fn_1_95C', 'fn_1_A60'),
                'player': ('fn_1_527C', 'fn_1_66EC'),
                'gameplay': ('fn_1_7684', 'fn_1_7944'),
                'score': ('fn_1_E9A8', 'fn_1_EF88', 'fn_1_F058', 'fn_1_F0B0'),
            }
            subject = '\n'.join(function_definition_body(sources[owner], name)
                                for owner, names in functions.items() for name in names)
            bridge = (ROOT / 'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
            subject += '\n' + function_definition_body(bridge, 'PSMTXMultVecArraySR')
            (directory / 'm675_subject.inc').write_text(subject, encoding='utf-8')
            executable = directory / 'm675.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-I', str(directory), '-O1', '-UNDEBUG',
                '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                '-Werror=int-conversion', '-fsanitize=undefined,float-cast-overflow',
                '-fno-sanitize-recover=all', str(ROOT / 'tests/native/m675_native_selftest.c'),
                '-o', str(executable)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
