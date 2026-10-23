"""Run Pixel Perfect's real state consumers on native-width records."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M636NativeAbi(unittest.TestCase):
    def test_function_extractor_does_not_treat_a_call_as_a_definition(self):
        source = '''
void caller(void) { if (target(3)) { abort(); } }
int other(void) {
    if (target(4) != 0) { return 1; }
    return 0;
}
static int target(int value) { return value; }
'''
        self.assertEqual(function_definition_body(source, 'target'),
                         'static int target(int value) { return value; }')

    def test_shared_state_camera_players_pattern_and_landing(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            sources = {name: (patched / f'src/REL/m636dll/{name}.c').read_text(encoding='utf-8')
                       for name in ('camera', 'pattern', 'sequence', 'ai', 'main')}
            # No owner may bypass the native layout. Runtime tests below verify
            # actual field effects, not just the presence of replacement text.
            for name, source in sources.items():
                self.assertNotRegex(source, r'\([us]8\s*\*\)\s*\(?\s*&lbl_1_bss_0', name)
            self.assertEqual(sources['main'].count('MgTimerCreate(0)'), 1)
            source = r'''
#include <assert.h>
#include <string.h>
#include "REL/m636dll.h"
#include "game/mgdata.h"
M636WorkingData lbl_1_bss_0;
MGDATA MgDataTbl[4] = {{.type=MG_TYPE_4P}, {.type=MG_TYPE_1VS3}, {.type=MG_TYPE_2VS2}, {.type=MG_TYPE_KETTOU}};
GW_PLAYER_CONF GwPlayerConf[GW_PLAYER_MAX];
s32 MgNoGet(s16 overlay) { return overlay >= 0 && overlay < 4 ? overlay : -1; }
u8 lbl_1_data_378[768];
f32 lbl_1_data_270[2][2] = {{850,400},{-50,400}};
static MGACTOR actors[4];
static MGPLAYER players[4];
static MGTIMER timer;
static int panCount, modelKills, motionSets;
static s16 lastMotionModel, lastMotion;
void OSReport(const char *format, ...) {}
HU3D_MODELID Hu3DModelCameraCreate(HU3D_MOTIONID motion, u16 camera) {
    assert(motion == 55 && camera == HU3D_CAM0); return 70;
}
void Hu3DCameraMotionStart(HU3D_MODELID model, u16 camera) {
    assert(model == 70 && camera == HU3D_CAM0);
}
float Hu3DMotionMaxTimeGet(HU3D_MODELID model) { assert(model == 70); return 180; }
float Hu3DMotionTimeGet(HU3D_MODELID model) { assert(model == 70); return 27; }
void Hu3DModelKill(HU3D_MODELID model) { assert(model == 31); ++modelKills; }
BOOL Hu3DMotionEndCheck(HU3D_MODELID model) { return FALSE; }
void Hu3D3Dto2D(HuVecF *position, s16 camera, HuVecF *screen) {
    assert(camera == 1);
    int index = panCount % 4;
    assert(memcmp(position, &actors[index].pos, sizeof(*position)) == 0);
    *screen = *position;
}
void CharModelVoicePanSet(s16 character, s16 pan) {
    int index = panCount++ % 4;
    assert(character == 20 + index && pan == 32 + index);
}
void Hu3DMotionTimeSet(HU3D_MODELID model, float time) { __builtin_trap(); }
void Hu3DModelAttrReset(HU3D_MODELID model, u32 attributes) { __builtin_trap(); }
s32 HuAudFXPlay(s32 sound) { __builtin_trap(); }
u32 MgSeqModeNext(void) { __builtin_trap(); }
void MgPlayerAttrSet(MGPLAYER *player, u16 attributes) {
    assert(player >= players && player < players + 4);
    assert(attributes == MGPLAYER_ATTR_COMSTK);
}
s16 MgSeqGameMesIdGet(void) { return 1; }
u8 GameMesStatGet(GAMEMESID id) { return 0; }
s32 HuAudBGMPlay(s16 music) { __builtin_trap(); }
u16 MgPlayerVibAttrCheck(MGPLAYER *player, u16 flags) {
    assert(player == &players[3] && flags == 0x200); return flags;
}
void MgActorPosGet(MGACTOR *actor, HuVecF *position) { *position = actor->pos; }
BOOL MgTimerDoneCheck(MGTIMER *value) { assert(value == &timer); return FALSE; }
HU3D_MOTIONID Hu3DMotionIDGet(HU3D_MODELID model) { return 11; }
void Hu3DMotionSet(HU3D_MODELID model, HU3D_MOTIONID motion) {
    lastMotionModel = model; lastMotion = motion; ++motionSets;
}
s32 frandmod(s32 n) { assert(n > 0); return n - 1; }
'''
            bridge = (ROOT / 'src/os/dll_bridge.c').read_text(encoding='utf-8')
            source += function_definition_body(bridge, 'mp6_dll_bridge_selftest_teams') + '\n'
            for name, functions in {
                'camera': ('fn_1_6360', 'fn_1_6398'),
                'pattern': ('fn_1_3834', 'fn_1_3984', 'fn_1_39FC', 'fn_1_3A84', 'fn_1_3AE0', 'fn_1_2F10'),
                'sequence': ('fn_1_1EC', 'fn_1_2F4', 'fn_1_494'),
                'ai': ('fn_1_3D80',),
            }.items():
                source += '\n'.join(re.findall(r'^#define PIXEL_\w+(?:[^\n\\]|\\\n)*$', sources[name], re.M)) + '\n'
                source += '\n'.join(function_definition_body(sources[name], function)
                                    for function in functions) + '\n'
            source += r'''
int main(void) {
    for (int i=0; i<4; ++i) GwPlayerConf[i].grpNo = 3-i;
    mp6_dll_bridge_selftest_teams(-1); mp6_dll_bridge_selftest_teams(0);
    for (int i=0; i<4; ++i) assert(GwPlayerConf[i].grpNo == 3-i);
    mp6_dll_bridge_selftest_teams(1);
    for (int i=0; i<4; ++i) assert(GwPlayerConf[i].grpNo == (i != 0));
    mp6_dll_bridge_selftest_teams(2);
    for (int i=0; i<4; ++i) assert(GwPlayerConf[i].grpNo == i/2);
    mp6_dll_bridge_selftest_teams(3); /* a duel: two participants, inactive slots apart */
    for (int i=0; i<4; ++i) assert(GwPlayerConf[i].grpNo == (i < 2 ? i : 2));
    _Static_assert(sizeof(void *) == 8, "exercise native 64-bit ABI");
    _Static_assert(__builtin_types_compatible_p(MGACTOR_CORRECT_HOOK, __typeof__(&fn_1_2F10)),
                   "landing callback ABI");
    memset(&lbl_1_bss_0, 0x5A, sizeof(lbl_1_bss_0));
    lbl_1_bss_0.timer = &timer;
    for (int i=0; i<4; ++i) {
        players[i].actor = &actors[i];
        actors[i].pos = (Vec){160 + 5*i, 100 + i, -50 + i};
        lbl_1_bss_0.players[i] = &players[i];
        lbl_1_bss_0.charNo[i] = 20 + i;
        lbl_1_bss_0.teamSide[i] = i / 2;
    }
    lbl_1_bss_0.cameraMotions[1] = 55;
    lbl_1_bss_0.cameraModel = 31;
    M636WorkingData expected = lbl_1_bss_0;
    expected.cameraModel = 70; expected.cameraFrame = 0; expected.cameraLastFrame = 180;
    fn_1_6398(1);
    assert(modelKills == 1 && memcmp(&expected, &lbl_1_bss_0, sizeof(expected)) == 0);
    expected.cameraFrame = 27;
    fn_1_6360(NULL);
    assert(memcmp(&expected, &lbl_1_bss_0, sizeof(expected)) == 0);
    fn_1_1EC();
    lbl_1_bss_0.selectedPicture = 2;
    fn_1_2F4(0, 1);
    assert(panCount == 8);
    expected = lbl_1_bss_0;
    expected.cpuActive = 0; expected.roundIndex = 0;
    expected.points[0] = expected.points[1] = 0;
    expected.sequenceMode = 0; expected.tick = 0;
    for (int i=0; i<4; ++i) { expected.cpuMode[i] = 1; expected.targetCell[i] = -1; }
    fn_1_494(0, 0);
    assert(memcmp(&expected, &lbl_1_bss_0, sizeof(expected)) == 0);
    for (int i=0; i<768; ++i) lbl_1_data_378[i] = (i * 7 + i/256) % 251;
    for (int picture=0; picture<3; ++picture) for (int cell=0; cell<16; ++cell) {
        expected = lbl_1_bss_0;
        for (int y=0; y<4; ++y) for (int x=0; x<4; ++x)
            expected.targetPixels[y*4+x] = lbl_1_data_378[picture*256 + (cell/4*4+y)*16 + cell%4*4+x];
        fn_1_3834(picture, cell);
        assert(memcmp(&expected, &lbl_1_bss_0, sizeof(expected)) == 0);
    }
    memset(lbl_1_bss_0.targetPixels, 0, 16);
    memset(lbl_1_bss_0.teamPixels, 0, 32);
    assert(fn_1_3984(0) == 1 && fn_1_39FC(1) == 0 && fn_1_3D80(3) == -1);
    lbl_1_bss_0.teamPixels[0][15] = 1;
    lbl_1_bss_0.teamPixels[1][2] = 1;
    assert(fn_1_39FC(0) == 1 && fn_1_39FC(1) == 1);
    assert(fn_1_3D80(1) == 15 && fn_1_3D80(3) == 2);
    assert(fn_1_3A84(0, 15) == 0 && fn_1_3A84(1, 15) == 1);
    actors[3].colGroundAttr = 1;
    actors[3].pos = (Vec){550, 0, -300}; // Right team, column 2, row 0.
    lbl_1_bss_0.panelModels[1][2] = 91;
    lbl_1_bss_0.panelMotions[2] = 12; lbl_1_bss_0.panelMotions[3] = 13;
    expected = lbl_1_bss_0;
    expected.landingCounter[3] = 90; expected.teamPixels[1][2] = 0;
    fn_1_2F10(&actors[3], 3);
    assert(motionSets == 1 && lastMotionModel == 91 && lastMotion == 13);
    assert(memcmp(&expected, &lbl_1_bss_0, sizeof(expected)) == 0);
    return 0;
}
'''
            subject, executable = directory / 'm636.c', directory / 'm636.exe'
            subject.write_text(source, encoding='utf-8')
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-O1', '-UNDEBUG', '-fsanitize=undefined',
                '-fno-sanitize-recover=all', str(subject), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
