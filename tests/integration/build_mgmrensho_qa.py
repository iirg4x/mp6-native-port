"""Observe real Endurance Alley input and lifecycle in an isolated QA build."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.build_overlay_probe import build_probe
from tests.integration.build_mgmfree_qa import replace_once, instrument_bridge


def main_source(source):
    source = '#include "mp6_events.h"\n#include "mp6_boot.h"\n#include "game/omovl.h"\n' + source
    source = replace_once(source, 's32 _prolog(void)\n{',
        's32 _prolog(void)\n{\n    mp6_event_post("qa.endurance.enter", 0, "prolog");')
    source = replace_once(source, 'void _epilog(void) {',
        'void _epilog(void) {\n    mp6_event_post("qa.endurance.epilog", 0, "done");')
    source = replace_once(source, 'void fn_1_EE24(void)\n{',
        'void fn_1_EE24(void)\n{\n    mp6_event_post("qa.endurance.back", 0, "quit");')
    source = replace_once(source, '    switch (*(s16 *)&MgModeSubMode) {',
        '    char phase[16];\n    snprintf(phase, sizeof(phase), "%d", MgModeSubMode);\n'
        '    mp6_event_post("qa.endurance.state", MgModeSubMode, phase);\n'
        '    switch (*(s16 *)&MgModeSubMode) {')
    source = replace_once(source, '    _SetFlag(FLAG_INST_MG_MODE);',
        '    if (overlayHistory->evtno == 0 && getenv("MP6_QA_ENDURANCE_ROUND")) {\n'
        '        MgModeWork[0] = (u8)MgNoGet(DLL_m602dll);\n'
        '        mp6_event_post("qa.endurance.fixture", MgModeWork[0], "m602dll");\n'
        '    }\n    _SetFlag(FLAG_INST_MG_MODE);')
    source = replace_once(source, '    case 8:\n        fn_1_D2BC();',
        '    case 8:\n'
        '        mp6_event_post("qa.endurance.result", MgModePlayNum, "returned");\n'
        '        if (getenv("MP6_QA_ENDURANCE_ROUND"))\n'
        '            mp6_max_ticks = (int)mp6_tick_count + 900;\n'
        '        fn_1_D2BC();')
    return source


def messages_source(source):
    source = '#include "mp6_events.h"\nstatic int qaRoundVisits;\n' + source
    for number, label in ((6, 'round'), (2, 'quit')):
        before = (f'            HuWinMesSet(lbl_1_bss_B8.windowId, MESSNUM(MESS_MGM_RENSHOU, {number}));\n'
                  '            HuWinMesWait(lbl_1_bss_B8.windowId);\n'
                  '            result = HuWinChoiceGet(lbl_1_bss_B8.windowId, -1);')
        after = before.replace('            result =',
            f'            mp6_event_post("qa.endurance.choice", {number}, "{label}");\n            result =')
        if label == 'round':
            after = after.replace('            result =',
                '            if (++qaRoundVisits > 1)\n'
                '                mp6_event_post("qa.endurance.reentry", qaRoundVisits, "ready");\n'
                '            result =')
        source = replace_once(source, before, after)
    return source


def bridge_source(source):
    # Both menus use the same forced-load seam, in-memory unlock fixture and
    # return/reload event protocol. Only their event/env namespace differs.
    return instrument_bridge(source).replace('qa.mgmfree', 'qa.endurance').replace(
        'MP6_QA_MGMFREE', 'MP6_QA_ENDURANCE')


def course_source(source):
    return replace_once(source,
        '    Hu3DModelCameraSet(lbl_1_bss_FC->mdlId[modelIndex], 1U);',
        '''    if (getenv("MP6_QA_ENDURANCE_TEXTURES")) {
        HSF_DATA *hsf = Hu3DData[lbl_1_bss_FC->mdlId[modelIndex]].hsf;
        for (int i = 0; i < hsf->attributeNum; ++i) {
            HSF_ATTRIBUTE *attr = &hsf->attribute[i];
            HSF_BITMAP *bitmap = attr->bitmap;
            if (!bitmap) continue;
            OSReport("[ENDURANCE-TEX] section=%d attr=%s flags=%x lod=%u bitmap=%s fmt=%d pix=%d size=%dx%d lod=%u\\n",
                modelIndex, attr->name, attr->flag, attr->maxLod, bitmap->name,
                bitmap->dataFmt, bitmap->pixSize, bitmap->sizeX, bitmap->sizeY, bitmap->maxLod);
        }
    }
    Hu3DModelCameraSet(lbl_1_bss_FC->mdlId[modelIndex], 1U);''')


if __name__ == '__main__':
    raise SystemExit(build_probe('mgmrensho-qa', {
        ('mgmrenshodll', 'main.c'): main_source,
        ('mgmrenshodll', 'messages.c'): messages_source,
        ('mgmrenshodll', 'course.c'): course_source,
        ('os', 'dll_bridge.c'): bridge_source,
    }))
