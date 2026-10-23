"""Execute Stamp By Me's real native owners, callbacks, and camera transition."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M649NativeAbi(unittest.TestCase):
    def test_native_owners_and_shared_four_camera_mapping(self):
        apply_patches.require_pinned_decomp()
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            sources = {unit: (patched / f'src/REL/m649Dll/{unit}.c').read_text(encoding='utf-8')
                       for unit in ('main', 'camera', 'player', 'prop', 'effect', 'score')}
            camera = (patched / 'src/game/hsfman.c').read_text(encoding='utf-8')
            subject = re.search(r'^HU3D_CAMERA defCamera = \{.*?^\};', camera, re.M | re.S).group()
            bridge = (ROOT / 'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
            matrix = (Path(build.AURORA_ROOT) / 'lib/dolphin/mtx/mtx.c').read_text(encoding='utf-8')
            subject += '\n' + function_definition_body(matrix, 'C_MTXTransApply')
            subject += '\n' + function_definition_body(bridge, 'PSMTXTransApply')
            for name in ('Create', 'PerspectiveSet', 'PerspectiveGet', 'ViewportSet', 'ScissorSet',
                         'PosSetV', 'PosGet', 'Kill', 'AllKill'):
                subject += '\n' + function_definition_body(camera, 'Hu3DCamera' + name)
            for label in ('0', '1C4'):
                subject += '\n' + re.search(r'^\w+ lbl_1_data_' + label + r'(?:\[\d+\])? = .*?;',
                                             sources['main'], re.M | re.S).group()
            functions = {
                'main': ('fn_1_BD8', 'fn_1_36C', 'fn_1_BE0', 'fn_1_C78', 'fn_1_1244',
                         'fn_1_127C', 'mp6_m649_reset'),
                'camera': ('fn_1_14A0', 'fn_1_15A8', 'fn_1_1718', 'fn_1_18BC', 'fn_1_19BC',
                           'fn_1_1B08', 'fn_1_3250', 'fn_1_3354', 'fn_1_347C', 'fn_1_32EC',
                           'fn_1_33AC', 'fn_1_2780', 'fn_1_281C', 'fn_1_29E4', 'fn_1_2A64',
                           'fn_1_1EF4', 'fn_1_2C84'),
                'player': ('fn_1_4ECC', 'fn_1_5044', 'fn_1_5094', 'fn_1_512C'),
                'prop': ('fn_1_6990',),
                'effect': ('fn_1_8F58',),
                'score': ('fn_1_A220', 'fn_1_A528', 'fn_1_A5A4', 'fn_1_A600', 'fn_1_A644'),
            }
            for owner, names in functions.items():
                for name in names:
                    subject += '\n' + function_definition_body(sources[owner], name)
            (directory / 'm649_subject.inc').write_text(subject, encoding='utf-8')
            executable = directory / 'm649.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-I', str(directory), '-O1', '-UNDEBUG',
                '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all',
                str(ROOT / 'tests/native/m649_native_selftest.c'),
                str(ROOT / 'src/hsf/mp6_widescreen_camera.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
