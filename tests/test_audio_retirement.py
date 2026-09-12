"""Real mixer/collector tests with deterministic PCM and allocation ownership."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]


class AudioRetirement(unittest.TestCase):
    def test_mixer_samples_and_game_thread_retirement(self):
        source = (ROOT/'src/audio/msm_bridge.c').read_text()
        with tempfile.TemporaryDirectory(prefix='audio-retirement-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            typedefs = '\n'.join(re.search(r'typedef struct \{[^{}]*\} '+name+r';', source).group(0)
                                 for name in ('MsmPcmRetirement', 'MsmKgRelRec'))
            (folder/'audio_subject.inc').write_text(typedefs+'\n'+'\n'.join(function(source, n)
                for n in ('mp6_pcm_detach', 'mp6_pcm_release', 'mp6_msm_collect_finished', 'mp6_msm_render',
                          'mp6_msm_apply_voice_limit', 'msmStreamStop', 'msmStreamStopAll',
                          'mp6_se_keygroup_release', 'msmSeStop', 'msmSeStopAll',
                          'mp6_msm_savestate_restore_voice')))
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-std=c11', '-O2', '-UNDEBUG', '-I'+str(folder),
                str(ROOT/'tests/native/audio_retirement_selftest.c'), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_callback_has_no_normal_logging_or_pcm_free(self):
        mixer = (ROOT/'src/audio/msm_bridge.c').read_text()
        render = function(mixer, 'mp6_msm_render')
        self.assertNotIn('free(', render)
        self.assertEqual(render.count('g_pcmRetired = 1;'), 4)
        output = (ROOT/'src/audio/audio_out_sdl.c').read_text()
        callback = function(output, 'mp6_audio_callback')
        self.assertNotIn('printf(', callback)
        self.assertNotIn('free(', callback)
        self.assertIn('mp6_msm_collect_finished()', function(mixer, 'msmSysRegularProc'))
        self.assertIn('mp6_msm_collect_finished()', (ROOT/'src/gx/aurora_bridge.c').read_text())
        self.assertIn('mp6_msm_collect_finished()', output)
        for name in ('mp6_msm_apply_voice_limit', 'msmStreamStop', 'msmStreamStopAll',
                     'mp6_se_keygroup_release', 'msmSeStop', 'msmSeStopAll',
                     'mp6_msm_savestate_restore_voice'):
            self.assertNotIn('free(', function(mixer, name))
        for name in ('msmStreamPlay', 'msmSePlay'):
            body = function(mixer, name)
            self.assertIn('mp6_pcm_detach(&retired', body)
            self.assertIn('mp6_pcm_release(&retired)', body)


if __name__ == '__main__':
    unittest.main()
