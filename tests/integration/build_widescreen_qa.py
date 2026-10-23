"""Isolated scene/camera QA: original gameplay, timed exit and state logging only."""
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from native_minigames import MODULES

COMPILED_MINIGAMES = tuple(sorted({name.lower() for name in MODULES} | {'m616dll', 'm670dll'}))

MINIGAME_SETUP = r'''
#include "game/board/main.h"
#include "game/flag.h"
void mbSaveStoryInit(s32 teamChar, s32 mgPack, s32 storyComDif);
/* A fresh QA card has no unlocked games. Direct mode entry must reproduce the
 * menu's admission prerequisites, restricted to this build's real overlays. */
static void qa_prepare_mode(s16 overlay)
{
    switch (overlay) {
    case DLL_mdminidll:
    case DLL_mgmfreedll:
    case DLL_mgmbattledll:
    case DLL_mgmbingodll:
    case DLL_mgmdecathlondll:
    case DLL_mgmrenshodll:
    case DLL_mgmtournamentdll:
        break;
    default: return;
    }
    int unlocked = 0;
    for (int game = 0; (s16)MgDataTbl[game].ovl != DLL_NONE; ++game) {
        switch (MgDataTbl[game].ovl) {
        /* COMPILED_MINIGAME_CASES */
            GWMgUnlockSet(game + GW_MGNO_BASE);
            ++unlocked;
            break;
        default: break;
        }
    }
    printf("[WIDE-QA] mode admission unlocked=%d\n", unlocked);
}
/* Direct board loads need the rules/player state normally supplied by the
 * Party menu. Use the game's initializer; never replace board gameplay. */
static void qa_prepare_board(s16 overlay)
{
    int board;
    switch (overlay) {
    case DLL_w01dll: board = 0; break;
    case DLL_w02dll: board = 1; break;
    case DLL_w03dll: board = 2; break;
    case DLL_w04dll: board = 3; break;
    case DLL_w05dll: board = 4; break;
    case DLL_w06dll: board = 5; break;
    case DLL_s01dll: board = 6; break;
    case DLL_s02dll: board = 7; break;
    case DLL_s03dll: board = 8; break;
    case DLL_w10dll: board = 9; break;
    case DLL_w11dll: board = 10; break;
    default: return;
    }
    mbSaveInit(board);
    if (board >= 6 && board <= 8)
        mbSaveStoryInit(GwPlayerConf[1].charNo, 0, 1);
    else if (board != 10)
        mbSavePartyInit(FALSE, TRUE, 0, 20, 0, 0, 0, 0);
    for (int p = 0; p < GW_PLAYER_MAX; ++p) {
        GwPlayer[p].charNo = GwPlayerConf[p].charNo;
        GwPlayer[p].padNo = GwPlayerConf[p].padNo;
        GwPlayer[p].comF = TRUE;
        GwPlayer[p].comDif = 1;
        GwPlayer[p].team = 0;
    }
}
/* Direct scene captures skip the mode menu. Supply the normal launch
 * prerequisites, not substitute gameplay or camera behavior. */
static void qa_prepare_minigame(s16 overlay)
{
    qa_prepare_mode(overlay);
    /* Results normally inherit a completed Party game's rules. Fresh-card
     * turnMax == 0 is not a legal results launch (its heading starts at 10). */
    qa_prepare_board(overlay == DLL_mdpresultdll ? DLL_w01dll : overlay);
    int game = MgNoGet(overlay);
    if (game < 0) return;
    const char *night = getenv("MP6_WS_QA_NIGHT");
    if (night) GwMgNightF = strcmp(night, "1") == 0;
    const char *cpu = getenv("MP6_WS_QA_ALL_CPU");
    const char *difficultyText = getenv("MP6_WS_QA_CPU_DIFFICULTY");
    int difficulty = difficultyText ? atoi(difficultyText) : 0;
    const char *levels = getenv("MP6_WS_QA_CPU_LEVELS");
    int levelsValid = levels && strlen(levels) == GW_PLAYER_MAX;
    for (int p = 0; levelsValid && p < GW_PLAYER_MAX; ++p)
        if (levels[p] < '0' || levels[p] > '3') levelsValid = 0;
    int type = MgDataTbl[game].type;
    for (int p = 0; p < GW_PLAYER_MAX; ++p) {
        int group = p;
        if (type == MG_TYPE_1VS3) group = p != 0;
        else if (type == MG_TYPE_2VS2) group = p / 2;
        GwPlayerConf[p].grpNo = group;
        if (cpu && strcmp(cpu, "1") == 0) {
            GwPlayerConf[p].type = 1;
            if (levelsValid) GwPlayerConf[p].comDif = levels[p] - '0';
            else if (difficulty >= 0 && difficulty <= 3) GwPlayerConf[p].comDif = difficulty;
        }
        CharMotionInit(GwPlayerConf[p].charNo);
    }
    printf("[WIDE-QA] minigame setup game=%d type=%d groups=%d,%d,%d,%d\n",
           game, type, GwPlayerConf[0].grpNo, GwPlayerConf[1].grpNo,
           GwPlayerConf[2].grpNo, GwPlayerConf[3].grpNo);
}
'''.replace('        /* COMPILED_MINIGAME_CASES */', '\n'.join(
    '        case DLL_' + name + ':' for name in COMPILED_MINIGAMES))


