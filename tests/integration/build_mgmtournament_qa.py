"""Build an isolated native Tournament menu/duel QA executable.

The source transforms in this module are applied only by ``build_probe`` to
the generated QA executable.  They observe the real Tournament state machine,
make one deterministic Duel available when requested, and observe its natural
return.  The checked-out decompilation and normal binary are never modified.
"""
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tests.integration.build_overlay_probe import build_probe
from tests.integration.build_mgmfree_qa import instrument_bridge, replace_once


def instrument_tournament_characters(source):
    """Observe the actual Tournament overlay lifetime."""
    source = '#include "mp6_events.h"\n#include <stdlib.h>\n' + source
    source = replace_once(
        source,
        's32 _prolog(void)\n{',
        's32 _prolog(void)\n{\n'
        '    mp6_event_post("qa.mgmtournament.enter", 0, "menu");',
    )
    source = replace_once(
        source,
        'void _epilog(void)\n{',
        'void _epilog(void)\n{\n'
        '    mp6_event_post("qa.mgmtournament.epilog", 0, "done");',
    )
    return replace_once(
        source,
        '    fn_1_AA04();\n    playerIndex = 0;',
        '    fn_1_AA04();\n'
        '    /* Deterministic starting bracket only; native play decides results. */\n'
        '    if (getenv("MP6_QA_MGMTOURNAMENT_GAME"))\n'
        '        for (playerIndex = 0; playerIndex < 4; ++playerIndex)\n'
        '            MgModeWorkInt[playerIndex] = playerIndex;\n'
        '    playerIndex = 0;',
    )


def instrument_tournament_scene(source):
    """Observe state/route and select one recovered Duel for QA only."""
    source = (
        '#include "mp6_events.h"\n'
        '#include "mp6_boot.h"\n'
        '#include "game/omovl.h"\n'
        '#include <stdio.h>\n'
        '#include <stdlib.h>\n'
        'static s16 qaTournamentLastState = -32768;\n' + source
    )
    source = replace_once(
        source,
        '    _SetFlag(FLAG_INST_MG_MODE);',
        '''    if (getenv("MP6_QA_MGMTOURNAMENT_GAME")) {
        /* m662 is the recovered Trick or Tree Duel; keep this fixture QA-only. */
        MgModeWork[0] = (u8)MgNoGet(DLL_m662dll);
        GWMgUnlockSet(MgModeWork[0] + TOURNAMENT_UNLOCK_GAME_BASE);
        {
            s16 qaPlayer;
            /* Keep the first recovered pair deterministic: player zero is the
             * scripted human and the opponent is native CPU input. */
            for (qaPlayer = 0; qaPlayer < 4; qaPlayer++) {
                GwPlayerConf[qaPlayer].type = (qaPlayer == 0) ? 0 : 1;
                GwPlayerConf[qaPlayer].padNo = qaPlayer;
            }
        }
        mp6_event_post("qa.mgmtournament.fixture", MgModeWork[0], "m662dll");
    }
    _SetFlag(FLAG_INST_MG_MODE);''',
    )
    source = replace_once(
        source,
        '    history = omOvlHisGet(0);\n    switch (MgModeSubMode) {',
        '''    history = omOvlHisGet(0);
    if (qaTournamentLastState != MgModeSubMode) {
        char qaState[16];
        snprintf(qaState, sizeof(qaState), "%d", MgModeSubMode);
        mp6_event_post("qa.mgmtournament.state", MgModeSubMode, qaState);
        qaTournamentLastState = MgModeSubMode;
    }
    switch (MgModeSubMode) {''',
    )
    source = replace_once(
        source,
        '        GwSystem.mgInstDispF = 1;\n',
        '        GwSystem.mgInstDispF = 1;\n'
        '        mp6_event_post("qa.mgmtournament.route", 1, "inst");\n',
    )
    return replace_once(
        source,
        '            MgModeWorkInt[index + 8] = coinBonus;\n',
        '            MgModeWorkInt[index + 8] = coinBonus;\n'
        '            if (getenv("MP6_QA_MGMTOURNAMENT_GAME"))\n'
        '                mp6_event_post("qa.mgmtournament.coin", coinBonus, "mgCoinBonus");\n',
    )


def instrument_tournament_bracket(source):
    source = '#include "mp6_widescreen.h"\n#include <stdlib.h>\n#include <stdio.h>\n' + source
    return replace_once(
        source,
        '        Hu3DModelCameraSet(lbl_1_bss_DC->mdlId[modelIndex], 1);',
        '''        if (getenv("MP6_WS_HSF_DIAG")) {
            char label[32];
            snprintf(label, sizeof(label), "tournament-layer-%d", modelIndex);
            mp6_widescreen_debug_dump_model(lbl_1_bss_DC->mdlId[modelIndex], label);
        }
        Hu3DModelCameraSet(lbl_1_bss_DC->mdlId[modelIndex], 1);''',
    )


