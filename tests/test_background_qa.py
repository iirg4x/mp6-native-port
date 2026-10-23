"""Background capture must not open a window or consume physical PAD input."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import apply_patches, build
from setup.lib import step_aurora
from tests.integration.background_qa import MARKER, isolate_background
from tests.aurora_test_support import vendored

ROOT = Path(__file__).resolve().parents[1]


class BackgroundQA(unittest.TestCase):
    def test_runner_rejects_old_binary_and_forces_muted_isolation(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as tmp:
            exe = Path(tmp)/'fixture.exe'
            exe.write_bytes(b'old executable')
            env = {'MP6_BACKGROUND_QA': '0', 'SDL_AUDIO_DRIVER': 'wasapi'}
            before = dict(env)
            with self.assertRaisesRegex(ValueError, 'lacks background QA'):
                isolate_background(env, exe)
            self.assertEqual(env, before)
            exe.write_bytes(b'fixture\0'+MARKER.encode()+b'\0')
            isolate_background(env, exe)
            self.assertEqual(env['MP6_BACKGROUND_QA'], '1')
            self.assertEqual(env['SDL_AUDIO_DRIVER'], 'dummy')
            self.assertEqual(env['SDL_AUDIODRIVER'], 'dummy')

    def test_all_active_graphical_runners_require_isolation(self):
        for name in ('run_board_qa.py','run_title_qa.py','run_mdmini_qa.py','run_widescreen_scenes.py'):
            source = (ROOT/'tests/integration'/name).read_text()
            self.assertIn('isolate_background(env, ', source)
            self.assertIn('creationflags=background_process_flags()', source)

    def test_native_virtual_input_and_rumble_guards(self):
        header = vendored('lib/background_qa.hpp')
        def prefix(file, signature, end):
            text = vendored(file)
            start = text.index(signature)
            return text[start:text.index(end, start)]
        pad = prefix('lib/dolphin/pad/pad.cpp','u32 PADRead(', '  if (!g_keyboardBindingsLoaded)')
        motor = prefix('lib/dolphin/pad/pad.cpp','void PADControlMotor(', '  const auto controller')
        hotplug = prefix('lib/input.cpp','SDL_JoystickID add_controller(', '  auto* ctrl')
        program = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
using u32=uint32_t;
using SDL_JoystickID=uint32_t;
constexpr int PAD_CHANMAX=4, PAD_ERR_NONE=0, PAD_ERR_NO_CONTROLLER=-1;
struct PADStatus { int button, stickX, triggerLeft, err; };
bool g_virtualPadActive[4]={true,false,true,false};
PADStatus g_virtualPadStatus[4]={{0x100,-60,128,0},{},{0x200,75,255,0},{}};
bool g_blockPAD=false, g_suppressHeldOnRead=true;
int physicalReads=0, motors=0, opens=0;
void merge_virtual_status(PADStatus& status,const PADStatus& value) { status=value; }
''' + header.replace('#pragma once','') + '\nusing aurora::background_qa;\n' + pad + r'''
  ++physicalReads; return 123;
}
''' + motor + '\n  ++motors;\n}\n' + hotplug + r'''
  ++opens; return which;
}
int main(int argc,char** argv) {
  bool background=argv[1][0]=='1';
  assert(background_qa()==background);
  PADStatus statuses[4]{};
  auto support=PADRead(statuses);
  PADControlMotor(0,1); PADControlMotor(0,0); add_controller(55);
  if(background) {
    assert(!physicalReads && !motors && !opens && !support);
    assert(statuses[0].button==0x100 && statuses[0].stickX==-60 && statuses[0].triggerLeft==128);
    assert(statuses[2].button==0x200 && statuses[2].stickX==75 && statuses[2].triggerLeft==255);
    assert(statuses[1].err==-1 && statuses[3].err==-1 && !g_suppressHeldOnRead);
    g_blockPAD=true;
    PADRead(statuses);
    assert(!statuses[0].button && !statuses[0].stickX && !statuses[0].triggerLeft);
    assert(statuses[0].err==0 && statuses[1].err==-1);
  } else {
    assert(physicalReads==1 && motors==2 && opens==1 && support==123);
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='background-qa-',dir=ROOT/'build') as tmp:
            cpp, exe = Path(tmp)/'test.cpp', Path(tmp)/'test.exe'
            cpp.write_text(program)
            result = subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',str(cpp),'-o',str(exe)],
                                    capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            for value in ('1','0','','true','10'):
                result = subprocess.run([str(exe),str(int(value=='1'))],
                    env=dict(os.environ,MP6_BACKGROUND_QA=value),capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