SOLO_RESULTS_SETUP = r'''
#include "game/saveload.h"
/* Legal completed-board state, not replacement results/reward logic. The
 * runner only enables this with a copied, attested QA card. */
static void qa_prepare_solo_results(void)
{
    mbSaveInit(6);
    mbSaveStoryInit(1, 0, 1);
    GWSingleDataInit();
    GWSingleMgRecordNumSet(0);
    GWSingleMgWinNumSet(0);
    GwCommon.confSingleDiff = 1;
    GwCommon.storyMgPack = 0;
    GwSystem.turnPlayerNo = 0;
    GwSystem.turnNo = 3; /* (3 - 1) * 10 coins -> one Star. */
    GwPlayer[0].charNo = 0;
    GwPlayer[0].padNo = 0;
    GwPlayer[0].comF = FALSE;
    GwPlayer[0].comDif = 0;
    GwPlayer[0].team = 0;
    GWMgCoinBonusSet(0, 1);
    SLSaveFlagSet(TRUE);
}
'''


def once(text, before, after):
    if text.count(before) != 1: raise RuntimeError('QA anchor changed: '+before)
    return text.replace(before, after, 1)


def m659_gradient_probe(source):
    """Negative control changes only this asset repair in the QA executable."""
    source = '#include <stdlib.h>\n' + source
    return once(source,
                '    mp6_widescreen_extend_color_rim_bottom(model, "642nightSKYa1",\n'
                '                                          0.0f, 21.9824f, -18000.0f);',
                '    if (getenv("MP6_WS_QA_ORIGINAL_GRADIENT"))\n'
                '        mp6_widescreen_extend_color_rim(model, "642nightSKYa1", 2, 0.0f, 21.9824f);\n'
                '    else\n'
                '        mp6_widescreen_extend_color_rim_bottom(model, "642nightSKYa1",\n'
                '                                              0.0f, 21.9824f, -18000.0f);')


def solo_results_probe(source, bank=False):
    """Observe the real panel and shared save-award screen, not star model 143."""
    source = '#include <stdlib.h>\n#include "mp6_events.h"\n' + source
    if bank:
        anchor = 'static HUWINID SLBankAwardExec(s16 award)\n{'
        return once(source, anchor, anchor + '\n'
                    '    if (getenv("MP6_WS_QA_SOLO_RESULTS"))\n'
                    '        mp6_event_post("qa.mdsing.bank-ready", award, "ready");')
    anchor = '    mdsingResultPanelOpen();'
    return once(source, anchor, anchor + '\n'
                '    if (getenv("MP6_WS_QA_SOLO_RESULTS"))\n'
                '        mp6_event_post("qa.mdsing.result-ready", 0, "ready");')


PARTY_MENU_ENTRIES = (
    ('s16 fn_1_30B4C(void)', 'intro'),
    ('s16 fn_1_38A3C(s16 arg0)', 'board'),
    ('s16 fn_1_3CFE4(s16 arg0)', 'rules'),
    ('s16 fn_1_31AC0(s16 arg0)', 'players'),
    ('s16 fn_1_329A4(s16 arg0)', 'characters'),
    ('s16 fn_1_39C60(s16 arg0)', 'handicap'),
    ('s16 fn_1_3BEA8(void)', 'confirm'),
    ('void fn_1_3CA70(void)', 'launch'),
)

BOARD_TURN_ENTRIES = {
    'w03dll': 'void fn_1_1097C(int playerNo) {',
    'w04dll': 'void fn_1_DF4(int playerNo)\n{',
}


def party_menu_probe(source):
    """Observe native menu entry points; never set their selections or state."""
    source = '#include "mp6_events.h"\n' + source
    for declaration, stage in PARTY_MENU_ENTRIES:
        # Parameter names are not part of the observed entrypoint's ABI.
        # Still require its exact return/argument types and one definition.
        pattern = re.escape(declaration).replace(r'\ arg0', r'\s+[A-Za-z_]\w*')
        matches = list(re.finditer(r'^' + pattern + r'\s*\{', source, re.M))
        if len(matches) != 1:
            raise RuntimeError('QA anchor changed: ' + declaration)
        anchor = matches[0].group()
        source = once(source, anchor, anchor +
                      f'\n    mp6_event_post("qa.party.stage", 0, "{stage}");')
    return source


def mode_select_probe(source):
    """Observe the native mdsel -> Party choice in QA-generated source."""
    source = '#include "mp6_events.h"\n' + source
    anchor = '            omOvlCall(DLL_mdpartydll, 0, 0);'
    replacement = '''            {
                static const char *const qa_mode_name[] = {
                    "party", "minigame", "extras", "unknown"
                };
                int qa_mode = lbl_1_bss_1A30[0];
                const char *qa_name = qa_mode >= 0 && qa_mode < 3
                    ? qa_mode_name[qa_mode] : qa_mode_name[3];
                mp6_event_post("qa.mdsel.choice", qa_mode, qa_name);
            }
''' + anchor
    return once(source, anchor, replacement)


