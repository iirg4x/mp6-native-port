"""The GameCube Microphone: shared engine, backends, setting and talk button."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


def patched(rel):
    original = (Path(build.DECOMP) / rel).read_text(encoding='utf-8', errors='surrogateescape')
    patch = (ROOT / 'compat/decomp' / (rel + '.patch')).read_text(encoding='utf-8', errors='surrogateescape')
    return apply_patches.apply_unified_diff(original, patch, rel)


class Microphone(unittest.TestCase):
    def test_engine_matches_spoken_words(self):
        with tempfile.TemporaryDirectory(prefix='mic-', dir=ROOT / 'build') as temp:
            exe = Path(temp) / 'test.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O1', '-UNDEBUG',
                                     '-I', 'src/host', 'tests/native/mic_engine_selftest.c',
                                     'src/mic/mic_engine.c', 'src/mic/mic_host.c', '-o', str(exe)],
                                    cwd=ROOT, capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_linked_everywhere_and_carved_out(self):
        for rel in ('src/mic/mic_engine.c', 'src/mic/mic_host.c'):
            self.assertIn(rel, build.PLATFORM_SOURCES_COMMON)
        for rel in ('src/mic/mic_pcm_sdl.c', 'src/mic/speech_sapi.c', 'src/mic/speech_android.c',
                    'src/mic/speech_web.c'):
            self.assertIn(rel, build.PLATFORM_AURORA_ONLY)
            self.assertIn(rel, build.HOST_STATE_SECTION_SOURCES)
        self.assertIn('src/mic/mic_host.c', build.HOST_STATE_SECTION_SOURCES)
        # The engine holds game state (context tables) and is captured.
        self.assertNotIn('src/mic/mic_engine.c', build.HOST_STATE_SECTION_SOURCES)
        shims = (ROOT / 'src/null/shims_manual.c').read_text(encoding='utf-8')
        self.assertNotIn('s32 MICProbeEx(', shims)
        self.assertNotIn('int gsapi_EngineGetParam(', shims)

    def test_game_mic_runs_its_speech_thread_inline(self):
        mic = patched('src/game/mic.c')
        self.assertNotIn('OSSendMessage(&MCMessageQueue', mic)
        self.assertIn('static void MCThreadCommand(s32 command)', mic)
        periodic = mic.split('void HuMCPeriodicProc(void)', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('MCThreadStep();', periodic)
        self.assertNotIn('OSSetAlarm', periodic)
        # Result nodes are host pointers, never narrowed through u32.
        self.assertIn('uintptr_t resultNode', mic)

    def test_backends_only_run_from_the_main_loop(self):
        bridge = (ROOT / 'src/gx/aurora_bridge.c').read_text(encoding='utf-8')
        pump = bridge.index('    mp6_mic_host_pump();')
        self.assertLess(bridge.index('    mp6_pump_keyboard_to_pad();\n    /* Mic backends'), pump)
        self.assertLess(pump, bridge.index('g_postRetraceCB((u32)mp6_tick_count);', pump))
        self.assertIn('SDL_SCANCODE_M', bridge)
        engine = (ROOT / 'src/mic/mic_engine.c').read_text(encoding='utf-8')
        self.assertNotIn('mp6_mic_backend_start(', engine)
        self.assertNotIn('mp6_mic_host_pump(', engine)
        android = (ROOT / 'packaging/android/app/src/main/AndroidManifest.xml').read_text(encoding='utf-8')
        self.assertIn('android.permission.RECORD_AUDIO', android)
        self.assertIn('android.speech.RecognitionService', android)

    def test_setting_is_persisted_and_published(self):
        launcher = (ROOT / 'src/gx/ui/launcher_core.cpp').read_text(encoding='utf-8')
        self.assertIn('"game.microphone"', launcher)
        self.assertIn('mp6_mic_set_enabled(g_cfg.microphone);', launcher)
        self.assertIn('g_cfg.microphone = 1;', launcher)
        self.assertIn('.key = "Use Microphone"', (ROOT / 'src/gx/ui/settings.cpp').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
