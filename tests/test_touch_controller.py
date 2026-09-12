"""Actual Android controller adapter/model/ImGui skin, driven on the host."""
from pathlib import Path
import subprocess
import re
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class TouchController(unittest.TestCase):
    def test_launcher_config_roundtrip(self):
        source = (ROOT / 'src/gx/ui/launcher_core.cpp').read_text(encoding='utf-8')
        def function(name):
            return re.search(r'^static [^\n]+ ' + name + r'\([^;]+?\)\n\{.*?^\}', source, re.M | re.S).group()
        subject = '\n'.join(function(name) for name in ['mp6_launcher_enh_values_of',
            'mp6_launcher_enh_values', 'mp6_launcher_enh_store', 'mp6_launcher_defaults'])
        subject += source[source.index('enum Mp6ConfigKeyBit'):source.index('static void mp6_launcher_config_save')]
        subject += function('mp6_launcher_config_save')
        with tempfile.TemporaryDirectory(prefix='touch-config-', dir=ROOT / 'build') as temp:
            directory = Path(temp)
            (directory / 'touch_config_subject.inc').write_text(subject, encoding='utf-8')
            exe = directory / 'config-test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I', str(ROOT / 'include'), '-I', str(directory),
                str(ROOT / 'tests/native/touch_config_selftest.cpp'),
                '-x', 'c++', str(ROOT / 'src/enh/mp6_enhancements.c'), '-o', str(exe)],
                cwd=ROOT, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe), str(directory / 'config.json')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)

    def test_native_controller(self):
        deps = ROOT / 'build/android-aurora/_deps'
        imgui = deps / 'imgui-src'
        directory = ROOT / 'build/touch-controller-qa'
        directory.mkdir(exist_ok=True)
        exe = directory / 'touch-pad-selftest.exe'
        cmd = [build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG', '-DSDL_STATIC_LIB', '-DTARGET_PC',
               '-I', str(ROOT / 'include'), '-I', str(ROOT / 'build/android-aurora-source/include'),
               '-I', str(deps / 'sdl-src/include'), '-I', str(imgui),
               str(ROOT / 'tests/native/touch_pad_selftest.cpp'),
               *[str(imgui / name) for name in ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp']],
               '-o', str(exe)]
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(exe), str(directory)], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS', result.stdout)
        # Generated visual fixtures only, no user image is edited.
        from PIL import Image
        for ppm in directory.glob('touch-*.ppm'):
            with Image.open(ppm) as im:
                im.save(ppm.with_suffix('.png'))

    def test_full_pad_merge_and_android_only_settings(self):
        source = (ROOT / 'src/gx/aurora_bridge.c').read_text()
        for field in ['substickX = touch.cstickX', 'substickY = touch.cstickY',
                      'triggerLeft = touch.triggerL', 'triggerRight = touch.triggerR']:
            self.assertIn(field, source)
        source = (ROOT / 'src/gx/ui/input.cpp').read_text()
        self.assertIn('mp6_touch_pad_control_at(event.tfinger.x, event.tfinger.y)', source)
        self.assertIn('|| mp6_touch_pad_editing()', source)


if __name__ == '__main__':
    unittest.main()