AO_LAYER_TRACE = r'''
/* QA-only AO scope trace.  The release build never receives this source. */
static int qa_ao_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MP6_AO_TRACE");
        enabled = value && strcmp(value, "1") == 0;
    }
    return enabled;
}

static int qa_ao_active_layer = -1;

static void qa_ao_layer_event(const char *event, int layer, const char *reason)
{
    if (!qa_ao_trace_enabled()) return;
    mp6_event_post(event, layer, reason);
    printf("[AO-QA-LAYER] tick=%ld layer=%d reason=%s\n",
           mp6_tick_count, layer, reason);
    fflush(stdout);
}
'''


def layer_scope_probe(source):
    """Trace clear-layer ownership in the isolated generated hsfman unit."""
    source = ('#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n'
              '#include "mp6_boot.h"\n#include "mp6_events.h"\n' +
              AO_LAYER_TRACE + '\n' + source)
    source = once(source, '                hookFunc(layer);', '''                qa_ao_active_layer = layer;
                qa_ao_layer_event("qa.ao.layer.hook", layer, "prehook");
                hookFunc(layer);
                qa_ao_layer_event("qa.ao.layer.hook", layer, "posthook");
                qa_ao_active_layer = -1;''')
    source = once(source, 'void Hu3DLayerHookReset(s16 layerNo)\n{', '''void Hu3DLayerHookReset(s16 layerNo)
{
    qa_ao_layer_event("qa.ao.layer.reset", layerNo, "reset");''')
    source = once(source, 'void Hu3DZClearLayerSet(s16 layerNo)\n{', '''void Hu3DZClearLayerSet(s16 layerNo)
{
    qa_ao_layer_event("qa.ao.layer.install", layerNo, "clear");''')
    source = once(source, '    mp6_ao_depth_cleared(Hu3DCameraNo);', '''    mp6_ao_depth_cleared(Hu3DCameraNo);
    qa_ao_layer_event("qa.ao.layer.clear", qa_ao_active_layer, "depth-clear");''')
    return source


AO_SCOPE_TRACE = r'''
/* QA-only, opt-in AO scope trace.  It is compiled into a generated probe
 * source and is never linked into the production executable. */
static int qa_ao_scope_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MP6_AO_TRACE");
        enabled = value && strcmp(value, "1") == 0;
    }
    return enabled;
}

static unsigned qa_ao_scope_epoch[HU3D_CAM_MAX];
static unsigned qa_ao_scope_count[HU3D_CAM_MAX];

static void qa_ao_scope_epoch_reset(int camera)
{
    if (camera < 0 || camera >= HU3D_CAM_MAX) return;
    ++qa_ao_scope_epoch[camera];
    qa_ao_scope_count[camera] = 0;
}

static void qa_ao_scope_trace(int camera, float fov, const float *viewport,
                               const char *reason)
{
    unsigned count = 0;
    unsigned epoch = 0;
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    if (!qa_ao_scope_trace_enabled()) return;
    if (camera >= 0 && camera < HU3D_CAM_MAX) {
        count = ++qa_ao_scope_count[camera];
        epoch = qa_ao_scope_epoch[camera];
    }
    if (viewport) {
        vx = viewport[0]; vy = viewport[1];
        vw = viewport[2]; vh = viewport[3];
    }
    printf("[AO-QA-TRACE] tick=%ld camera=%d fov=%.4f vp=%.4f,%.4f,%.4f,%.4f reason=%s count=%u epoch=%u\n",
           mp6_tick_count, camera, fov, vx, vy, vw, vh, reason, count, epoch);
    fflush(stdout);
}
'''


def ao_scope_probe(source):
    """Add opt-in camera/epoch evidence to generated AO renderer code."""
    source = ('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
              '#include "mp6_boot.h"\n' + source)
    source = once(source, '#include "mp6_enhancements.h"\n',
                  '#include "mp6_enhancements.h"\n' + AO_SCOPE_TRACE + '\n')
    source = once(source, 'void mp6_ao_camera_snapshot_reset(int camera)\n{', '''void mp6_ao_camera_snapshot_reset(int camera)
{
    qa_ao_scope_epoch_reset(camera);''')
    source = once(source, '    if (!hasDepth) {\n', '''    if (!hasDepth) {
        qa_ao_scope_trace(camera,
                          camera >= 0 && camera < HU3D_CAM_MAX ? Hu3DCamera[camera].fov : 0.0f,
                          NULL,
                          "no-depth-epoch");
''')
    source = once(source, '    if (!view) {\n', '''    if (!view) {
        qa_ao_scope_trace(camera,
                          camera >= 0 && camera < HU3D_CAM_MAX ? Hu3DCamera[camera].fov : 0.0f,
                          NULL,
                          "no-view");
''')
    source = once(source, '    mp6_ao_apply(view);\n', '''    mp6_ao_apply(view);
    qa_ao_scope_trace(camera,
                      atanf(view->projection[0]) * 114.591559f,
                      view->viewport,
                      "apply");
''')
    return source


