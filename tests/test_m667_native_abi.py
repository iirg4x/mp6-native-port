"""Run Talkie Walkie's actual native data, pool, camera and draw boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M667NativeAbi(unittest.TestCase):
    def test_native_owners_descriptors_and_camera(self):
        apply_patches.require_pinned_decomp()
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            sources = {unit: (patched / f'src/REL/m667dll/{unit}.c').read_text(encoding='utf-8')
                       for unit in ('map_players_effects', 'particles_tracks_materials', 'model_sound_description')}
            camera = (patched / 'src/game/hsfman.c').read_text(encoding='utf-8')
            subject = re.search(r'^HU3D_CAMERA defCamera = \{.*?^\};', camera, re.M | re.S).group()
            for name in ('Create', 'PerspectiveSet', 'ViewportSet', 'ScissorSet', 'Kill', 'AllKill'):
                subject += '\n' + function_definition_body(camera, 'Hu3DCamera' + name)
            subject += '\n' + sources['model_sound_description']
            for pattern in (r'typedef struct M667RegistryTemplate80 \{[^}]+\} M667RegistryTemplate80;',
                            r'const M667RegistryTemplate80 lbl_1_rodata_128 = .*?;'):
                subject += '\n' + re.search(pattern, sources['map_players_effects'], re.S).group()
            for label in ('230', '3C4', '494'):
                subject += '\n' + re.search(r'^\w+ lbl_1_data_' + label + r'\[[^;]*?^\};',
                    sources['map_players_effects'], re.M | re.S).group()
            functions = {
                'map_players_effects': ('fn_1_A0', 'fn_1_140', 'fn_1_768', 'fn_1_2B68',
                    'fn_1_1DB8', 'fn_1_38E4', 'fn_1_5AF8', 'fn_1_1338',
                    'fn_1_1934', 'fn_1_344', 'fn_1_5898',
                    'fn_1_6C5C', 'fn_1_6C60', 'fn_1_6C6C'),
                'particles_tracks_materials': ('fn_1_7350', 'fn_1_7B10', 'fn_1_7BA8',
                    'fn_1_7C8C', 'fn_1_7CC8', 'fn_1_7EF0', 'fn_1_7F48', 'fn_1_8E8C'),
            }
            for owner, names in functions.items():
                for name in names:
                    subject += '\n' + function_definition_body(sources[owner], name)
            (directory / 'm667_subject.inc').write_text(subject, encoding='utf-8')
            executable = directory / 'm667.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-I', str(directory), '-O1', '-UNDEBUG',
                '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all',
                str(ROOT / 'tests/native/m667_native_selftest.c'),
                str(ROOT / 'src/hsf/mp6_widescreen_camera.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