def instrument_tournament_window(source):
    """Record the real menu result without changing prompt semantics."""
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(
        source,
        '            HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '            result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
        '            HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '            mp6_event_post("qa.mgmtournament.prompt", 5, "quit");\n'
        '            result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
    )
    source = replace_once(
        source,
        '                HuWinMesSet(lbl_1_bss_C8.windowId, MESSNUM(MESS_MGM_TOURNAMENT, 14));\n'
        '                HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '                result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
        '                HuWinMesSet(lbl_1_bss_C8.windowId, MESSNUM(MESS_MGM_TOURNAMENT, 14));\n'
        '                HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '                mp6_event_post("qa.mgmtournament.prompt", 14, "round");\n'
        '                result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
    )
    source = replace_once(
        source,
        '                HuWinMesSet(lbl_1_bss_C8.windowId, MESSNUM(MESS_MGM_TOURNAMENT, 4));\n'
        '                HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '                result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
        '                HuWinMesSet(lbl_1_bss_C8.windowId, MESSNUM(MESS_MGM_TOURNAMENT, 4));\n'
        '                HuWinMesWait(lbl_1_bss_C8.windowId);\n'
        '                mp6_event_post("qa.mgmtournament.prompt", 4, "round");\n'
        '                result = HuWinChoiceGet(lbl_1_bss_C8.windowId, -1);',
    )
    # This anchor includes the fn_1_2128 switch tail.  Reinsert the complete
    # original anchor after the event block so no source brace is consumed.
    anchor = (
        '            break;\n'
        '        }\n'
        '    }\n'
        '    HuWinExClose(lbl_1_bss_C8.windowId);\n'
        '    HuPrcSleep(10);\n'
        '    return result;'
    )
    return replace_once(
        source,
        anchor,
        '''            break;
        }
    }
    /* The source maps result 1 to instructions, result 2 to the
     * simulated/advanced round, and zero to direct play. */
    if (result < 0) {
        mp6_event_post("qa.mgmtournament.choice", result, "quit");
        mp6_event_post("qa.mgmtournament.back", result, "quit");
    } else if (result == 1) {
        mp6_event_post("qa.mgmtournament.choice", result, "inst");
    } else if (result == 2) {
        mp6_event_post("qa.mgmtournament.choice", result, "simulate");
    } else {
        mp6_event_post("qa.mgmtournament.choice", result, "play");
    }
    HuWinExClose(lbl_1_bss_C8.windowId);
    HuPrcSleep(10);
    return result;''',
    )


def instrument_instruction(source):
    """Observe the real instruction overlay selected by Tournament."""
    source = '#include "mp6_events.h"\n' + source
    source = replace_once(
        source,
        '    lbl_1_bss_36 = GwSystem.mgNo;\n',
        '    lbl_1_bss_36 = GwSystem.mgNo;\n'
        '    mp6_event_post("qa.mgmtournament.inst", lbl_1_bss_36, "entered");\n',
    )
    source = replace_once(
        source,
        '    flag_2 = 0;\n'
        '    if (lbl_1_bss_20->type != MG_TYPE_KETTOU) {',
        '    flag_2 = 0;\n'
        '    mp6_event_post("qa.mgmtournament.inst", lbl_1_bss_36, "ready");\n'
        '    if (lbl_1_bss_20->type != MG_TYPE_KETTOU) {',
    )
    return replace_once(
        source,
        '            if (lbl_1_bss_24 & PAD_BUTTON_START) {\n'
        '                HuAudFXPlay(MSM_SE_CMN_03);\n'
        '                GWMgPlayNumAdd(1);',
        '            if (lbl_1_bss_24 & PAD_BUTTON_START) {\n'
        '                HuAudFXPlay(MSM_SE_CMN_03);\n'
        '                GWMgPlayNumAdd(1);\n'
        '                mp6_event_post("qa.mgmtournament.inst_start", 0, "game");',
    )


def bridge_source(source):
    """Reuse the shared isolated reload/return protocol under Tournament names."""
    return instrument_bridge(source).replace(
        'qa.mgmfree', 'qa.mgmtournament').replace(
        'MP6_QA_MGMFREE', 'MP6_QA_MGMTOURNAMENT')


TOURNAMENT_INSTRUMENTERS = {
    ('mgmtournamentdll', 'bracket.c'): instrument_tournament_bracket,
    ('mgmtournamentdll', 'characters.c'): instrument_tournament_characters,
    ('mgmtournamentdll', 'scene.c'): instrument_tournament_scene,
    ('mgmtournamentdll', 'window.c'): instrument_tournament_window,
    # Keep the upstream source-directory spelling: build.collect_units()
    # resolves this recovered overlay as src/REL/instDll/setup.c.
    ('instDll', 'setup.c'): instrument_instruction,
    ('os', 'dll_bridge.c'): bridge_source,
}


def main():
    return build_probe('mgmtournament-qa', TOURNAMENT_INSTRUMENTERS)


if __name__ == '__main__':
    raise SystemExit(main())