RESIZE_DRIVER = r'''
/* QA-only, at a closed frame boundary. No input, focus or gameplay changes. */
static void qa_resize_tick(void)
{
    static long start = -1;
    static int next, pending = -1;
    static long changed;
    static const int widths[] = {640,848,1120,1120,1120,848,640,640,848};
    static const int enabled[] = {0,1,1,0,1,1,1,0,1};
    const char *target = getenv("MP6_WS_QA_SCENE");
    if (!getenv("MP6_WS_QA_RESIZE") || !target) return;
    if (start < 0) {
        if (!mp6_event_matches("ovl.start", target)) return;
        start = mp6_tick_count;
    }
    SDL_Window *window = mp6_aspect_window();
    if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) {
        fprintf(stderr,"[FATAL] resize QA requires its own hidden window\n");
        exit(3);
    }
    if (pending >= 0 && mp6_tick_count - changed >= 12) {
        int width, height;
        SDL_GetWindowSize(window, &width, &height);
        printf("[WIDE-RESIZE] settled step=%d tick=%ld window=%dx%d wide=%d logical=%d hidden=1\n",
               pending,mp6_tick_count,width,height,mp6_widescreen_enabled(),mp6_widescreen_render_width());
        fflush(stdout);
        pending = -1;
    }
    if (next < 9 && mp6_tick_count - start >= 600 + next * 120) {
        Mp6EnhValues values;
        mp6_enh_get_values(&values);
        values.widescreen = enabled[next];
        mp6_enh_set_values(&values);
        mp6_widescreen_set_enabled(enabled[next]);
        /* Leave the window freely sized when the content is native: FIT must
         * letterbox it, not rely on resizing the physical window to hide it. */
        if (!SDL_SetWindowAspectRatio(window,0.0f,0.0f) ||
            !SDL_SetWindowSize(window,widths[next],480)) {
            fprintf(stderr,"[FATAL] resize QA: %s\n",SDL_GetError());
            exit(3);
        }
        mp6_bridge_apply_content_aspect_policy(1);
        printf("[WIDE-RESIZE] request step=%d tick=%ld\n",next,mp6_tick_count);
        pending = next++;
        changed = mp6_tick_count;
    }
}
'''


def resize_probe(source):
    source = once(source, 'void VIWaitForRetrace(void)\n{',
                  'static void qa_resize_tick(void);\n\nvoid VIWaitForRetrace(void)\n{')
    source = once(source, '    mp6_dispatch_aurora_events(aurora_update());',
                  '    qa_resize_tick();\n    mp6_dispatch_aurora_events(aurora_update());')
    return source + RESIZE_DRIVER


def bus_motion_probe(source):
    """Read actual lamp vertices at every tick; old-clock control is QA-only."""
    source = '#include "mp6_events.h"\n' + source
    source = once(source, 'void fn_1_1C74(OMOBJ *obj)\n{', '''void fn_1_1C74(OMOBJ *obj)
{
    static unsigned qaBusTick;
    static int qaBusEdge;
    if (getenv("MP6_WS_QA_BUS")) {
        HU3D_MODEL *model = &Hu3DData[obj->mdlId[5]];
        HU3D_CAMERA *camera = &Hu3DCamera[6];
        Mtx view;
        float left = 1.0e9f, right = -1.0e9f;
        float width = mp6_widescreen_render_width();
        float tanHalf = tanf(camera->fov * 0.00872664626f);
        MTXLookAt(view, &camera->pos, &camera->up, &camera->target);
        for (int i = 0; i < model->hsf->objectNum; ++i) {
            HSF_OBJECT *object = &model->hsf->object[i];
            if (object->type != HSF_OBJ_MESH || !object->mesh.vertex) continue;
            Mtx world;
            HuVecF *vertices = object->mesh.vertex->data;
            Hu3DModelObjMtxGet(obj->mdlId[5], object->name, world);
            for (int v = 0; v < object->mesh.vertex->count; ++v) {
                HuVecF position, projected;
                PSMTXMultVec(world, &vertices[v], &position);
                PSMTXMultVec(view, &position, &projected);
                float x = width * 0.5f *
                    (1.0f + projected.x / (-projected.z * tanHalf * camera->aspect));
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
        OSReport("[BUS-EXIT] tick=%u phase=%.6f timer=%.6f k=%.6f left=%.6f right=%.6f width=%.6f\\n",
            qaBusTick, Hu3DMotionTimeGet(obj->mdlId[5]),
            lbl_1_bss_A8[5].value.countdown, mp6_widescreen_scale_factor(),
            left, right, width);
        int near = lbl_1_bss_A8[5].value.countdown <= 6.0f &&
                   Hu3DMotionTimeGet(obj->mdlId[5]) > 100.0f;
        if (near && !qaBusEdge) mp6_event_post("qa.bus.near-restart", 0, "edge");
        qaBusEdge = near;
        static int qaBusExit;
        int exiting = left > width - 20.0f && left < width;
        if (exiting && !qaBusExit) mp6_event_post("qa.bus.exit", 0, "edge");
        qaBusExit = exiting;
        ++qaBusTick;
    }
''')
    # Keep a reproducible negative control out of production. No mesh or
    # timing override is enabled unless explicitly requested for this probe.
    source = once(source, '    f32 travelStep = 1.0f / mp6_widescreen_scale_factor();',
        '    f32 travelStep = 1.0f / mp6_widescreen_scale_factor();\n'
        '    f32 qaBusTimerStep = getenv("MP6_WS_QA_BUS_OLD_TIMER") ? 1.0f : travelStep;')
    if source.count('record->value.countdown -= travelStep') != 2:
        raise ValueError('bus motion countdown anchors changed')
    return source.replace('record->value.countdown -= travelStep',
                          'record->value.countdown -= qaBusTimerStep')


