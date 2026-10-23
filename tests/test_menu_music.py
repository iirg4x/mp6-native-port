"""Linked-disc menu music: real decoder, bounded parser, shuffle and game gain."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_frame_resource_lifetimes import function
from tests.source_tokens import compact_source

ROOT = Path(__file__).resolve().parents[1]


class MenuMusic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='menu-music-', dir=ROOT / 'build')
        cls.folder = Path(cls.temp.name)
        obj = cls.folder / 'codec.o'
        cls.exe = cls.folder / 'pdt-test.exe'
        subprocess.run([build.ZIG, 'cc', '-O2', '-c', 'src/audio/dspadpcm.c', '-o', str(obj)],
                       cwd=ROOT, check=True, capture_output=True, timeout=90)
        subprocess.run([build.ZIG, 'c++', '-std=c++17', '-O2', '-UNDEBUG',
                        'tests/native/menu_music_selftest.cpp', str(obj), '-o', str(cls.exe)],
                       cwd=ROOT, check=True, capture_output=True, timeout=90)
        cls.sfx = cls.folder / 'sfx-test.exe'
        subprocess.run([build.ZIG, 'c++', '-std=c++17', '-O2', '-UNDEBUG',
                        'tests/native/menu_sfx_selftest.cpp', str(obj), '-o', str(cls.sfx)],
                       cwd=ROOT, check=True, capture_output=True, timeout=90)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_bank_validation_decoder_and_shuffle(self):
        result = subprocess.run([str(self.exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ui_cues_decode_safely(self):
        bank = ROOT / 'build/disc-cache/orig/GP6E01/files/sound/MP6_SND.msm'
        result = subprocess.run([str(self.sfx), *([str(bank)] if bank.is_file() else [])],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

    def test_all_available_retail_music_decodes(self):
        bank = ROOT / 'build/disc-cache/orig/GP6E01/files/sound/MP6_Str.pdt'
        if not bank.is_file():
            self.skipTest('User-linked disc bank is not available; synthetic tests still run')
        result = subprocess.run([str(self.exe), str(bank)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('97 audible tracks', result.stdout)
        self.assertIn('6 idle tracks only', result.stdout)
        print(result.stdout.strip())

    def test_menu_uses_idle_catalog_not_all_looping_music(self):
        source = compact_source((ROOT / 'src/audio/menu_music.cpp').read_text())
        self.assertIn('tracks=menu_music_catalog(reader(),uint32_t(size))', source)
        self.assertNotIn('tracks=music_catalog(', source)

    def test_game_gain_restores_latest_volume_without_touching_mixer(self):
        source = (ROOT / 'src/audio/audio_out_sdl.c').read_text()
        methods = '\n'.join(function(source, name) for name in
                            ('mp6_audio_set_master_gain', 'mp6_audio_set_menu_override'))
        test = self.folder / 'gain-test.c'
        test.write_text('''#include <assert.h>
#include <stddef.h>
static int stream, calls; static int *g_stream;
static float g_mp6MasterGain=1.f, observed=1.f; static int g_menuMusicOverride;
static void SDL_SetAudioStreamGain(int *s,float v) { assert(s==&stream); observed=v; ++calls; }
''' + methods + '''
int main(void) {
 mp6_audio_set_menu_override(0); assert(calls==0);
 mp6_audio_set_master_gain(.6f); assert(calls==0 && g_mp6MasterGain==.6f);
 g_stream=&stream; mp6_audio_set_menu_override(1); assert(observed==0.f);
 mp6_audio_set_master_gain(.35f); assert(observed==0.f && g_mp6MasterGain==.35f);
 mp6_audio_set_menu_override(0); assert(observed==.35f);
 const int before=calls; mp6_audio_set_menu_override(0); assert(calls==before);
 mp6_audio_set_master_gain(2.f); assert(observed==1.f);
 mp6_audio_set_menu_override(1); mp6_audio_set_master_gain(-1.f); assert(observed==0.f);
 mp6_audio_set_menu_override(0); assert(observed==0.f);
 g_stream=NULL; mp6_audio_set_menu_override(1); assert(g_menuMusicOverride);
 mp6_audio_set_menu_override(0); assert(!g_menuMusicOverride);
}
''')
        exe = self.folder / 'gain-test.exe'
        subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', str(test), '-o', str(exe)],
                       check=True, capture_output=True, timeout=90)
        subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)
        self.assertNotIn('msmStream', methods)
        self.assertNotIn('mp6_msm_', methods)


if __name__ == '__main__':
    unittest.main()
