"""Build an isolated Mini-game Tour lifecycle probe.

The probe adds event markers and a deterministic trigger-token extension to a
QA-only executable.  It never changes the checked-out decompilation, the
normal executable, player saves, or the runtime's production input parser.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('mgmfree QA anchor changed: ' + before)
    return text.replace(before, after, 1)


def instrument_mgmfree_menu(source):
    """Add opt-in lifecycle/menu events without changing menu decisions."""
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(source, 'void fn_1_1C74(OMOBJ *obj)\n{',
        'void fn_1_1C74(OMOBJ *obj)\n{\n'
        '    static int qaFlowerEdge;\n'
        '    static unsigned qaSample;\n'
        '    if (getenv("MP6_WS_HSF_DIAG") && (qaSample % 60) == 0) {\n'
        '        for (int layer = 0; layer < 10; ++layer) {\n'
        '            HU3D_MODEL *model = &Hu3DData[obj->mdlId[layer]];\n'
        '            OSReport("[BUS-SAMPLE] tick=%u layer=%d time=%f modelx=%f\\n",\n'
        '                qaSample, layer, Hu3DMotionTimeGet(obj->mdlId[layer]), model->pos.x);\n'
        '            for (int o = 0; o < model->hsf->objectNum; ++o) {\n'
        '                HSF_OBJECT *object = &model->hsf->object[o];\n'
        '                if (object->type != HSF_OBJ_MESH && object->type != HSF_OBJ_ROOT) continue;\n'
        '                HuVecF world, screen;\n'
        '                Hu3DModelObjPosGet(obj->mdlId[layer], object->name, &world);\n'
        '                Hu3D3Dto2D(&world, 64, &screen);\n'
        '                OSReport("[BUS-POS] tick=%u layer=%d object=%s localx=%f worldx=%f screenx=%f\\n",\n'
        '                    qaSample, layer, object->name, object->mesh.curr.pos.x, world.x, screen.x);\n'
        '            }\n'
        '        }\n'
        '    }\n'
        '    ++qaSample;\n'
        '    float qaFlowerPhase = Hu3DMotionTimeGet(obj->mdlId[1]);\n'
        '    if (qaFlowerPhase >= 1074.0f && !qaFlowerEdge)\n'
        '        mp6_event_post("qa.bus.flowers.edge", 0, "wrap");\n'
        '    qaFlowerEdge = qaFlowerPhase >= 1074.0f;')
    source = replace_once(source, 'int _prolog(void)\n{', '''int _prolog(void)
{
    mp6_event_post("qa.mgmfree.enter", 0, "prolog");''')
    source = replace_once(source, 'void _epilog(void)\n{', '''void _epilog(void)
{
    mp6_event_post("qa.mgmfree.epilog", 0, "done");''')
    source = replace_once(source, 's32 fn_1_AB00(void)\n{', '''s32 fn_1_AB00(void)
{
    mp6_event_post("qa.mgmfree.intro", 0, "start");''')
    source = replace_once(source, '    fn_1_51BC(1);\n    for (;;) {', '''    fn_1_51BC(1);
    mp6_event_post("qa.mgmfree.ready", 0, "menu");
    for (;;) {''')
    source = replace_once(source, 's16 fn_1_B160(void)\n{', '''s16 fn_1_B160(void)
{
    mp6_event_post("qa.mgmfree.back_prompt", 0, "open");''')
    source = replace_once(source, '    fn_1_1400(2);\n    obj->objFunc = fn_1_1C74;', '''    fn_1_1400(2);
    if (getenv("MP6_WS_HSF_DIAG")) {
        for (int model = 0; model < 15; ++model) {
            char label[32];
            snprintf(label, sizeof(label), "bus-layer-%d", model);
            mp6_widescreen_debug_dump_model(obj->mdlId[model], label);
            mp6_widescreen_debug_dump_world_bbox(obj->mdlId[model], label);
            if (model < 10) {
                HSF_DATA *hsf = Hu3DData[obj->mdlId[model]].hsf;
                for (int o = 0; o < hsf->objectNum; ++o) {
                    HSF_OBJECT *object = &hsf->object[o];
                    OSReport("[BUS-OBJECT] layer=%d object=%d name=%s type=%d children=%d x=%f\\n",
                        model, o, object->name, object->type,
                        object->mesh.childNum, object->mesh.base.pos.x);
                }
                /* HSF motionNum stores the first motion's track count. */
                if (hsf->motion) {
                    HSF_MOTION *motion = hsf->motion;
                    for (int t = 0; t < motion->numTracks; ++t) {
                        HSF_TRACK *track = &motion->track[t];
                        if (track->type != HSF_TRACK_TRANSFORM ||
                            track->channel != HSF_CHANNEL_POSX) continue;
                        OSReport("[BUS-MOTION] layer=%d target=%d curve=%d keys=%d duration=%f\\n",
                            model, track->target, track->curveType,
                            track->numKeyframes, motion->maxTime);
                    }
                }
            }
        }
    }
    obj->objFunc = fn_1_1C74;''')
    source = replace_once(source, '''    result = fn_1_B00(1, 2);
    if (result != 0) {''', '''    result = fn_1_B00(1, 2);
    mp6_event_post("qa.mgmfree.back", result,
                   result == 0 ? "quit" : "cancel");
    if (result != 0) {''')

    # L/R are the category controls in the recovered source.  The event is
    # emitted only after the original state update, so a passing run proves
    # the menu accepted the input rather than merely observing a key press.
    source = replace_once(source, '''        change = 1;
        fn_1_8658();
    } else if (HuPadBtn[0] & PAD_TRIGGER_R) {''', '''        change = 1;
        mp6_event_post("qa.mgmfree.category", lbl_1_bss_9D0[0], "left");
        fn_1_8658();
    } else if (HuPadBtn[0] & PAD_TRIGGER_R) {''')
    source = replace_once(source, '''        change = 1;
        fn_1_8658();
    } else if (HuPadDStkRep[0] & PAD_BUTTON_RIGHT) {''', '''        change = 1;
        mp6_event_post("qa.mgmfree.category", lbl_1_bss_9D0[0], "right");
        fn_1_8658();
    } else if (HuPadDStkRep[0] & PAD_BUTTON_RIGHT) {''')
    source = replace_once(source, '''            lbl_1_bss_9D0[2] = 1;
            change = 2;
        }
    } else if (HuPadDStkRep[0] & PAD_BUTTON_LEFT) {''', '''            lbl_1_bss_9D0[2] = 1;
            change = 2;
            mp6_event_post("qa.mgmfree.item", lbl_1_bss_9D0[2], "right");
        }
    } else if (HuPadDStkRep[0] & PAD_BUTTON_LEFT) {''')
    source = replace_once(source, '    fn_1_3AE8();\n    for (;;) {',
        '    fn_1_3AE8();\n    mp6_event_post("qa.mgmfree.group", 0, "ready");\n    for (;;) {')
    return source


def instrument_instruction(source):
    """Observe the actual bus-selected game, without changing its route."""
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(source, '    lbl_1_bss_36 = GwSystem.mgNo;',
        '    mp6_event_post("qa.inst.enter", GwSystem.mgNo, "selected");\n'
        '    lbl_1_bss_36 = GwSystem.mgNo;')
    return replace_once(source, '    lbl_1_bss_6 = 2;\n\n    flag_2 = 0;',
        '    lbl_1_bss_6 = 2;\n    mp6_event_post("qa.inst.ready", 0, "menu");\n\n'
        '    flag_2 = 0;')


def instrument_bridge(source):
    """Permit exactly one in-process MGMfree reload for the QA run."""
    source = replace_once(source, '    mp6_dll_bridge_selftest_teams(g_forcedTestOverlay);', '''    mp6_dll_bridge_selftest_teams(g_forcedTestOverlay);
    /* In-memory fixture only: exercise previews without reading/writing a save. */
    if (getenv("MP6_QA_MGMFREE_UNLOCK")) {
        for (int game = 0; MgDataTbl[game].ovl != 65535; ++game)
            GWMgUnlockSet(game + 601);
    }''')
    source = replace_once(source, '    static int checked;\n    const char *raw;',
                          '    static int checked;\n    static int qaReloaded;\n    static int qaReturned;\n    static unsigned long qaFirstReturnSeq;\n    const char *raw;')
    source = replace_once(source, '    if (checked) return;', '''    if (checked && !qaReloaded &&
        getenv("MP6_QA_MGMFREE_RELOAD") && omcurovl == DLL_mdseldll &&
        mp6_event_matches("qa.mgmfree.back", "quit")) {
        qaReloaded = 1;
        qaFirstReturnSeq = mp6_event_last_seq("qa.mgmfree.back");
        checked = 0;
    }
    if (!qaReturned && omcurovl == DLL_mdseldll &&
        mp6_event_matches("qa.mgmfree.back", "quit") &&
        (!getenv("MP6_QA_MGMFREE_RELOAD") ||
         (qaReloaded && mp6_event_last_seq("qa.mgmfree.back") > qaFirstReturnSeq))) {
        qaReturned = 1;
        mp6_event_post("qa.mgmfree.complete", 0, "returned");
        mp6_max_ticks = (int)mp6_tick_count + 120;
    }
    if (checked) return;''')
    return source


def instrument_script(source):
    """Add L/R shoulder tokens used only by this generated QA executable."""
    return replace_once(
        source,
        '    if (len == 5 && strncmp(name, "start", 5) == 0) return PAD_BUTTON_START;',
        '''    if (len == 5 && strncmp(name, "start", 5) == 0) return PAD_BUTTON_START;
    if (len == 1 && name[0] == 'l') return PAD_TRIGGER_L;
    if (len == 1 && name[0] == 'r') return PAD_TRIGGER_R;''')


def instrument_movie(source):
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(source, '    SimpleControl.movieOpen = 1;',
                          '    mp6_event_post("qa.thp.open", 0, moviePath);\n'
                          '    SimpleControl.movieOpen = 1;')
    return replace_once(source,
        '    SimpleControl.videoBuffers[0].frameIndex = SimpleControl.nextFrameNumber;',
        '    SimpleControl.videoBuffers[0].frameIndex = SimpleControl.nextFrameNumber;\n'
        '    mp6_event_post("qa.thp.frame", SimpleControl.nextFrameNumber, "decoded");')


def instrument_dvd_root(source):
    """Honor the requested runtime content root before any DVD file opens."""
    source = '#include "mp6_dvd_files.h"\n' + source
    return replace_once(source, '    if (g_pathsResolved) return;', '''    if (g_pathsResolved) return;
    const char *qaRoot = getenv("MP6_QA_DISC_ROOT");
    if (qaRoot && *qaRoot) {
        char qaFiles[MP6_DVD_PATH_CAP], qaFst[MP6_DVD_PATH_CAP], qaError[1200];
        if (!mp6_dvd_validate_disc_root(qaRoot, qaError, sizeof(qaError)) ||
            mp6_path_join_checked(qaFiles, sizeof(qaFiles), qaRoot, "files") ||
            mp6_path_join_checked(qaFst, sizeof(qaFst), qaRoot, "sys/fst.bin") ||
            mp6_dvd_set_root_override(qaFiles, qaFst)) abort();
        printf("[QA-CONTENT] root=%s\\n", qaRoot);
        return;
    }''')


def main():
    sys.path.insert(0, str(ROOT))
    from tests.integration.build_overlay_probe import build_probe
    return build_probe('mgmfree-qa', {
        ('mgmfreedll', 'menu.c'): instrument_mgmfree_menu,
        ('instDll', 'setup.c'): instrument_instruction,
        ('os', 'dll_bridge.c'): instrument_bridge,
        ('os', 'input_script.c'): instrument_script,
        ('video', 'thp_player.c'): instrument_movie,
        ('dvd', 'dvd_files.c'): instrument_dvd_root,
    })


if __name__ == '__main__':
    raise SystemExit(main())