def main():
    os.environ.setdefault('MP6_DISC_ROOT', str(ROOT/'build/disc-cache/orig/GP6E01'))
    os.environ.setdefault('MP6_DECOMP_INC_DATA', str(ROOT/'build/disc-cache/split/include'))
    sys.path.insert(0, str(ROOT/'tools'))
    import build
    output = ROOT/'build/widescreen-qa'
    output.mkdir(exist_ok=True)
    configure, collect = build.configure_windows, build.collect_units
    objects = None

    def configure_qa(configuration):
        nonlocal objects
        configure(configuration)
        objects = Path(build.OBJ_DIR)
        build.BUILD_DIR, build.OBJ_DIR = str(output), str(output/'obj')

    def collect_qa(*args, **kwargs):
        result = []
        for source, flags, name, flavor in collect(*args, **kwargs):
            path, changed = Path(source), None
            if path.name == 'dll_bridge.c':
                changed = once(path.read_text(encoding='utf-8'),
                    'mp6_max_ticks = (int)mp6_tick_count + 600;',
                    '/* Widescreen QA arms its budget on target scene entry. */')
                changed = once(changed, 'static OMOVL g_forcedTestOverlay;',
                    '#include "game/mgdata.h"\n#include "game/charman.h"\n'
                    + MINIGAME_SETUP + SOLO_RESULTS_SETUP + '\nstatic OMOVL g_forcedTestOverlay;')
                changed = once(changed, '    omOvlCallEx(g_forcedTestOverlay, TRUE, 0, 0);',
                    '    if (g_forcedTestOverlay == DLL_mdsingdll && getenv("MP6_WS_QA_SOLO_RESULTS")) {\n'
                    '        qa_prepare_solo_results();\n'
                    '        omOvlCallEx(g_forcedTestOverlay, TRUE, 1, 1);\n'
                    '    } else {\n'
                    '        qa_prepare_minigame(g_forcedTestOverlay);\n'
                    '        omOvlCallEx(g_forcedTestOverlay, TRUE, 0, 0);\n'
                    '    }')
            elif path.name == 'menu.c' and path.parent.name.lower() == 'mgmfreedll':
                changed = bus_motion_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'scene.c' and path.parent.name.lower() == 'm659dll':
                changed = m659_gradient_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'gamemes.c' and path.parent.name.lower() == 'game':
                changed = '#include "mp6_events.h"\n' + path.read_text(encoding='utf-8')
                anchor = 'void GameMesPauseCreate(void)\n{'
                changed = once(changed, anchor, anchor + '\n'
                               '    mp6_event_post("qa.pause", 0, "open");')
            elif path.name == 'mdsing.c' and path.parent.name.lower() == 'mdsingdll':
                changed = solo_results_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'saveload.c' and path.parent.name.lower() == 'game':
                changed = solo_results_probe(path.read_text(encoding='utf-8'), bank=True)
            elif path.name == 'mdsel.c' and path.parent.name.lower() == 'mdseldll':
                changed = mode_select_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'mdparty.c' and path.parent.name.lower() == 'mdpartydll':
                changed = party_menu_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'board.c' and path.parent.name.lower() in BOARD_TURN_ENTRIES:
                changed = '#include "mp6_events.h"\n' + path.read_text(encoding='utf-8')
                turn_entry = BOARD_TURN_ENTRIES[path.parent.name.lower()]
                changed = once(changed, turn_entry, turn_entry + '\n'
                               '    mp6_event_post("qa.board.turn", playerNo, "begin");')
            elif path.name == 'game.c' and path.parent.name.lower() == 'ttwarsdll':
                changed = '#include "mp6_events.h"\n' + path.read_text(encoding='utf-8')
                changed = once(changed, 'void fn_1_21B8(void)\n{',
                               'void fn_1_21B8(void)\n{\n'
                               '    mp6_event_post("qa.ttwars.piece", 0, "created");')
            elif path.name == 'math_camera.c' and path.parent.name.lower() == 'm658dll':
                changed = once(path.read_text(encoding='utf-8'),
                    '            rootModelId = Hu3DModelCreate(resource);',
                    '            rootModelId = Hu3DModelCreate(resource);\n'
                    '            printf("[WIDE-RESOURCE] m658 data=%08x model=%d\\n", '
                    'entry->resourceId, rootModelId);')
            elif path.name == 's03.c' and path.parent.name.lower() == 's03dll':
                changed = once(path.read_text(encoding='utf-8'),
                    '    modelId = mbObjCreate(DATANUM(DATA_s03, 4), NULL, FALSE);',
                    '    modelId = mbObjCreate(DATANUM(DATA_s03, 4), NULL, FALSE);\n'
                    '    printf("[WIDE-RESOURCE] s03 data=4 model=%d\\n", mbObjModelIDGet(modelId));')
            elif path.name == 'hsfman.c' and path.parent.name.lower() == 'game':
                changed = layer_scope_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'mp6_ambient_occlusion.c' and path.parent.name.lower() == 'hsf':
                changed = ao_scope_probe(path.read_text(encoding='utf-8'))
            elif path.name == 'aurora_bridge.c':
                changed = resize_probe(path.read_text(encoding='utf-8'))
                flags = [*flags, '-I'+str(path.parent)]
            elif path.name == 'game.c' and path.parent.name == 'm618dll':
                changed = '#include <stdlib.h>\n#include "mp6_events.h"\n#include "mp6_boot.h"\nstatic int qaWinnerReported;\n' + path.read_text(encoding='utf-8')
                changed = once(changed, 'void fn_1_1CE0(void)\n{', '''void fn_1_1CE0(void)
{
    qaWinnerReported = 0;
''')
                changed = once(changed, 'void fn_1_796C(int winner)\n{', '''void fn_1_796C(int winner)
{
    if (winner >= 0 && !qaWinnerReported) {
        qaWinnerReported = 1;
        mp6_event_post("qa.m618.winner", winner, "ready");
        const char *budget = getenv("MP6_WS_QA_WINNER_CAPTURE");
        if (budget) mp6_max_ticks = (int)mp6_tick_count + atoi(budget);
    }''')
            elif path.name == 'game.c' and path.parent.name.lower() == 'm640dll':
                changed = '#include <stdlib.h>\n#include "mp6_events.h"\n#include "mp6_boot.h"\n' + path.read_text(encoding='utf-8')
                changed = once(changed, '    if ((s16)frame == 60) {', '''    if ((s16)frame == 45) {
        mp6_event_post("qa.m640.winner", 0, "ready");
        const char *budget = getenv("MP6_WS_QA_WINNER_CAPTURE");
        if (budget) mp6_max_ticks = (int)mp6_tick_count + atoi(budget);
    }
    if ((s16)frame == 60) {''')
            elif path.name == 'game.c' and path.parent.name.lower() == 'm650dll':
                # All-CPU runs never set a record; the record scene stages the
                # Chomp and bone off screen, so let QA ask for it.
                changed = '#include <stdlib.h>\n' + path.read_text(encoding='utf-8')
                changed = once(changed, '(lbl_1_bss_28[playerIndex].computerDifficulty == -1) &&',
                    '(lbl_1_bss_28[playerIndex].computerDifficulty == -1 || getenv("MP6_WS_QA_M650_RECORD")) &&')
                changed = once(changed, '(elapsedTime < lbl_1_bss_0.record) &&',
                    '(elapsedTime < lbl_1_bss_0.record || getenv("MP6_WS_QA_M650_RECORD")) &&')
            elif path.name == 'sequence.c' and path.parent.name.lower() == 'm650dll':
                changed = '#include <stdlib.h>\n#include "mp6_events.h"\n#include "mp6_boot.h"\n' + path.read_text(encoding='utf-8')
                changed = once(changed, 'void fn_1_1228(void)\n{', '''void fn_1_1228(void)
{
    mp6_event_post("qa.m650.winner", lbl_1_bss_0.winner, "ready");
    {
        const char *budget = getenv("MP6_WS_QA_WINNER_CAPTURE");
        if (budget) mp6_max_ticks = (int)mp6_tick_count + atoi(budget);
    }''')
            elif path.name == 'camera.c' and path.parent.name.lower() == 'm659dll':
                changed = '#include <stdlib.h>\n#include "mp6_events.h"\n#include "mp6_boot.h"\n' + path.read_text(encoding='utf-8')
                changed = once(changed, '        switch (lbl_1_data_0->winnerCount) {', '''
        if (lbl_1_data_0->winnerCount == 1) {
            mp6_event_post("qa.m659.winner", lbl_1_data_0->winners[0], "ready");
            const char *budget = getenv("MP6_WS_QA_WINNER_CAPTURE");
            if (budget) mp6_max_ticks = (int)mp6_tick_count + atoi(budget);
        } else {
            mp6_event_post("qa.m659.result", lbl_1_data_0->winnerCount, "tie");
        }
        switch (lbl_1_data_0->winnerCount) {''')
            elif path.name == 'mp6_widescreen_camera.c':
                # Inline QA-only helpers so their content participates in the
                # generated-source hash, including edits outside include/.
                changed = ('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
                           '#include "game/object.h"\n#include "mp6_boot.h"\n#include "mp6_events.h"\n'
                           + (ROOT/'tests/integration/qa_scene_export.h').read_text(encoding='utf-8')
                           + '\n') + path.read_text(encoding='utf-8')
                changed = once(changed, '    offset[0] = offset[1] = offset[2] = 0.0f;\n',
                    '    offset[0] = offset[1] = offset[2] = 0.0f;\n'
                    '    if (getenv("MP6_WS_QA_NO_OFFSTAGE")) return 0;\n')
                changed = once(changed, '    for (i = 0; i < HU3D_CAM_MAX; ++i) apply_camera(i);', '''
    for (i = 0; i < HU3D_CAM_MAX; ++i) apply_camera(i);
    static unsigned long armedSeq;
    static long armedTick;
    static int exitRequested;
    {
        const char *target = getenv("MP6_WS_QA_SCENE");
        const char *budgetText = getenv("MP6_WS_QA_BUDGET");
        unsigned long seq = mp6_event_last_seq("ovl.start");
        if (target && budgetText && seq != armedSeq && mp6_event_matches("ovl.start", target)) {
            int budget = atoi(budgetText);
            if (getenv("MP6_WS_QA_WINNER_CAPTURE")) budget = 20000;
            if (budget >= 1 && budget <= 100000) {
                mp6_max_ticks = (int)mp6_tick_count + budget;
                armedSeq = seq;
                armedTick = mp6_tick_count;
                exitRequested = 0;
                printf("[WIDE-QA] budget armed scene=%s ticks=%d start=%ld\\n", target, budget, mp6_tick_count);
            }
        }
        /* Exercise the normal overlay exit path only when explicitly requested.
         * This is not a completed-round signal or part of the release build. */
        if (getenv("MP6_WS_QA_REQUEST_EXIT") && armedSeq && !exitRequested &&
            target && mp6_event_matches("ovl.start", target) &&
            mp6_tick_count >= mp6_max_ticks - 200) {
            exitRequested = 1;
            omSysExitReq = TRUE;
            mp6_event_post("qa.scene.exit", 0, "requested");
        }
    }
    mp6_qa_scene_export_tick();
    if (mp6_tick_count % 300 == 0) {
        static unsigned long dumpedSeq;
        if (Hu3DData && omcurovl != DLL_NONE && omcurovl != DLL_bootdll &&
            getenv("MP6_WS_HSF_DIAG") && armedSeq && dumpedSeq != armedSeq &&
            mp6_tick_count - armedTick >= 500) {
            dumpedSeq = armedSeq;
            for (int model = 0; model < HU3D_MODEL_MAX; ++model) {
                if (Hu3DData[model].hsf == NULL ||
                    (Hu3DData[model].attr & (HU3D_ATTR_HOOKFUNC | HU3D_ATTR_CAMERA))) continue;
                char label[48];
                snprintf(label, sizeof(label), "wide-ovl%d-model%d", omcurovl, model);
                mp6_widescreen_debug_dump_model(model, label);
                mp6_widescreen_debug_dump_world_bbox(model, label);
                /* Render-only isolation after setup; never part of production. */
                const char *hidden = getenv("MP6_WS_QA_HIDE_MESH");
                if (hidden) {
                    HSF_DATA *hsf = Hu3DData[model].hsf;
                    char hiddenList[1024];
                    snprintf(hiddenList, sizeof(hiddenList), ",%s,", hidden);
                    for (int n = 0; n < hsf->objectNum; ++n) {
                        HSF_OBJECT *o = &hsf->object[n];
                        char token[160];
                        snprintf(token, sizeof(token), ",%s,", o->name ? o->name : "");
                        if (o->type == HSF_OBJ_MESH && o->name && strstr(hiddenList, token)) {
                            o->flags |= HSF_MATERIAL_DISPOFF;
                            printf("[WIDE-ISOLATE] hidden %s/%s\\n", label, o->name);
                        }
                    }
                }
                const char *wanted = getenv("MP6_WS_QA_MESH");
                HSF_DATA *hsf = Hu3DData[model].hsf;
                if (wanted) printf("[WIDE-OWNER] %s attr=%08x camera=%u motion=%d cenv=%d\\n",
                    label, Hu3DData[model].attr, Hu3DData[model].cameraBit,
                    Hu3DData[model].motId, hsf->cenvNum);
                for (int n = 0; wanted && n < hsf->objectNum; ++n) {
                    HSF_OBJECT *o = &hsf->object[n];
                    char meshToken[160];
                    snprintf(meshToken, sizeof(meshToken), ",%s,", o->name ? o->name : "");
                    char wantedList[1024];
                    snprintf(wantedList, sizeof(wantedList), ",%s,", wanted);
                    if (o->type != HSF_OBJ_MESH || !o->name || !strstr(wantedList, meshToken) ||
                        !o->mesh.vertex || !o->mesh.face) continue;
                    mp6_widescreen_debug_dump_verts(model, o->name, label);
                    Mtx liveMtx;
                    Hu3DModelObjMtxGet(model, o->name, liveMtx);
                    printf("[WIDE-LIVE] %s/%s", label, o->name);
                    for (int row=0; row<3; ++row)
                        for (int col=0; col<4; ++col) printf(" %.5f", liveMtx[row][col]);
                    printf("\\n");
                    printf("[WIDE-OBJECT] %s/%s flags=%08x const=%08x cenv=%d\\n",
                        label,o->name,o->flags,o->constData ? ((HSF_CONSTDATA *)o->constData)->attr : 0,o->mesh.cenvNum);
                    HuVecF *v = o->mesh.vertex->data;
                    HuVec2f *uv = o->mesh.st ? o->mesh.st->data : NULL;
                    HSF_FACE *faces = o->mesh.face->data;
                    for (int f = 0; f < o->mesh.face->count && f < 1000; ++f) {
                        HSF_FACE *face = &faces[f];
                        int type = face->typeSrc & HSF_FACE_MASK;
                        int count = type == HSF_FACE_QUAD ? 4 : 3;
                        printf("[WIDE-MESH] %s/%s face=%d type=%d mat=%d", label, o->name, f, type, face->mat);
                        for (int c = 0; c < count; ++c) {
                            HSF_FACE_INDEX id = face->index[c];
                            if (id.vertex < 0 || id.vertex >= o->mesh.vertex->count) continue;
                            printf(" c%d=v%d(%.4f,%.4f,%.4f)", c,
                                id.vertex,v[id.vertex].x,v[id.vertex].y,v[id.vertex].z);
                            if (uv && id.st >= 0 && id.st < o->mesh.st->count)
                                printf("/uv%d(%.4f,%.4f)", id.st,uv[id.st].x,uv[id.st].y);
                            if (o->mesh.normal && o->mesh.normal->data && id.normal >= 0 && id.normal < o->mesh.normal->count) {
                                HuVecF normal;
                                if (hsf->cenvNum == 0) {
                                    const HSF_S8VEC *packed = o->mesh.normal->data;
                                    normal.x = packed[id.normal].x / 64.0f;
                                    normal.y = packed[id.normal].y / 64.0f;
                                    normal.z = packed[id.normal].z / 64.0f;
                                } else {
                                    normal = ((HuVecF *)o->mesh.normal->data)[id.normal];
                                }
                                printf("/n%d-%s(%.4f,%.4f,%.4f)", id.normal,
                                    hsf->cenvNum == 0 ? "s8" : "f32",normal.x,normal.y,normal.z);
                            }
                            if (o->mesh.color && o->mesh.color->data && id.color >= 0 && id.color < o->mesh.color->count) {
                                GXColor color = ((GXColor *)o->mesh.color->data)[id.color];
                                printf("/rgba(%u,%u,%u,%u)", color.r,color.g,color.b,color.a);
                            }
                        }
                        printf("\\n");
                    }
                }
            }
        }
        for (i = 0; i < HU3D_CAM_MAX; ++i) {
            HU3D_CAMERA *c = &Hu3DCamera[i];
            if (!cameras[i].live || c->fov == -1) continue;
            printf("[WIDE-QA] tick=%ld camera=%d fov=%.4f aspect=%.4f vp=%.2f,%.2f,%.2f,%.2f sc=%u,%u,%u,%u\\n",
                   mp6_tick_count, i, c->fov, c->aspect,
                   c->viewportX, c->viewportY, c->viewportW, c->viewportH,
                   c->scissorX, c->scissorY, c->scissorW, c->scissorH);
            if (getenv("MP6_WS_QA_MESH"))
                printf("[WIDE-CAMERA] camera=%d pos=%.5f,%.5f,%.5f target=%.5f,%.5f,%.5f up=%.5f,%.5f,%.5f clip=%.5f,%.5f\\n",
                       i,c->pos.x,c->pos.y,c->pos.z,c->target.x,c->target.y,c->target.z,
                       c->up.x,c->up.y,c->up.z,c->near,c->far);
        }
    }''')
                flags = [*flags, '-I'+str(ROOT/'tests/integration')]
            obj = objects/name
            if changed is not None:
                source = str(output/(name[:-2]+'_probe.c'))
                Path(source).write_text(changed, encoding='utf-8')
                obj = output/'obj'/name
            result.append((source,flags,str(obj),flavor))
        return result

    build.configure_windows, build.collect_units = configure_qa, collect_qa
    sys.argv = ['tools/build.py','--configuration','release','-j2']
    code = build.main()
    if code == 0:
        exe = output/'release/mp6native.exe'
        (output/'ready.json').write_text(json.dumps(dict(
            exe=hashlib.sha256(exe.read_bytes()).hexdigest(),
            builder=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
