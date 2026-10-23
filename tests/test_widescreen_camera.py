"""Execute the actual camera setters and shared mapping, not a Python model."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build, apply_patches, audit_widescreen

ROOT = Path(__file__).resolve().parents[1]


class WidescreenCamera(unittest.TestCase):
    def test_native_setters_lifetime_masks_animation_and_resize(self):
        rel = 'src/game/hsfman.c'
        dep = Path(apply_patches.require_pinned_decomp())
        source = apply_patches.apply_unified_diff((dep/rel).read_text(encoding='utf-8'),
            (ROOT/'compat/decomp'/ (rel+'.patch')).read_text(encoding='utf-8'))
        definitions = re.search(r'^HU3D_CAMERA defCamera = \{.*?^\};', source, re.M | re.S).group()
        for name in ('Create', 'PerspectiveSet', 'ViewportSet', 'ScissorSet', 'Kill', 'AllKill'):
            definitions += '\n' + re.search(r'^void Hu3DCamera'+name+r'\([^;]+?\)\n\{.*?^\}', source, re.M | re.S).group()
        with tempfile.TemporaryDirectory(prefix='wide-camera-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'camera_subject.inc').write_text(definitions)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG', '-I'+str(directory),
                str(ROOT/'tests/native/widescreen_camera_selftest.c'),
                str(ROOT/'src/hsf/mp6_widescreen_camera.c'), '-o',str(exe)], capture_output=True,text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_callers_do_not_double_widen_camera_setters(self):
        for row in audit_widescreen.inventory():
            if row['owner'] == 'game': continue
            for call in row['calls']:
                if call['api'] in ('Hu3DCameraPerspectiveSet', 'Hu3DCameraViewportSet', 'Hu3DCameraScissorSet'):
                    self.assertNotIn('mp6_widescreen_', call['args'], row['source'])
        source = (ROOT/'src/hsf/mp6_widescreen_extrude.c').read_text()
        self.assertNotIn('g_mp6WsCameras', source)
        self.assertIn('mp6_widescreen_camera_reapply();', source)


if __name__ == '__main__':
    unittest.main()
