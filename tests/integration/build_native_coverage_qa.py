"""Basic native entry/exit probe; visual/AO/widescreen auditing is separate."""
from tests.integration.build_overlay_probe import build_probe


PREPARE = r'''
#include "game/board/main.h"
#include "game/flag.h"
static void qa_prepare_recovered_board(s16 overlay)
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
    mbSavePartyInit(FALSE, TRUE, 0, 20, 0, 0, 0, 0);
    GwSystem.boardNo = board;
    _ClearFlag(FLAG_BOARD_SAVEINIT);
    for (int p = 0; p < GW_PLAYER_MAX; ++p) {
        GwPlayer[p].charNo = GwPlayerConf[p].charNo;
        GwPlayer[p].padNo = GwPlayerConf[p].padNo;
        GwPlayer[p].comF = TRUE;
        GwPlayer[p].comDif = 1;
        GwPlayer[p].team = 0;
    }
}
'''


def instrument_bridge(source):
    source = PREPARE + source
    before = '    mp6_dll_bridge_selftest_teams(g_forcedTestOverlay);'
    assert source.count(before) == 1
    source = source.replace(before, '    qa_prepare_recovered_board(g_forcedTestOverlay);\n' + before)
    source = source.replace('mp6_max_ticks = (int)mp6_tick_count + 600;',
                            'mp6_max_ticks = (int)mp6_tick_count + 6000;')
    before = '    if (checked) return;'
    assert source.count(before) == 1
    source = source.replace(before, '''    static long qaEntered;
    if (checked && omcurovl == g_forcedTestOverlay) {
        if (!qaEntered) {
            qaEntered = mp6_tick_count;
            mp6_max_ticks = (int)mp6_tick_count + 2000;
            mp6_ticks_unlimited = 0;
        }
        if (mp6_tick_count == qaEntered + 1700) {
            omSysExitReq = TRUE;
            mp6_event_post("qa.coverage.exit", 0, "requested");
        }
    }
''' + before)
    return source


if __name__ == '__main__':
    raise SystemExit(build_probe('native-coverage-qa', {
        ('os', 'dll_bridge.c'): instrument_bridge,
    }, jobs=1))
