"""Real console command tests and Android display-policy preprocessing."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]

class AndroidDisplayConsole(unittest.TestCase):
    def test_nonprinting_keys_do_not_swallow_next_ime_commit(self):
        source = (ROOT/'src/gx/ui/console.cpp').read_text()
        key_handler = source.split('listen(mDocument, Rml::EventId::Keydown,')[1].split('/*capture=*/true);')[0]
        self.assertIn('mSwallowNextText = (key == Rml::Input::KI_OEM_3', key_handler)
        self.assertNotIn('mSwallowNextText = true', key_handler)
        self.assertNotIn('Rml::Input::KI_F9', key_handler)
        self.assertNotIn('Rml::Input::KI_TAB', key_handler)
        self.assertIn('event.StopPropagation()', key_handler)

    def test_clear_removes_output_and_overlays(self):
        with tempfile.TemporaryDirectory(prefix='console-clear-', dir=ROOT/'build') as tmp:
            exe = Path(tmp)/'test.exe'
            command = [build.ZIG, 'cc', '-O2', '-UNDEBUG', '-I'+str(ROOT/'include'),
                       str(ROOT/'src/gx/console/console_core.c'),
                       str(ROOT/'tests/native/console_clear_selftest.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_android_never_applies_desktop_window_preferences(self):
        source = (ROOT/'src/gx/ui/launcher_core.cpp').read_text()
        source = source[source.index('static void mp6_launcher_apply_display(void)'):source.index('static void mp6_launcher_apply_volume(void)')]
        for platform,defines in [('android',['-D__ANDROID__']),('windows',[])]:
            result = subprocess.run([build.ZIG,'c++','-E','-P','-x','c++',*defines,'-'],
                input=source,capture_output=True,text=True,check=True)
            self.assertIn('SDL_SetWindowFullscreen(g_window, true)', result.stdout)
            for desktop in ['SDL_SetWindowFullscreen(g_window, false)', 'SDL_SetWindowSize', 'SDL_SetWindowAspectRatio', 'g_cfg.windowMode']:
                if platform=='android': self.assertNotIn(desktop,result.stdout)
                else: self.assertIn(desktop,result.stdout)

    def test_modern_immersive_preserves_console_keyboard(self):
        source = (ROOT/'packaging/android/app/src/main/java/com/mp6/game/Mp6Activity.java').read_text()
        self.assertIn('Build.VERSION.SDK_INT >= 30', source)
        self.assertIn('WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE', source)
        self.assertIn('controller.hide(WindowInsets.Type.systemBars())', source)
        self.assertNotIn('WindowInsets.Type.ime()', source)
        self.assertIn('post(this::hideSystemBars)', source)

if __name__ == '__main__':
    unittest.main()
