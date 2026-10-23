"""The Quick Party mod: per-tick board speed, text and intro shortcuts."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import apply_patches, build

ROOT=Path(__file__).resolve().parents[1]


def patched(rel):
    original=(Path(build.DECOMP)/rel).read_text(encoding='utf-8',errors='surrogateescape')
    patch=(ROOT/'compat/decomp'/(rel+'.patch')).read_text(encoding='utf-8',errors='surrogateescape')
    return apply_unified_diff(original,patch,rel)


apply_unified_diff=apply_patches.apply_unified_diff


class QuickPlay(unittest.TestCase):
    def test_speed_policy(self):
        with tempfile.TemporaryDirectory(prefix='quick-play-',dir=ROOT/'build') as temp:
            exe=Path(temp)/'test.exe'
            result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-O2','-UNDEBUG',
                '-Dgetenv=mp6_test_getenv','tests/native/quick_play_selftest.c',
                'src/os/quick_play.c','-o',str(exe)],
                cwd=ROOT,capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_store_is_host_owned_and_linked_everywhere(self):
        self.assertIn('src/os/quick_play.c',build.HOST_STATE_SECTION_SOURCES)
        self.assertIn('src/os/quick_play.c',build.PLATFORM_SOURCES_COMMON)
        store=(ROOT/'src/os/quick_play.c').read_text()
        self.assertLess(store.index('#include "mp6_host_section.h"'),store.index('static int g_setting'))

    def test_setting_is_persisted_and_settled_every_tick(self):
        launcher=(ROOT/'src/gx/ui/launcher_core.cpp').read_text()
        self.assertIn('"game.quick_play"',launcher)
        self.assertIn('mp6_quick_play_set_speed(g_cfg.quickPlay);',launcher)
        self.assertIn('.key = "Quick Party"',(ROOT/'src/gx/ui/settings.cpp').read_text())
        bridge=(ROOT/'src/gx/aurora_bridge.c').read_text()
        wait=bridge.split('static void mp6_tick_throttle_wait(void)',1)[1]
        # Settled every tick, before the free-run return, so the web (which
        # free-runs) gets paced extra ticks too.
        self.assertIn('if (!mp6_tick_quick_play_apply()) {',wait)
        self.assertIn('mp6_quick_play_set_achieved(',bridge)
        self.assertEqual(bridge.count('mp6_tick_quick_play_hide()) aurora_hide_next_frame();'),2)
        sleep=(ROOT/'src/os/process_native.c').read_text()
        self.assertIn('process->sleep = mp6_quick_play_scale_sleep(time);',sleep)

    def test_board_logic_makes_up_the_shortfall(self):
        # Animation playback is never scaled in-game (board scripts poll frames).
        self.assertNotIn('mp6_quick_play',patched('src/game/hsfmotion.c'))
        self.assertEqual(patched('src/board/camera.c').count('maxTime = (s16)mp6_quick_play_scale_ticks(maxTime);'),2)
        self.assertIn('maxTime = (s16)mp6_quick_play_scale_ticks(maxTime);',patched('src/board/player.c'))

    def test_every_human_wait_holds_normal_speed(self):
        sites={'src/board/dice.c':'dice block','src/board/branch.c':'path choice',
               'src/board/capselect.c':'orb menu','src/board/capsule.c':'orb target',
               'src/board/capspecial.c':'coin amount','src/board/mgcall.c':'minigame pick',
               'src/board/roulette.c':'roulette','src/board/scroll.c':'map view',
               'src/board/shopevent.c':'shop','src/game/window.c':'choice window'}
        for rel,what in sites.items():
            with self.subTest(rel):
                self.assertIn(f'mp6_quick_play_hold("{what}")',patched(rel))

    def test_text_and_intro_shortcuts(self):
        window=patched('src/game/window.c')
        self.assertIn('mp6_quick_play_keywait(winId, winP);',window)
        self.assertRegex(window,r'mesTime >= \(mp6_quick_play_instant_text\(\) \? 0 : winP->mesSpeed\)')
        boot=patched('src/REL/bootDll/boot.c')
        skip=boot.index('if (mp6_quick_play_skip_intro() && getenv("MP6_BOOT_TO") == NULL)')
        self.assertLess(skip,boot.index('omOvlCallEx(DLL_fileseldll, TRUE, 0, 0);'))
        self.assertIn('if (omovlevtno == 0 && !mp6_quick_play_skip_intro())',boot)

    def test_text_shortcut_uses_the_computers_queued_press(self):
        # A computer's pause has its A press scripted in the shared key queue.
        # Dismissing the pause early must use that press up, or it answers the
        # next window: a Duel space's opponent pick then took the first entry
        # and the leftover cursor keys hit the wager menu's Back, over and over.
        window=patched('src/game/window.c')
        body=window[window.index('static void mp6_quick_play_keywait('):]
        body=body[:body.index('\n}\n')]
        self.assertIn('(HuWinActivePadGet(winP) & winP->pushKey)',body)
        self.assertLess(body.index('mp6_quick_play_take_com_press()'),body.index('winKey[i] |= key;'))
        take=window[window.index('static BOOL mp6_quick_play_take_com_press('):]
        take=take[:take.index('\n}\n')]
        self.assertIn('comKeyIdxNow = (comKeyIdxNow + 1) & 255;',take)
        self.assertIn('!= HUWIN_COMKEY_NONE',take)


    def test_menus_skip_host_prompts_but_keep_settings(self):
        mdsel=patched('src/REL/mdseldll/mdsel.c')
        self.assertLess(mdsel.index('if (!mp6_quick_play_skip_intro()) {'),
                        mdsel.index('fn_1_23E0(1, MESSNUM(MESS_ALL_MAIN_MENU, 0), 1);'))
        party=patched('src/REL/mdpartydll/mdparty.c')
        for message in ('MDPARTY_MSG_SETUP_FIRST','MDPARTY_MSG_NEXT_PARTY_PROMPT'):
            call=party.index(f'fn_1_4020(1, {message}' if message.endswith('FIRST') else f'fn_1_4020(3, {message}')
            self.assertIn('if (!mp6_quick_play_skip_intro()) {',party[call-200:call])
        self.assertIn('result = !back ? 1 : (fn_1_2EF24() ? 1 : -1);',party)
        # The saved-party resume choice and every setting screen are untouched.
        self.assertIn('fn_1_4020(2, MDPARTY_MSG_SAVED_GAME_CONTINUE_OPTION, 1);',party)


if __name__=='__main__':unittest.main()
