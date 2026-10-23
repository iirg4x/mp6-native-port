"""Execute Jump the Gun's allocations and split transition on the native ABI."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M643NativeAbi(unittest.TestCase):
    def test_allocations_and_real_camera_transition(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            sources = {name: (patched / f'src/REL/m643DLL/{name}.c').read_text(encoding='utf-8')
                       for name in ('game', 'startup', 'math')}
            camera = (patched / 'src/game/hsfman.c').read_text(encoding='utf-8')
            subject = re.search(r'^HU3D_CAMERA defCamera = \{.*?^\};', camera, re.M | re.S).group()
            for name in ('Create', 'PerspectiveSet', 'ViewportSet', 'ScissorSet', 'PosSetV', 'Kill', 'AllKill'):
                subject += '\n' + function_definition_body(camera, 'Hu3DCamera' + name)
            for label in ('60', '68', 'A8', 'E8', 'F8'):
                subject += '\n' + re.search(r'^const [^\n]* lbl_1_rodata_' + label +
                    r'\[.*?;', sources['game'], re.M | re.S).group()
            subject += '\n' + re.search(r'^HuVecF lbl_1_data_AC = .*?;', sources['game'], re.M).group()
            for owner, functions in {
                'startup': ('fn_1_16C', 'fn_1_20C', 'mp6_m643_reset'),
                'math': ('fn_1_298',),
                'game': ('fn_1_1640', 'fn_1_3374', 'fn_1_3A98'),
            }.items():
                for function in functions:
                    subject += '\n' + function_definition_body(sources[owner], function)
            # Keep the visibility regression tied to the production body. The
            # fixture supplies only the camera/model side effects it needs.
            subject += '\n' + function_definition_body(sources['game'], 'fn_1_2A98')
            (directory / 'm643_subject.inc').write_text(subject, encoding='utf-8')

            # Execute the allocation expressions from their real call sites.
            # Comparing the requested bytes with each complete native type
            # catches regressions to PPC byte counts, even if a mock heap has slack.
            allocations = {
                '17A0': 'M643SequenceWork', '2D90': 'M643ViewSequenceWork',
                '23C4': 'M643MapSceneWork', '4AEC': 'M643PlayerPrefix',
                '83C8': 'M643TeamScenePrefix', '2838': 'M643PlatformWork',
                '3374': 'M643ViewControlWork', 'B130': 'M643CameraEffectWork',
                'A138': 'M643ShotPrefix',
            }
            # Fail if a new call site is not represented below. The only
            # non-call match is the allocator definition's typed parameter list.
            all_calls = re.findall(r'\bfn_1_16C\(([^;\n]+)\)', sources['game'] + sources['startup'])
            all_calls = [call for call in all_calls if 'OMOBJ_FUNC' not in call]
            self.assertEqual(len(all_calls), len(allocations))
            calls = []
            for callback, record in allocations.items():
                matches = re.findall(r'fn_1_16C\([^;\n]+, fn_1_' + callback + r'\)',
                                     sources['game'] + sources['startup'])
                self.assertEqual(len(matches), 1, callback)
                calls.append(f'CHECK_ALLOCATION({record}, {matches[0]});')
            (directory / 'm643_allocations.inc').write_text('\n'.join(calls), encoding='utf-8')
            executable = directory / 'm643.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-I', str(directory), '-O1', '-UNDEBUG',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(ROOT / 'tests/native/m643_native_selftest.c'),
                str(ROOT / 'src/hsf/mp6_widescreen_camera.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
