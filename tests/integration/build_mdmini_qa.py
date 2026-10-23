"""Build an isolated lifecycle probe over the upstream's two entrance owners.

Only event logging and a second overlay request are added. Gameplay, the
normal executable, player saves and the dependency checkout are unchanged.
"""
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.test_inst_integration import function_definition_body


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('mdmini QA anchor changed: ' + before)
    return text.replace(before, after, 1)


def instrument_menu(source):
    source = '#include "mp6_events.h"\n#include "mp6_boot.h"\n' + source
    original = function_definition_body(source, 'fn_1_1E86C')
    changed = replace_once(original, 's16 fn_1_1E86C(s32 unused)\n{', '''s16 fn_1_1E86C(s32 unused)
{
    static int qaEntries;
    int qaPrevious = -1;
    ++qaEntries;
''')
    changed = replace_once(changed, '        switch (state) {', '''        if (state != qaPrevious) {
            static const char *names[] = {"intro", "players", "characters", "modes", "options", "launch"};
            mp6_event_post("qa.mdmini.stage", state, names[state]);
            if (state == 3) mp6_event_post("qa.mdmini.mode.ready", 0, "ready");
            qaPrevious = state;
            if (qaEntries == 2 && state == 1) {
                mp6_event_post("qa.mdmini.complete", 2, "reentered");
                mp6_max_ticks = (int)mp6_tick_count + 120;
            }
        }
        switch (state) {''')
    source = replace_once(source, original, changed)
    source = replace_once(source, 's16 fn_1_181CC(void)\n{', '''s16 fn_1_181CC(void)
{
    mp6_event_post("qa.mdmini.confirm", 0, "open");''')
    anchor = '        omOvlReturnEx(1, 1);'
    if source.count(anchor) != 2:
        raise RuntimeError('mdmini exit routes changed')
    return source.replace(anchor, '        mp6_event_post("qa.mdmini.exit", 0, "done");\n'+anchor)


def instrument_particles(source):
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(source, 'void fn_1_20068(s16 layerNo)\n{', '''void fn_1_20068(s16 layerNo)
{
    static int qaFeedback;
    if (!qaFeedback++) mp6_event_post("qa.mdmini.effect", 0, "feedback");''')
    return replace_once(source, 'void fn_1_1FAF4(s16 effectIndex, Point3d *position)\n{', '''void fn_1_1FAF4(s16 effectIndex, Point3d *position)
{
    mp6_event_post("qa.mdmini.effect", 1, "refraction");''')


def instrument_mgmfree_handoff(source):
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(source, 'int _prolog(void)\n{', '''int _prolog(void)
{
    mp6_event_post("qa.mgmfree.enter", 0, "prolog");''')
    return replace_once(source, '    fn_1_3F20(0);\n    fn_1_51BC(1);\n    for (;;) {', '''    fn_1_3F20(0);
    fn_1_51BC(1);
    mp6_event_post("qa.mgmfree.ready", 0, "menu");
    for (;;) {''')


def instrument_bridge(source):
    source = replace_once(source, '    static int checked;\n    const char *raw;',
        '    static int checked;\n    static int reentered;\n    static int returned;\n    const char *raw;')
    return replace_once(source, '    if (checked) return;', '''    if (checked && !reentered && omcurovl == DLL_mdseldll &&
        mp6_event_matches("qa.mdmini.exit", "done")) {
        reentered = 1;
        checked = 0;
    }
    if (getenv("MP6_QA_MDMINI_HANDOFF") && !returned &&
        omcurovl == DLL_mgmfreedll && mp6_event_matches("qa.mgmfree.ready", "menu")) {
        returned = 1;
        mp6_event_post("qa.mdmini.complete", 0, "mgmfree-entered");
        mp6_max_ticks = (int)mp6_tick_count + 120;
    }
    if (checked) return;''')


def instrument_floor(source):
    return replace_once(source,
        '    tiledMiniFloor = mp6_ws_minigame_floor_tile(Hu3DData[dupId].hsf, objName);',
        '''    tiledMiniFloor = mp6_ws_minigame_floor_tile(Hu3DData[dupId].hsf, objName);
    if (getenv("MP6_WS_HSF_DIAG")) {
        printf("[MDMINI-QA] tile=%d\\n", tiledMiniFloor);
    }''')


def main():
    os.environ.setdefault('MP6_DISC_ROOT', str(ROOT / 'build/disc-cache/orig/GP6E01'))
    os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT / 'build/disc-cache/split/include'))
    sys.path.insert(0, str(ROOT / 'tools'))
    import build
    output = ROOT / 'build/mdmini-qa'
    output.mkdir(exist_ok=True)
    original_configure, original_collect = build.configure_windows, build.collect_units
    objects = None

    def configure(configuration):
        nonlocal objects
        original_configure(configuration)
        objects = Path(build.OBJ_DIR)
        build.BUILD_DIR, build.OBJ_DIR = str(output), str(output / 'obj')

    transforms = {
        ('mdminidll', 'menu_setup.c'): instrument_menu,
        ('mdminidll', 'particle_effects.c'): instrument_particles,
        ('mgmfreedll', 'menu.c'): instrument_mgmfree_handoff,
        ('os', 'dll_bridge.c'): instrument_bridge,
        ('hsf', 'mp6_widescreen_extrude.c'): instrument_floor,
    }

    def collect(*args, **kwargs):
        units = []
        matched = set()
        for source, flags, name, flavor in original_collect(*args, **kwargs):
            path = Path(source)
            key = (path.parent.name.lower(), path.name)
            obj = objects / name
            if key in transforms:
                matched.add(key)
                changed = transforms[key](path.read_text())
                source = str(output / (name[:-2] + '_probe.c'))
                Path(source).write_text(changed)
                obj = output / 'obj' / name
            units.append((source, flags, str(obj), flavor))
        if matched != transforms.keys():
            raise RuntimeError(f'QA owners missing: {transforms.keys() - matched}')
        return units

    build.configure_windows, build.collect_units = configure, collect
    sys.argv = ['tools/build.py', '--configuration', 'release', '-j2']
    return build.main()


if __name__ == '__main__':
    raise SystemExit(main())
