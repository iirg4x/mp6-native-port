"""Execute Hyper Sniper's real allocation, collision, targeting and camera owners."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M646NativeAbi(unittest.TestCase):
    def test_native_owners_and_shared_collision(self):
        apply_patches.require_pinned_decomp()
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            owners = ('actors', 'character_motion', 'collision', 'model_attachments',
                      'model_shadows', 'score_display', 'sequence', 'target_selection')
            sources = {u: (patched / f'src/REL/m646Dll/{u}.c').read_text(encoding='utf-8') for u in owners}
            declarations = ''
            for owner, types in (
                ('actors', ('M646ActorState',)),
                ('model_shadows', ('M646StageWork',)),
                ('score_display', ('M646ScoreEntry', 'M646ScoreboardWork')),
                ('target_selection', ('M646TargetCandidate', 'M646AIProfile', 'M646CoordinatorWork')),
            ):
                for name in types:
                    declarations += re.search(r'typedef struct ' + name + r'_s \{.*?^\} ' + name + ';',
                                              sources[owner], re.M | re.S).group() + '\n'
            for source in sources.values():
                declarations += '\n'.join(re.findall(r'^#define M646_\w+ .+$', source, re.M)) + '\n'
            (directory / 'm646_types.inc').write_text(declarations, encoding='utf-8')
            camera = (patched / 'src/game/hsfman.c').read_text(encoding='utf-8')
            subject = re.search(r'^HU3D_CAMERA defCamera = \{.*?^\};', camera, re.M | re.S).group()
            for name in ('Create', 'PerspectiveSet', 'ViewportSet', 'ScissorSet', 'Kill', 'AllKill'):
                subject += '\n' + function_definition_body(camera, 'Hu3DCamera' + name)
            functions = {
                'actors': ('fn_1_AFC', 'fn_1_AF4', 'fn_1_AF8', 'fn_1_1F80'),
                'character_motion': ('fn_1_543C',),
                'collision': ('fn_1_2B50', 'fn_1_3E80', 'fn_1_3EF8', 'fn_1_3F70', 'fn_1_3FD4',
                              'fn_1_3350', 'fn_1_3894', 'fn_1_27C4', 'fn_1_27E8', 'fn_1_2968'),
                'model_shadows': ('fn_1_4E28',),
                'model_attachments': ('fn_1_8158', 'fn_1_8DAC'),
                'score_display': ('fn_1_8E78', 'fn_1_8F04', 'fn_1_8F5C', 'fn_1_8FA4'),
                'sequence': ('fn_1_E0',),
                'target_selection': ('fn_1_98E0', 'fn_1_A124', 'fn_1_A158', 'fn_1_A8E0',
                                     'fn_1_ABB0', 'fn_1_B07C', 'fn_1_B2BC'),
            }
            for owner, names in functions.items():
                for name in names:
                    subject += '\n' + function_definition_body(sources[owner], name)
            (directory / 'm646_subject.inc').write_text(subject, encoding='utf-8')
            executable = directory / 'm646.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'), *build.COMMON_FLAGS,
                '-I', str(directory), '-O1', '-UNDEBUG', '-fsanitize=undefined,float-cast-overflow',
                '-fno-sanitize-recover=all', str(ROOT / 'tests/native/m646_native_selftest.c'),
                str(ROOT / 'src/hsf/mp6_widescreen_camera.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
