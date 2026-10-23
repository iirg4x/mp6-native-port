"""Build an isolated instruction-screen lifecycle probe.

The probe selects m616 deterministically, records the instruction-screen
input branches, and gives the recovered game one opt-in return seam.  All
changes are applied to QA-only source copies; the checked-out decompilation,
normal executable, and player-save location remain untouched.
"""
import os
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("inst QA anchor changed: " + before)
    return text.replace(before, after, 1)


def instrument_inst_setup(source, game=616, game_no=15):
    """Select an instruction route; keep the original m616 lifecycle default."""
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(
        source,
        '    lbl_1_bss_36 = GwSystem.mgNo;',
        f'''    GwSystem.mgNo = {game_no}; /* MG_NAME_M{game}: deterministic QA route. */
    _SetFlag(FLAG_INST_MG_MODE);
    MgInstExitF = TRUE;
    mp6_event_post("qa.inst.enter", GwSystem.mgNo, "m{game}");
    lbl_1_bss_36 = GwSystem.mgNo;''')
    source = replace_once(
        source,
        '''    lbl_1_bss_6 = 2;

    flag_2 = 0;''',
        '''    lbl_1_bss_6 = 2;
    mp6_event_post("qa.inst.ready", 0, "menu");

    flag_2 = 0;''')
    source = replace_once(
        source,
        '''            if (lbl_1_bss_24 & PAD_BUTTON_START) {
                HuAudFXPlay(MSM_SE_CMN_03);
                GWMgPlayNumAdd(1);
                goto finish;
            }''',
        '''            if (lbl_1_bss_24 & PAD_BUTTON_START) {
                HuAudFXPlay(MSM_SE_CMN_03);
                GWMgPlayNumAdd(1);
                mp6_event_post("qa.inst.start", 0, "game");
                goto finish;
            }''')
    source = replace_once(
        source,
        '''        if (MgInstExitF) {
            if (lbl_1_bss_8 == 0 && (lbl_1_bss_24 & PAD_BUTTON_B)) {
                HuAudFXPlay(MSM_SE_CMN_04);
                flag_1 = 1;
                goto finish;
            }''',
        '''        if (MgInstExitF) {
            if (lbl_1_bss_8 == 0 && (lbl_1_bss_24 & PAD_BUTTON_B)) {
                HuAudFXPlay(MSM_SE_CMN_04);
                flag_1 = 1;
                mp6_event_post("qa.inst.back", 0, "return");
                goto finish;
            }''')
    return source


def instrument_m616_prolog(source):
    """Return from one QA-only m616 instance after proving it was entered."""
    source = '#include "mp6_events.h"\n#include "game/object.h"\n#include "game/process.h"\n#include <stdlib.h>\n' + source
    source = replace_once(
        source,
        'extern const VoidFunc _dtors[];\n',
        '''extern const VoidFunc _dtors[];

static void qa_inst_game_return(void)
{
    HuPrcVSleep();
    mp6_event_post("qa.inst.game_return", 0, "return");
    omOvlReturnEx(1, 1);
    for (;;) {
        HuPrcVSleep();
    }
}
''')
    source = replace_once(
        source,
        '''    fn_1_1590();
    return 0;''',
        '''    fn_1_1590();
    mp6_event_post("qa.inst.game", 0, "m616");
    if (getenv("MP6_QA_INST_RETURN")) {
        HuPrcChildCreate(qa_inst_game_return, 0x100, 0x2000, 0,
                         HuPrcCurrentGet());
    }
    return 0;''')
    return source


def instrument_bridge(source):
    """Reload instdll once after the real m616 return, then end the QA run."""
    source = replace_once(
        source,
        '    static int checked;\n    const char *raw;',
        '''    static int checked;
    static int qaInstReloaded;
    static int qaInstEnded;
    const char *raw;''')
    source = replace_once(
        source,
        '    if (checked) return;',
        '''    if (getenv("MP6_QA_INST_END") && !qaInstEnded &&
        omcurovl == DLL_mdseldll) {
        if (!qaInstReloaded &&
            mp6_event_matches("qa.inst.game_return", "return")) {
            qaInstReloaded = 1;
            g_forcedTestOverlay = DLL_instdll;
            if (!HuPrcChildCreate(mp6_dll_bridge_selftest_child, 0x100,
                                  0x2000, 0, HuPrcCurrentGet())) {
                fprintf(stderr, "[DLLSTUB-TEST] could not create inst reload process\\n");
            }
        } else if (mp6_event_matches("qa.inst.back", "return")) {
            qaInstEnded = 1;
            mp6_max_ticks = (int)mp6_tick_count + 120;
            mp6_ticks_unlimited = 0;
        }
    }
    if (checked) return;''')
    return source


def main():
    os.environ.setdefault('MP6_DISC_ROOT', str(ROOT / 'build/disc-cache/orig/GP6E01'))
    os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT / 'build/disc-cache/split/include'))
    sys.path.insert(0, str(ROOT / 'tools'))
    import build

    output = ROOT / 'build/inst-qa'
    output.mkdir(exist_ok=True)
    original_configure, original_collect = build.configure_windows, build.collect_units
    game = int(os.environ.get('MP6_QA_INST_GAME', '616'))
    names = (Path(build.DECOMP) / 'include/messnum/mg_name.h').read_text()
    entry = re.search(r'\bMG_NAME_M' + str(game) + r',\s*//(\d+)', names)
    if entry is None:
        raise ValueError(f'no recovered minigame name for M{game}')
    game_no = int(entry.group(1))
    objects = None

    def configure(configuration):
        nonlocal objects
        original_configure(configuration)
        objects = Path(build.OBJ_DIR)
        build.BUILD_DIR, build.OBJ_DIR = str(output), str(output / 'obj')

    def collect(*args, **kwargs):
        units = []
        for source, flags, name, flavor in original_collect(*args, **kwargs):
            path = Path(source)
            changed = None
            if path.parent.name == 'instDll' and path.name == 'setup.c':
                changed = instrument_inst_setup(path.read_text(), game, game_no)
            elif path.parent.name == 'm616dll' and path.name == 'prolog.c':
                changed = instrument_m616_prolog(path.read_text())
            elif path.name == 'dll_bridge.c':
                changed = instrument_bridge(path.read_text())
            obj = objects / name
            if changed is not None:
                source = str(output / (name[:-2] + '_probe.c'))
                Path(source).write_text(changed)
                obj = output / 'obj' / name
            units.append((source, flags, str(obj), flavor))
        return units

    build.configure_windows, build.collect_units = configure, collect
    sys.argv = ['tools/build.py', '--configuration', 'release', '-j2']
    return build.main()


if __name__ == '__main__':
    raise SystemExit(main())
