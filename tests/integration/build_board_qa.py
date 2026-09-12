"""Build an isolated board-cycle diagnostic executable, reusing release objects.

Generated probes log real turn, dice, capsule, and W01 geometry state. Optional
inventory/starting-space fixtures are explicit MP6_QA_* environment switches.
No fixture is compiled into normal releases and no dependency source is edited.
"""
import os
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
os.environ.setdefault('MP6_DISC_ROOT', 'build/disc-cache/orig/GP6E01')
os.environ.setdefault('MP6_DECOMP_INC_DATA', 'build/disc-cache/split/include')
sys.path.insert(0, str(ROOT / 'tools'))
import build

OUTPUT = ROOT / 'build/board-qa'
OUTPUT.mkdir(exist_ok=True)
READY = OUTPUT / 'build-ready.json'
READY.unlink(missing_ok=True)
configure_original = build.configure_windows
collect_original = build.collect_units

PRELUDE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "game/gamework.h"
#include "game/board/player.h"
#include "game/board/camera.h"
#include "mp6_boot.h"
#include "mp6_events.h"
#include "mp6_frame_dump.h"
extern int qa_last5_active;
static void qa_state(const char *stage, int p, int value) {
    mp6_frame_dump_trigger(stage);
    HuVecF pos = {0}, eye = {0}, center = {0};
    if (p >= 0 && p < 4) mbPlayerPosGet(p, &pos);
    mbCameraEyeGet(&eye);
    mbCameraCenterGet(&center);
    printf("[BOARD-QA] %s tick=%ld turn=%d time=%d p=%d value=%d mode=%d "
           "pos=(%.3f,%.3f,%.3f) eye=(%.3f,%.3f,%.3f) center=(%.3f,%.3f,%.3f)\n",
           stage, mp6_tick_count, GwSystem.turnNo, GwSystem.curTime, p, value,
           GwSystem.playerMode, pos.x,pos.y,pos.z,eye.x,eye.y,eye.z,center.x,center.y,center.z);
    for (int i=0;i<4;i++)
        printf("[BOARD-QA] player=%d space=%d move=%d coin=%d star=%d diceMode=%d diceNum=%d "
               "capsuleUse=%d inventory=%d,%d,%d\n",i,GwPlayer[i].masuId,GwPlayer[i].moveNum,
               GwPlayer[i].coin,GwPlayer[i].star,GwPlayer[i].diceMode,GwPlayer[i].diceNum,
               GwPlayer[i].capsuleUse,GwPlayer[i].capsule[0],GwPlayer[i].capsule[1],GwPlayer[i].capsule[2]);
    fflush(stdout);
}
'''


def replace_once(text, marker, replacement):
    assert text.count(marker) == 1, (marker, text.count(marker))
    return text.replace(marker, replacement)


def player_probe(text):
    # Observe accepted requests (after the same-motion early-out), without
    # altering playback. The low-level probe below records real wraps/end holds.
    text = '''static void qa_capsule_audit(int playerNo, const char *route);
int qa_motion_model[4] = {-1,-1,-1,-1};
int qa_motion_slot[4];
unsigned qa_motion_serial[4];
''' + text
    for api in ('mbPlayerMotionSet', 'mbPlayerMotionShiftSet'):
        fn = re.search(r'^void '+api+r'\([^;]+?\)\n\{.*?^\}', text, re.M | re.S).group()
        marker = '    playerWork[playerNo].motNo = motNo;'
        changed = replace_once(fn, marker, marker + r'''
    qa_motion_model[playerNo] = mbObjModelIDGet(mbPlayerObjIDGet(playerNo));
    qa_motion_slot[playerNo] = motNo;
    qa_motion_serial[playerNo]++;
    printf("[BOARD-QA] motion.start tick=%ld p=%d char=%d slot=%d attr=%08x serial=%u\n",
           mp6_tick_count,playerNo,GwPlayer[playerNo].charNo,motNo,attr,qa_motion_serial[playerNo]);
    if (motNo==9) mp6_frame_dump_trigger("motion.scared");
''')
        text = replace_once(text, fn, changed)
    marker = '    GwSystem.turnPlayerNo = playerNo;\n    mbPlayerPosReset(playerNo);'
    text = replace_once(text, marker, r'''
    GwSystem.turnPlayerNo = playerNo;
    static int route_given;
    const char *route = getenv("MP6_QA_ROUTE");
    static int night_setup;
    if (route && (!strcmp(route,"night-placement") || !strcmp(route,"night-last5")) && night_setup != 2) {
        if (!night_setup) {
            night_setup = 1;
            GwSystem.timeTurn = GwSystem.timeTurnMax-1;
            for (int qp=0;qp<4;qp++) GwPlayer[qp].comF = TRUE;
        } else if (GwSystem.curTime == 1) {
            night_setup = 2;
            GwPlayer[playerNo].comF = FALSE;
            GwPlayer[playerNo].padNo = 0;
            if (!strcmp(route,"night-placement")) {
                GwPlayer[playerNo].masuId = GwPlayer[playerNo].masuIdNext = 64;
                qa_state("FIXTURE.night-placement",playerNo,-1);
            } else {
                qa_state("FIXTURE.night-last5",playerNo,-1);
            }
        }
    }
    static int time_fixture;
    if (!time_fixture && route && !strcmp(route,"results")) {
        time_fixture = 1;
        /* Start the final round; let all four real turns and mbClose dispatch
         * the recovered results scene normally. This is not a full-match test. */
        GwSystem.turnNo = GwSystem.turnMax;
        for (int qp=0;qp<4;qp++) GwPlayer[qp].comF = TRUE;
        qa_state("FIXTURE.final-round",playerNo,-1);
    }
    if (!time_fixture && route && (!strcmp(route,"day-night") || !strcmp(route,"cpu-soak") ||
        !strcmp(route,"last5-transition"))) {
        time_fixture = 1;
        if (!strcmp(route,"day-night") || !strcmp(route,"last5-transition"))
            GwSystem.timeTurn = GwSystem.timeTurnMax-1;
        if (!strcmp(route,"last5-transition")) {
            GwSystem.turnNo = GwSystem.turnMax-5;
            qa_state("FIXTURE.last5-transition",playerNo,-1);
        }
        for (int qp=0;qp<4;qp++) GwPlayer[qp].comF = TRUE;
        printf("[BOARD-QA] FIXTURE day-night timeTurn=%d max=%d all-CPU\n",
               GwSystem.timeTurn,GwSystem.timeTurnMax);
    }
    if (!route_given && route && *route && !GwPlayer[playerNo].comF) {
        int attr = !strcmp(route,"bridge") ? 0x10 : !strcmp(route,"spring") ? 0x100 :
                   !strcmp(route,"slide") ? 0x40000 : 0;
        if (attr) {
            int space = mbMasuFind_MAttrIdGet(-1, attr);
            if (!strcmp(route,"bridge")) space = mbMasuMAttrFindLink(space,7);
            printf("[BOARD-QA] FIXTURE route=%s space=%d p=%d\n",route,space,playerNo);
            GwPlayer[playerNo].masuId = space;
            GwPlayer[playerNo].masuIdNext = space;
        }
        route_given = 1;
    }
    qa_state("turn.begin",playerNo,-1);
    mbPlayerPosReset(playerNo);''')
    marker = '    GwPlayer[playerNo].diceNum = 1;\nrepeat:'
    text = replace_once(text, marker, r'''
    static int capsule_given;
    const char *capsule = getenv("MP6_QA_CAPSULE");
    const char *route = getenv("MP6_QA_ROUTE");
    static int last5_given;
    static int star_given;
    static int duel_given;
    static int shop_given;
    static int snpc_given;
    if (route && !strncmp(route,"audit-",6)) qa_capsule_audit(playerNo,route);
    if (!snpc_given && route && !strcmp(route,"snpc-effects") && !GwPlayer[playerNo].comF) {
        extern void qa_snpc_effects(int playerNo);
        snpc_given = 1;
        qa_snpc_effects(playerNo);
        qa_state("snpc.complete",playerNo,-1);
        mp6_event_post("qa.complete",0,"done");
        mp6_max_ticks = (int)mp6_tick_count+120;
    }
    if (!shop_given && route && !strcmp(route,"shop") && !GwPlayer[playerNo].comF) {
        extern int mbev_Shop(int playerNo, int shopSpace);
        int qs;
        shop_given = 1;
        for (qs=1;qs<mbMasuNumGet();qs++) if (mbMasuTypeGet(qs)==9) break;
        if (qs>=mbMasuNumGet()) { printf("[BOARD-QA] FATAL missing authored shop space\n"); exit(3); }
        GwPlayer[playerNo].masuId = GwPlayer[playerNo].masuIdNext = qs;
        GwPlayer[playerNo].coin = 50;
        mbPlayerPosReset(playerNo);
        qa_state("FIXTURE.shop",playerNo,qs);
        mbev_Shop(playerNo,qs);
        qa_state("shop.end",playerNo,-1);
        mp6_event_post("qa.complete",0,"done");
        mp6_max_ticks = (int)mp6_tick_count+600;
    }
    if (!duel_given && route && !strcmp(route,"duel") && !GwPlayer[playerNo].comF) {
        duel_given = 1;
        qa_state("FIXTURE.duel",playerNo,-1);
        mbev_CapCallKettou(playerNo,GwPlayer[playerNo].masuId,TRUE);
        qa_state("duel.end",playerNo,-1);
    }
    if (!star_given && route && (!strcmp(route,"star") || !strcmp(route,"star-buy") ||
        !strcmp(route,"star-position") || !strcmp(route,"ztar") || !strcmp(route,"ztar-position")) && !GwPlayer[playerNo].comF) {
        extern void mbStarGetExec(int playerNo);
        star_given = 1;
        qa_state("star.begin",playerNo,-1);
        if (!strcmp(route,"star-buy")) {
            extern void mbev_StarMasu(int playerNo);
            int qs;
            for (qs=0;qs<mbMasuNumGet();qs++) if (mbMasuTypeGet(qs)==7) break;
            if (qs>=mbMasuNumGet()) { printf("[BOARD-QA] FATAL missing authored star space\n"); exit(3); }
            GwPlayer[playerNo].masuId = qs;
            GwPlayer[playerNo].coin = 50;
            mbPlayerPosReset(playerNo);
            qa_state("FIXTURE.star-purchase",playerNo,qs);
            mbev_StarMasu(playerNo);
        } else if (!strcmp(route,"ztar")) {
            extern void mbZtarGetExec(int playerNo);
            GwPlayer[playerNo].star = 2;
            qa_state("FIXTURE.ztar",playerNo,-1);
            mbZtarGetExec(playerNo);
        } else if (!strcmp(route,"star-position") || !strcmp(route,"ztar-position")) {
            extern void mbStarGetMain(int playerNo, HuVecF *pos, int num, BOOL focusF);
            extern void mbZtarGetMain(int playerNo, HuVecF *pos, int num, BOOL focusF);
            HuVecF pos;
            mbPlayerPosGet(playerNo,&pos);
            pos.y += 300.0f;
            if (!strcmp(route,"ztar-position")) {
                GwPlayer[playerNo].star = 2;
                qa_state("FIXTURE.ztar-position",playerNo,-1);
                mbZtarGetMain(playerNo,&pos,-1,TRUE);
            } else {
                qa_state("FIXTURE.star-position",playerNo,-1);
                mbStarGetMain(playerNo,&pos,1,TRUE);
            }
        } else {
            mbStarGetExec(playerNo);
        }
        qa_state("star.end",playerNo,-1);
        HuPrcSleep(1200);
        qa_state("star.after_wait",playerNo,-1);
    }
    if (!last5_given && route && (!strcmp(route,"last5") ||
        (!strcmp(route,"night-last5") && GwSystem.curTime == 1)) && !GwPlayer[playerNo].comF) {
        extern void mbev_Last5(void);
        last5_given = 1;
        mbev_Last5();
    }
    if (!capsule_given && capsule && atoi(capsule) >= 0 && !GwPlayer[playerNo].comF) {
        capsule_given = 1;
        int id = atoi(capsule);
        if (route && !strcmp(route,"land-orb")) {
            extern void mbCapMasuCapsuleSet(int masuId,int capsuleNo,int playerNo);
            mbCapMasuCapsuleSet(28,id,(playerNo+1)%4);
            qa_state("FIXTURE.land-orb",playerNo,id);
        } else if (id == 44) {
            qa_state("FIXTURE.donkey",playerNo,id);
            mbev_CapCallDonkey(playerNo);
        } else {
            mbPlayerCapsuleAdd(playerNo,id);
        }
        if (route && !strcmp(route,"warp-separated")) {
            int target = (playerNo+1)%4;
            GwPlayer[target].masuId = 18;
            mbPlayerPosReset(target);
        }
        qa_state("FIXTURE.capsule",playerNo,id);
    }
''' + marker)
    marker = '            GwPlayer[playerNo].capsuleUseNum++;'
    text = replace_once(text, marker, marker + '\n            qa_state("capsule.selected.return",playerNo,GwPlayer[playerNo].capsuleUse);')
    marker = '    GwPlayer[playerNo].diceMode = 0;\n    GwPlayer[playerNo].moveNum = result;'
    text = replace_once(text, marker, r'''
    if (getenv("MP6_QA_ROUTE") &&
        ((!strcmp(getenv("MP6_QA_ROUTE"),"land-orb") && !GwPlayer[playerNo].comF) ||
          !strncmp(getenv("MP6_QA_ROUTE"),"audit-pass",10) ||
          !strcmp(getenv("MP6_QA_ROUTE"),"audit-bullet"))) {
        result = 1;
        if (!strcmp(getenv("MP6_QA_ROUTE"),"audit-pass-long") ||
            !strcmp(getenv("MP6_QA_ROUTE"),"audit-bullet")) result=8;
        qa_state("FIXTURE.dice",playerNo,result);
    }
    qa_state("dice.result",playerNo,result);
    if (getenv("MP6_QA_HOLD_BAD_CAMERA")) {
        HuVecF eye,center;
        mbCameraEyeGet(&eye); mbCameraCenterGet(&center);
        if (!isfinite(eye.x) || fabsf(eye.x)>100000.0f || fabsf(center.y)>100000.0f) {
            printf("[BOARD-QA] PAUSED bad camera after real dice; renderer remains running\n");
            fflush(stdout);
            while (1) HuPrcVSleep();
        }
    }
''' + marker)
    marker = '    ev_PlayerEndTurn(playerNo);'
    text = replace_once(text, marker, marker + r'''
    qa_state("turn.end",playerNo,-1);
    if (route && (!strncmp(route,"audit-pass",10) || !strcmp(route,"audit-bullet"))) {
        qa_state("audit.end",playerNo,atoi(getenv("MP6_QA_CAPSULE")));
        mp6_event_post("qa.complete",0,"done");
        mp6_max_ticks=(int)mp6_tick_count+60;
    }
    if ((!GwPlayer[playerNo].comF && getenv("MP6_QA_ONE_TURN")) ||
        (route && !strcmp(route,"day-night") && GwSystem.curTime == 1)) {
        mp6_event_post("qa.complete",0,"done");
        mp6_max_ticks = (int)mp6_tick_count+60;
    }
''')
    return text + (ROOT / 'tests/integration/capsule_audit_fixture.c').read_text()


def shop_probe(text):
    marker = '    mbWinAttrSet(+(s16)helpWinId, HUWIN_ATTR_ALIGN_CENTER);'
    text = replace_once(text, marker, marker + r'''
    qa_state("shop.ready",work->playerNo,offerNum);
    mp6_event_post("qa.shop",offerNum,"ready");
''')
    marker = '            previous = selected;'
    return replace_once(text, marker, marker + r'''
            qa_state("shop.selected",work->playerNo,selected);
''')


def capsule_probe(text):
    marker = '    workData = HuMemDirectMallocNum(HEAP_HEAP, sizeof(CAPWORK), HU_MEMNUM_OVL);'
    text = replace_once(text, marker, '''    qa_state("capsule.begin",work->playerNo,work->capsuleNo);
    if (getenv("MP6_QA_CAPSULE") && (work->capsuleNo==atoi(getenv("MP6_QA_CAPSULE")) || work->capsuleNo==46))
        mp6_frame_dump_trigger("capsule.effect");
    mp6_event_post("qa.capsule",work->capsuleNo,"begin");
''' + marker)
    marker = '\n}\n\nvoid mbev_CapWait(CAPWORK *work)'
    return replace_once(text, marker, '\n    qa_state("capsule.return",work->playerNo,work->capsuleNo);' + marker)


def placement_probe(text):
    # Observe the real human selector; the runner supplies ordinary stick/A input.
    marker = '    CapSelectMasuListGet(masuFlag, masuId, 5, 5);\n    masuFlag[masuId] = CAPSULE_MASU_SELECT_BLOCKED;'
    text = replace_once(text, marker, marker + r'''
    const char *qa_route = getenv("MP6_QA_ROUTE");
    if (qa_route && !strcmp(qa_route,"replace-orb")) {
        for (int qi=0;qi<256;qi++) if (masuFlag[qi] & 1) {
            mbCapMasuCapsuleSet(qi,11,(work->unk00+1)%4);
            printf("[BOARD-QA] FIXTURE existing-orb space=%d capsule=11\n",qi);
        }
    }
''')
    marker = '            initialF = FALSE;'
    text = replace_once(text, marker, marker + r'''
            qa_state("placement.ready",work->unk00,masuId);
            for (int qi=0;qi<linkNum;qi++)
                printf("[BOARD-QA] placement.candidate space=%d angle=%f\n",candidates[qi],candidateAngle[qi]);
            mp6_event_post("qa.placement",0,"ready");
''')
    marker = '                    capsuleMasuSelectResult = masuId;'
    text = replace_once(text, marker, marker + '\n                    qa_state("placement.confirm",work->unk00,masuId);')
    marker = '            mbCapMasuCapsuleSet(masuId, capsuleNo, playerNo);'
    text = replace_once(text, marker, marker + r'''
            extern s16 mbCapValuePlayerGet(s16 value);
            int qa_value = mbMasuCapsuleGet(masuId);
            printf("[BOARD-QA] placement.space space=%d capsule=%d owner=%d\n",
                   masuId,mbCapValueTypeGet(qa_value),mbCapValuePlayerGet(qa_value));
''')
    for signature, label, player in [
        ('static void CapPlayerThrow(void)', 'placement.throw', 'GwSystem.turnPlayerNo'),
        ('static int CapEffThrowMasu(int masuId, int capsuleNo, int playerNo, BOOL bonusF)',
         'placement.hit', 'playerNo')]:
        function = re.search(r'^' + re.escape(signature) + r'\n\{.*?^\}',text,re.M|re.S)
        if not function:
            raise AssertionError(signature)
        original = function.group()
        changed = original.replace('{','{\n    qa_state("'+label+'.begin",'+player+',-1);',1)
        end_marker = '    HuPrcEnd();' if label == 'placement.throw' else '    return coinNum;'
        assert end_marker in changed, signature
        changed = changed.replace(end_marker,'    qa_state("'+label+'.end",'+player+',-1);\n'+end_marker)
        text = replace_once(text,original,changed)
    return text


def world_probe(text):
    text=replace_once(text,'void MB1_Create(void)\n{',r'''void MB1_Create(void)
{
    /* Night assets and board state must agree before any scene model loads. */
    const char *qa_route=getenv("MP6_QA_ROUTE");
    if (qa_route && !strncmp(qa_route,"audit-boo",9)) GwSystem.curTime=1;
''')
    text = replace_once(text, 'void ObjectSetup(void)\n{', r'''
static MBMODELID qa_scene_create(int data, const int *motion, BOOL link)
{
    MBMODELID id=mbObjCreate(data,motion,link);
    HU3D_MODELID model=mbObjModelIDGet(id);
    if (model>=0 && Hu3DData[model].hsf && Hu3DData[model].hsf->root)
        printf("[BOARD-QA] scene.asset=%08x model=%d root=%s\n",data,model,Hu3DData[model].hsf->root->name);
    return id;
}
#define mbObjCreate qa_scene_create
void ObjectSetup(void)
{''')
    marker = '    mbev_CapTeresaFadeCreate(work->baseModelId);'
    text = replace_once(text,marker,'    qa_state("boo.fade.begin",playerNo,-1);\n'+marker)
    marker = '    mbev_CapTeresaFadeKill(work->baseModelId);'
    text = replace_once(text,marker,marker+'\n    qa_state("boo.fade.end",playerNo,-1);')
    marker = 'void fn_1_6D4(OMOBJ *obj)\n{'
    if marker not in text:
        marker = 'void fn_1_6D4(OMOBJ *objP)\n{'
    text = replace_once(text, marker, marker + r'''
    static long last_tick;
    if (mp6_tick_count - last_tick >= 600) {
        last_tick = mp6_tick_count;
        qa_state("heartbeat",GwSystem.turnPlayerNo,-1);
        const char *round_limit = getenv("MP6_QA_STOP_ROUND");
        int stop_round = round_limit ? atoi(round_limit) : 7;
        if (stop_round > 0 && GwSystem.turnNo >= stop_round) {
            mp6_event_post("qa.complete",0,"done");
            mp6_max_ticks = (int)mp6_tick_count+60;
        }
    }
''')
    marker = '    t = 0.0f;\n    distance = finalLength / '
    assert text.count(marker) == 2
    text = text.replace(marker, r'''
    static int geometry_samples;
    if (geometry_samples++ < 8) {
        float expected = 0;
        for (int qi=0;qi<=100;qi++) {
            float qs = fn_1_14A90(&work->startPos,&controlPos,&work->endPos,(float)qi/100);
            expected += qs * ((qi==0 || qi==100) ? 0.5f : 1.0f) / 100;
        }
        printf("[BOARD-QA] curve function=%s tick=%ld length=%f "
               "control=(%f,%f,%f)\n",__func__,mp6_tick_count,finalLength,
               controlPos.x,controlPos.y,controlPos.z);
        printf("[BOARD-QA] curve expected_approx=%f\n",expected);
        fflush(stdout);
    }
''' + marker)
    # A starting-space fixture alone can walk AWAY from the slide/spring.
    # Install the authored hook through the real move-end dispatcher once.
    marker = 'int fn_1_760(int playerNo, s16 id)\n{'
    text = replace_once(text, marker, marker + r'''
    static int forced_event;
    const char *qa_route = getenv("MP6_QA_ROUTE");
    if (!forced_event && qa_route && !GwPlayer[playerNo].comF &&
        (!strcmp(qa_route,"slide") || !strcmp(qa_route,"spring"))) {
        forced_event = 1;
        s16 sourceSpace = mbMasuFind_MAttrIdGet(-1,!strcmp(qa_route,"slide") ? 0x40000 : 0x100);
        GwPlayer[playerNo].masuId = sourceSpace;
        mbPlayerPosReset(playerNo);
        mbPlayerMoveHookSet(playerNo,!strcmp(qa_route,"slide") ? fn_1_E234 : fn_1_2D08);
        qa_state("FIXTURE.event-hook",playerNo,sourceSpace);
        return 0;
    }
    qa_state("move.end",playerNo,id);
''')
    for name, label in [('fn_1_E234','slide'), ('fn_1_2D08','spring')]:
        function = re.search(r'^void ' + name + r'\(int playerNo\)\n\{.*?^\}', text, re.M | re.S).group()
        instrumented = function.replace('{', '{\n    qa_state("' + label + '.begin",playerNo,-1);', 1)
        instrumented = instrumented[:-1] + ('    qa_state("' + label + '.end",playerNo,-1);\n'
            '    if (getenv("MP6_QA_HOLD_EVENT_END")) while (1) HuPrcVSleep();\n}')
        text = replace_once(text, function, instrumented)
    marker = 'int MB1Ev_MasuHatena(int playerNo, s16 id)\n{'
    text = replace_once(text, marker, marker + '\n    qa_state("happening.begin",playerNo,id);')
    return text + '''
void qa_boo_house(int playerNo)
{
    fn_1_126EC(playerNo,lbl_1_bss_EB4.masuId);
}
'''


def special_probe(text):
    function = re.search(r'^static int ev_CapDonkeyStart\([^;]+?\)\n\{.*?^\}',
                         text, re.M | re.S).group()
    changed = replace_once(function, '    if (MBCapsuleEffRandF() < 0.3f) {',
        '    /* QA-only selection of the real dice branch; no waits are skipped. */\n'
        '    if ((getenv("MP6_QA_ROUTE") && !strcmp(getenv("MP6_QA_ROUTE"), "donkey-dice"))\n'
        '        || MBCapsuleEffRandF() < 0.3f) {')
    changed = replace_once(changed, '        if (donkeyDiceResultTbl[diceNo] >= 0) {',
        '        printf("[BOARD-QA] donkey.dice-result=%d reward=%d\\n", diceNo, donkeyDiceResultTbl[diceNo]);\n'
        '        if (donkeyDiceResultTbl[diceNo] >= 0) {')
    text = replace_once(text, function, changed)
    marker = '    omObj->data = HuMemDirectMallocNum(HEAP_HEAP, sizeof(CAPWORK), HU_MEMNUM_OVL);'
    text = replace_once(text, marker, r'''
    printf("[BOARD-QA] donkey.copy sizeof=%zu copied=%zu obj1.offset=%zu process.offset=%zu ids=%d,%d,%d\n",
           sizeof(CAPWORK),sizeof(CAPWORK),offsetof(CAPWORK,eventData),offsetof(CAPWORK,processNo),
           work->eventData[0],work->eventData[1],work->eventData[2]);
    fflush(stdout);
''' + marker)
    marker = 'static void ev_CapDonkeyOMExec(OMOBJ *obj)\n{'
    text = replace_once(text, marker, marker + r'''
    printf("[BOARD-QA] donkey.callback state=%d\n",obj->work[0]);
    fflush(stdout);
''')
    return sleep_probe(text)


def sleep_probe(text):
    # Identify a blocked real wait without forcing its predicate or skipping it.
    return text.replace('HuPrcVSleep();',
        '{ if (mp6_tick_count % 600 == 0) '
        'printf("[WAIT-QA] %s:%d tick=%ld\\n", __func__, __LINE__, mp6_tick_count); '
        'HuPrcVSleep(); }')


def dice_probe(text):
    function = re.search(r'^BOOL mbDiceNumStopCheck\([^;]+?\)\n\{.*?^\}',
                         text, re.M | re.S).group()
    changed = replace_once(function, '            if (work->updateF) {', r'''
            if (work->updateF) {
                if (mp6_tick_count % 600 == 0)
                    printf("[WAIT-QA] dice.num p=%d slot=%d obj=%p callback=%p expected=%p value=%d kill=%d time=%d/%d\n",
                        playerNo,i,diceNumOMObj[playerNo][i],diceNumOMObj[playerNo][i]->objFunc,
                        DiceNumObjOMExec,work->value,work->killF,work->time,work->maxTime);
''')
    return sleep_probe(replace_once(text, function, changed))


def configure(configuration):
    configure_original(configuration)
    global original_objects
    original_objects = Path(build.OBJ_DIR)
    build.BUILD_DIR = str(OUTPUT)
    build.OBJ_DIR = str(OUTPUT / 'obj')


def last5_probe(text):
    marker = 'void mbev_Last5(void)\n{'
    text = replace_once(text, marker, 'int qa_last5_active;\n' + marker + r'''
    qa_last5_active = 1;
    qa_state("last5.begin",GwSystem.turnPlayerNo,-1);
''')
    function = re.search(r'^void mbev_Last5\(void\)\n\{.*?^\}', text, re.M | re.S).group()
    changed = function[:-1] + r'''
    qa_state("last5.end",GwSystem.turnPlayerNo,-1);
    qa_last5_active = 0;
}'''
    return replace_once(text, function, changed)


def board_probe(text):
    marker = '    s32 ovl;\n    mbClose();'
    return replace_once(text, marker, '''    s32 ovl;
    if (GWPartyGet() && GwSystem.turnNo > GwSystem.turnMax)
        qa_state("board.finished",-1,GwSystem.turnMax);
    mbClose();''')


def overlay_probe(text):
    marker = 'void mp6_unavailable_overlay_return(void)\n{'
    return replace_once(text,marker,marker+r'''
    if (omCurrentOvlGet() == DLL_mdpresultdll) {
        fprintf(stderr,"[BOARD-QA] FATAL recovered results reached unavailable fallback\n");
        exit(3);
    }
''')


def results_probe(text):
    backdrop = re.search(r'^void fn_1_5160\(OMOBJ \*obj\)\n\{.*?^\}',text,re.M|re.S).group()
    changed = replace_once(backdrop, '        Hu3DModelShadowMapSet(obj->mdlId[i]);', r'''
        Hu3DModelShadowMapSet(obj->mdlId[i]);
        HSF_DATA *hsf=Hu3DData[obj->mdlId[i]].hsf;
        printf("[RESULT-ASSET] index=%d root=%s objects=%d\n",i,hsf->root->name,hsf->objectNum);
        for (int qi=0;qi<hsf->objectNum;qi++) {
            HSF_OBJECT *qo=hsf->object+qi;
            if (qo->type!=HSF_OBJ_MESH || !qo->mesh.vertex) continue;
            printf("[RESULT-ASSET] mesh=%s vertices=%d bbox=(%g,%g,%g)-(%g,%g,%g)\n",
                qo->name,qo->mesh.vertex->count,qo->mesh.mesh.min.x,qo->mesh.mesh.min.y,qo->mesh.mesh.min.z,
                qo->mesh.mesh.max.x,qo->mesh.mesh.max.y,qo->mesh.mesh.max.z);
            if (getenv("MP6_QA_RESULTS_GEOMETRY")) {
                HSF_TRANSFORM *t=&qo->mesh.base;
                printf("[RESULT-MESH] %d %d %s pos %g %g %g rot %g %g %g scale %g %g %g\n",
                    i,qi,qo->name,t->pos.x,t->pos.y,t->pos.z,t->rot.x,t->rot.y,t->rot.z,t->scale.x,t->scale.y,t->scale.z);
                for (int vi=0;vi<qo->mesh.vertex->count;vi++) {
                    HuVecF v=((HuVecF*)qo->mesh.vertex->data)[vi];
                    printf("[RESULT-V] %d %g %g %g\n",vi,v.x,v.y,v.z);
                }
                for (int fi=0;fi<qo->mesh.face->count;fi++) {
                    HSF_FACE *f=(HSF_FACE*)qo->mesh.face->data+fi;
                    int kind=f->typeSrc&HSF_FACE_MASK;
                    int n=kind==HSF_FACE_QUAD?4:3;
                    printf("[RESULT-F] %d %d %d",fi,kind,f->mat);
                    for (int c=0;c<n;c++) printf(" %d/%d/%d",f->index[c].vertex,f->index[c].st,f->index[c].normal);
                    if (kind==HSF_FACE_TRISTRIP) for (int c=0;c<f->strip.count;c++)
                        printf(" %d/%d/%d",f->strip.data[c].vertex,f->strip.data[c].st,f->strip.data[c].normal);
                    printf("\n");
                }
            }
        }
''')
    text = replace_once(text,backdrop,changed)
    text = r'''
static int qa_results_finished;
static void qa_results_mark(const char *stage) {
    printf("[RESULT-QA] %s tick=%ld\n",stage,mp6_tick_count);
    fflush(stdout);
    mp6_frame_dump_trigger(stage);
    mp6_event_post("qa.results",0,stage);
}
void qa_results_returned(void) {
    if (!qa_results_finished) return;
    qa_results_mark("results.returned");
    qa_results_finished = 0;
    mp6_event_post("qa.complete",0,"done");
    mp6_max_ticks = (int)mp6_tick_count+600;
}
''' + text
    for marker, stage in [
        ('void fn_1_F548(void)\n{', 'results.enter'),
        ('void fn_1_169A4(void)\n{', 'results.ceremony'),
        ('    fn_1_295C(MDRESULT_MESSAGE_GUIDE_START, 1);', 'results.ranking'),
        ('    fn_1_295C(MDRESULT_MESSAGE_GUIDE_RETURN, 0);', 'results.graphs'),
        ('    SLSaveBoardEndExec();', 'results.saved'),
    ]:
        text = replace_once(text, marker, marker + '\n    qa_results_mark("'+stage+'");')
    text = replace_once(text, '                GXBegin(GX_QUADS, GX_VTXFMT0, 4);', r'''
                static int qa_graph_drawn;
                if (!qa_graph_drawn) {
                    qa_graph_drawn = 1;
                    printf("[RESULT-QA] graph.draw tick=%ld\n",mp6_tick_count);
                    fflush(stdout);
                    mp6_frame_dump_trigger("graph.draw");
                }
                GXBegin(GX_QUADS, GX_VTXFMT0, 4);''')
    return replace_once(text, '    SLSaveBoardEndExec();',
                        '    qa_results_finished = 1;\n    SLSaveBoardEndExec();')


def menu_return_probe(text):
    signature = 'void ObjectSetup(void)'
    fn = re.search(r'^'+re.escape(signature)+r'\n\{.*?^\}',text,re.M|re.S).group()
    changed = fn[:-1] + '\n    extern void qa_results_returned(void);\n    qa_results_returned();\n}'
    return replace_once(text,fn,changed)


def input_probe(text):
    # The shipping script parser covers the old menu route, not shoulder keys.
    # Add R only in this diagnostic executable; the real game's pad processing
    # still receives/handles the trigger normally.
    marker = "    if (len == 1 && name[0] == 'b') return PAD_BUTTON_B;"
    return replace_once(text, marker, marker +
        "\n    if (len == 1 && name[0] == 'r') return PAD_TRIGGER_R;")


def snpc_probe(text):
    marker = '    GXSetNumTexGens(texGen + 1);\n    GXSetNumTevStages(tevStage + 1);'
    function = re.search(r'^static void FadeMatHook\([^;]+?\)\n\{.*?^\}', text, re.M | re.S).group()
    changed = replace_once(function, marker, marker + r'''
    static int samples;
    if (qa_last5_active && material->attrNum == 0 && samples < 8) {
        GXAttrType tex0;
        GXGetVtxDesc(GX_VA_TEX0, &tex0);
        printf("[BOARD-QA] last5.fade-untextured tick=%ld object=%s tex0=%d texGen=%d tevStage=%d\n",
               mp6_tick_count,drawObj->object->name,tex0,texGen,tevStage);
        fflush(stdout);
        samples++;
    }
''')
    return replace_once(text, function, changed) + r'''

/* Test-only effect harness. W01 supplies an already recovered scene and assets;
 * no undecompiled board entry point or SNPC gameplay hook is substituted. */
void qa_snpc_effects(int playerNo) {
    MBSNPCSAVEWORK save = {0};
    HuVecF pos;
    HuVecF offset = {0,150,0};
    HuVecF rotation = {-20,0,0};
    mbCameraStackPush();
    mbPlayerPosGet(playerNo,&pos);
    mbCameraMovePlayer(playerNo,&rotation,&offset,1000,25,1);
    mbCameraMoveWait();
    snpcSaveWork = &save;
    snpcWork = mbMalloc(sizeof(*snpcWork));
    save.masuId = GwPlayer[playerNo].masuId;
    for (int type=0;type<2;type++) {
        save.isKoopa = type;
        SNpcObjCreate();
        SNpcObjPosSetV(&pos);
        mbPlayerDispSet(playerNo,FALSE);
        qa_state(type ? "snpc.fire" : "snpc.banana",playerNo,type);
        SNpcEffectExec();
        qa_state("snpc.effect-end",playerNo,type);
        qa_state(type ? "snpc.star-loss" : "snpc.star-gain",playerNo,type);
        SNpcStarCreate(type,FALSE,&pos);
        SNpcStarWait();
        qa_state("snpc.star-end",playerNo,type);
        HuPrcSleep(30);
        SNpcObjKill();
    }
    HuMemDirectFree(snpcWork);
    snpcWork = NULL;
    snpcSaveWork = NULL;
    mbPlayerDispSet(playerNo,TRUE);
    mbCameraStackPop(1);
    mbCameraMoveWait();
}
'''


def collect(*args, **kwargs):
    units = []
    probes = {'board_player.o': player_probe, 'board_capevent.o': capsule_probe,
              'game_hsfmotion.o': motion_probe,
              'game_hsfdraw.o': ground_geometry_probe,
              'board_shopevent.o': shop_probe,
              'board_capsule.o': placement_probe,
              'board_capspecial.o': special_probe, 'board_dice.o': dice_probe,
              'board_last5.o': last5_probe, 'board_snpc.o': snpc_probe,
              'board_board.o': board_probe,
              'rel_mdpresult.o': results_probe,
              'rel_mdparty.o': menu_return_probe,
              'rel_boot.o': menu_return_probe,
              'rel_mdsel.o': menu_return_probe,
              'plat_input_script_aurora.o': input_probe,
              'plat_overlay_fallback_aurora.o': overlay_probe,
              'rel_world01.o': world_probe}
    for source, flags, name, flavor in collect_original(*args, **kwargs):
        obj = original_objects / name
        if name == 'plat_mp6_enhancements_aurora.o':
            contents = Path(source).read_text(encoding='utf-8')
            contents = replace_once(contents, 'void mp6_enh_cache_environment(void)\n{',
                'void mp6_enh_cache_environment(void)\n{\n'
                '    if (getenv("MP6_QA_UNCACHED_SETTINGS")) return;')
            # Count real draw-side requests without adding a clock read to
            # each one. This is never linked into a distributed executable.
            contents = replace_once(contents, 'static Mp6EnhValues g_resolved;',
                '#include <stdio.h>\nstatic unsigned long long qaEnhQueries;\n'
                'static Mp6EnhValues g_resolved;')
            for field in ('widescreen','unlockedFps','shadowQuality','aa','sfxVoices','ambientOcclusion','heapScale'):
                contents = replace_once(contents, '    if (g_resolvedValid) return g_resolved.'+field+';',
                    '    ++qaEnhQueries;\n    if (g_resolvedValid) return g_resolved.'+field+';')
            contents += '\n__attribute__((destructor)) static void qa_enh_count(void) {\n'
            contents += '    fprintf(stderr,"[ENH-QA] accessor calls=%llu cached=%d\\n",qaEnhQueries,g_resolvedValid);\n}\n'
            source = str(OUTPUT / 'enhancement_lookup_probe.c')
            Path(source).write_text(contents,encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        if name == 'plat_mp6_grounding_aurora.o':
            contents = Path(source).read_text(encoding='utf-8')
            contents = contents.replace('#include "grounding_math.h"',
                '#include "' + (ROOT / 'src/hsf/grounding_math.h').as_posix() + '"\n#include <stdlib.h>')
            contents = replace_once(contents, 'static int active(void)\n{',
                'static int active(void)\n{\n    if (getenv("MP6_QA_NO_GROUNDING")) return 0;\n'
                # A quick state includes the draw copies. Rebuild just the
                # visual registry after restore for a genuine old-floor A/B;
                # authored meshes and all simulation state remain untouched.
                '    if (getenv("MP6_QA_NO_LAWN_LIFT") && lawnAnchor.object) {\n'
                '        HU3D_MODELID terrain=terrainId, tree=treeId, ids[8];\n'
                '        int count=sceneryCount;\n'
                '        for (int i=0;i<count;i++) ids[i]=scenery[i].id;\n'
                '        mp6_ground_w01_begin(terrain);\n'
                '        for (int i=0;i<count;i++) mp6_ground_w01_scenery(ids[i]);\n'
                '        mp6_ground_w01_tree(tree);\n'
                '    }')
            contents = replace_once(contents, 'static void calibrate_lawn(HSF_OBJECT *o, Mtx world)\n{',
                'static void calibrate_lawn(HSF_OBJECT *o, Mtx world)\n{\n    if (getenv("MP6_QA_NO_LAWN_LIFT")) return;')
            contents = replace_once(contents, '    HuVecF *original=o->mesh.vertex->data;',
                '    HuVecF *original=o->mesh.vertex->data;\n'
                '    if (getenv("MP6_QA_NO_CONNECTOR_LIFT") && !strcmp(o->name,"b01_m001")) return original;\n'
                '    if (getenv("MP6_QA_NO_LOG_GROUNDING")) return original;\n'
                '    if (getenv("MP6_QA_NO_TREE_GROUNDING") && m-Hu3DData==treeId) return original;')
            source = str(OUTPUT / 'grounding_probe.c')
            Path(source).write_text(contents, encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        if name == 'plat_mp6_ambient_occlusion_aurora.o':
            contents = Path(source).read_text(encoding='utf-8')
            contents = contents.replace('#include <math.h>', '#include <math.h>\n#include <stdlib.h>')
            contents = replace_once(contents, 'void mp6_ao_foliage_camera(int camera)\n{',
                'void mp6_ao_foliage_camera(int camera)\n{\n    if (getenv("MP6_QA_LEGACY_FOLIAGE_VIEWPORT")) return;')
            source = str(OUTPUT / 'ao_camera_probe.c')
            Path(source).write_text(contents, encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        if name == 'plat_ambient_occlusion_aurora.o':
            flags = [*flags, '-I'+str(ROOT/'src/gx')]
            contents = Path(source).read_text(encoding='utf-8')
            # Observe the first admitted draw for each camera; no release probe.
            contents = replace_once(contents, '    job->frame = g_frame;', r'''
    job->frame = g_frame;
    static unsigned qaAdmittedCameras=0;
    if (view->camera>=0 && view->camera<16 && !(qaAdmittedCameras & (1u<<view->camera))) {
        qaAdmittedCameras |= 1u<<view->camera;
        std::fprintf(stderr,"[AO-QA] camera.admitted camera=%d frame=%llu\n",
                     view->camera,(unsigned long long)g_frame);
    }
''')
            contents = replace_once(contents,
                'mp6_ao_working_size(ctx.targetWidth, ctx.targetHeight, &aoWidth, &aoHeight);',
                'mp6_ao_working_size(ctx.targetWidth, ctx.targetHeight, &aoWidth, &aoHeight);\n'
                '    if (std::getenv("MP6_QA_MOBILE_AO"))\n'
                '        mp6_ao_working_size_with_limit(ctx.targetWidth,ctx.targetHeight,960,&aoWidth,&aoHeight);')
            # Keep old/new compositing in one QA executable so state guards and
            # exact scene poses remain valid for the regression comparison.
            shader = (ROOT / 'src/gx/ambient_occlusion_shader.hpp').read_text()
            unshaded = shader.replace('ambient_occlusion_shader','unshaded_ao_shader').replace(
                'let amount = clamp(1.7 * occluded / max(unoccluded, 1e-6), 0.0, 1.0);', 'let amount = 0.0;')
            foreground = shader.replace('ambient_occlusion_shader','foreground_ao_shader').replace(
                'return vec4f(foreground*(1.0-visibility),visibility);', 'return vec4f(foreground,0);')
            water = shader.replace('ambient_occlusion_shader','water_ao_shader')
            water = replace_once(water, '    let uv = pixel.xy / p.sizes.xy;',
                '    if (linear_depth(pixel.xy/p.sizes.xy)==0.0) { return vec4f(0,0,0,1); }\n'
                '    let maskAt=clamp(vec2i(pixel.xy),vec2i(0),vec2i(textureDimensions(foliageColor))-1);\n'
                '    return vec4f(vec3f(select(0.0,1.0,textureLoad(foliageColor,maskAt,0).a>0.0)),0);\n'
                '    let uv = pixel.xy / p.sizes.xy;')
            legacy = replace_once(shader, 'ambient_occlusion_shader', 'legacy_foliage_shader')
            legacy = replace_once(legacy, 'FoliageSeed(vec4f(0),', 'FoliageSeed(vec4f(1),')
            legacy = replace_once(legacy, 'return vec4f(foreground*(1.0-visibility),visibility);',
                'return vec4f(vec3f(mix(1.0,visibility,textureLoad(foliageColor,at,0).r)),1.0);')
            legacy = legacy.replace('return vec4f(0,0,0,1);', 'return vec4f(1);')
            shader_path = OUTPUT / 'foliage_comparison_shader.hpp'
            selector = '''
inline std::string qa_ao_shader(bool compatibility, bool preparedDepth=false,
                              bool attachmentDepth=false, bool multisampledDepth=false,
                              bool decals=true) {
    if (std::getenv("MP6_QA_RAW_AO_DEPTH")) {
        std::fprintf(stderr,"Raw-depth QA is incompatible with the ordered depth task.\\n");
        std::abort();
    }
    if (std::getenv("MP6_QA_WATER_AO")) return water_ao_shader(compatibility,preparedDepth,attachmentDepth,multisampledDepth,decals);
    return (std::getenv("MP6_QA_FOREGROUND_AO") ? foreground_ao_shader :
        (std::getenv("MP6_QA_UNSHADED_AO") ? unshaded_ao_shader :
        (std::getenv("MP6_QA_LEGACY_FOLIAGE_BLEND") ? legacy_foliage_shader : ambient_occlusion_shader)))(compatibility,preparedDepth,attachmentDepth,multisampledDepth,decals);
}
'''
            shader_path.write_text(shader + '\n' + legacy + '\n' + unshaded + '\n' + foreground + '\n' + water + selector, encoding='utf-8')
            # Includes the direct single/MSAA foliage seed added with borrowed
            # depth. The selector forwards all five parameters unchanged.
            if contents.count('ambient_occlusion_shader(') != 4:
                raise RuntimeError('AO source selection changed; review prepared/attachment/seed QA variants')
            contents = contents.replace('ambient_occlusion_shader(', 'qa_ao_shader(')
            contents = replace_once(contents, 'blend.color.srcFactor = wgpu::BlendFactor::One;',
                'blend.color.srcFactor = std::getenv("MP6_QA_LEGACY_FOLIAGE_BLEND") ? wgpu::BlendFactor::Dst : wgpu::BlendFactor::One;')
            contents = replace_once(contents, 'blend.color.dstFactor = wgpu::BlendFactor::SrcAlpha;',
                'blend.color.dstFactor = std::getenv("MP6_QA_LEGACY_FOLIAGE_BLEND") ? wgpu::BlendFactor::Zero : wgpu::BlendFactor::SrcAlpha;')
            contents = replace_once(contents, 'const uint32_t emptyPixel=0;',
                'const uint32_t emptyPixel=std::getenv("MP6_QA_LEGACY_FOLIAGE_BLEND") ? 0xffffffffu : 0;')
            contents = replace_once(contents, 'extern "C" int mp6_ao_foliage_begin(int camera) {',
                'extern "C" int mp6_ao_foliage_begin(int camera) {\n'
                '    if (std::getenv("MP6_QA_NO_FOLIAGE_MASK")) return 0;')
            contents = contents.replace('"ambient_occlusion_shader.hpp"',
                '"' + shader_path.as_posix() + '"')
            contents = replace_once(contents, 'extern "C" void mp6_ao_begin_frame(void) {\n', r'''
extern "C" long mp6_tick_count;
extern "C" void mp6_frame_dump_trigger(const char *);
extern "C" void mp6_ao_begin_frame(void) {
    // QA-only live-setting sweep. No scene/gameplay state is modified.
    const char *cycle = std::getenv("MP6_QA_AO_CYCLE");
    if (cycle && mp6_tick_count >= std::atoi(cycle)) {
        static int previous = -1;
        int level = (1 + (mp6_tick_count - std::atoi(cycle)) / 120) % 3;
        if (level != previous) {
            Mp6EnhValues values;
            mp6_enh_get_values(&values);
            values.ambientOcclusion = level;
            mp6_enh_set_values(&values);
            previous = level;
            std::fprintf(stderr, "[AO-QA] live level=%d tick=%ld frame=%llu\\n",
                         level, mp6_tick_count, (unsigned long long)g_frame);
            mp6_frame_dump_trigger("ao.cycle");
        }
    }
''')
            source = str(OUTPUT / 'ambient_occlusion_probe.cpp')
            Path(source).write_text(contents, encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        if name in ('plat_framescope_aurora.o', 'plat_aurora_bridge_aurora.o'):
            contents = Path(source).read_text(encoding='utf-8')
            if name == 'plat_framescope_aurora.o':
                contents = gx_state_probe(contents)
            else:
                contents = contents.replace('    GXBegin(type, vtxfmt, nverts);',
                    '    extern void qa_gx_check(void); qa_gx_check();\n    GXBegin(type, vtxfmt, nverts);')
                contents = contents.replace('    GXCallDisplayList(list, nbytes);',
                    '    extern void qa_gx_check(void); qa_gx_check();\n    GXCallDisplayList(list, nbytes);')
            source = str(OUTPUT / (name[:-2] + '_probe.c'))
            Path(source).write_text(contents, encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        if name in probes:
            prelude = '' if name == 'game_hsfdraw.o' else PRELUDE
            contents = prelude + probes[name](Path(source).read_text(encoding='utf-8'))
            source = str(OUTPUT / (name[:-2] + '_probe.c'))
            Path(source).write_text(contents, encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        units.append((source, flags, str(obj), flavor))
    return units


def ground_geometry_probe(text):
    foliage = (ROOT / 'include/mp6_ao_foliage_draw.h').read_text()
    foliage = replace_once(foliage, '    int water=mp6_ao_water_surface(draw);', r'''
    int water=mp6_ao_water_surface(draw);
    if (getenv("MP6_QA_WATER_AO")) {
        static int seen[512];
        int id=(int)(draw->model-Hu3DData);
        if (id>=0 && id<512 && !seen[id]) {
            seen[id]=1;
            fprintf(stderr,"[WATER-QA] model=%d root=%s object=%s water=%d\n",id,
                draw->model->hsf->root->name,draw->object->name,water);
        }
    }''')
    foliage = replace_once(foliage, '    if (!mp6AoFoliageDrawing) return;', '''
    if (!mp6AoFoliageDrawing) return;
    if (getenv("MP6_QA_LEGACY_FOLIAGE_BLEND")) {
        for (int stage=0;stage<GX_MAX_TEVSTAGE;stage++) {
            GXSetTevColorIn(stage,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO);
            GXSetTevColorOp(stage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
        }
    }''')
    foliage_path = OUTPUT / 'foliage_comparison_draw.h'
    foliage = replace_once(foliage, 'GXSetZMode(GX_TRUE,GX_LEQUAL,GX_FALSE);',
        'GXSetZMode(GX_TRUE,getenv("MP6_QA_FOLIAGE_NO_DEPTH") ? GX_ALWAYS : GX_LEQUAL,GX_FALSE);')
    foliage_path.write_text(foliage, encoding='utf-8')
    text = replace_once(text, '#include "mp6_ao_foliage_draw.h"', '#include "' + foliage_path.as_posix() + '"')
    marker = 'static void ObjDraw(HU3D_DRAW_OBJ *drawObj)\n{'
    header = (ROOT / 'tests/integration/ground_geometry_probe.h').as_posix()
    text = replace_once(text, marker, '#include "' + header + '"\n' + marker +
                        '\n    qa_ground_geometry(drawObj);')
    text = replace_once(text, 'void Hu3DDrawPost(void)\n{',
                        'static void qa_ground_geometry(HU3D_DRAW_OBJ *draw);\n'
                        'void Hu3DDrawPost(void)\n{')
    return replace_once(text, '                Hu3DObjInfoP = drawObj->object->constData;',
                        '                qa_ground_geometry(drawObj);\n'
                        '                Hu3DObjInfoP = drawObj->object->constData;')


def motion_probe(text):
    fn = re.search(r'^void Hu3DMotionNext\([^;]+?\)\n\{.*?^\}', text, re.M | re.S).group()
    changed = fn.replace('{', r'''{
    extern int qa_motion_model[4], qa_motion_slot[4];
    extern unsigned qa_motion_serial[4];
    static unsigned ended[4];
    int oldId = Hu3DData[modelId].motId;
    int oldShift = Hu3DData[modelId].motIdShift;
    float oldTime = Hu3DData[modelId].motWork.time;
''', 1)
    changed = changed[:-1] + r'''
    for (int p=0;p<4;p++) {
        if (qa_motion_model[p]!=modelId || oldId!=modelP->motId ||
            oldShift!=-1 || modelP->motIdShift!=-1) continue;
        if (modelP->motWork.time<oldTime && !(modelP->motAttr & HU3D_MOTATTR_REV)
            && qa_motion_slot[p]!=1 && qa_motion_slot[p]!=2 && qa_motion_slot[p]!=3)
            printf("[BOARD-QA] motion.wrap tick=%ld p=%d slot=%d attr=%08x serial=%u before=%.2f after=%.2f end=%.2f\n",
                   mp6_tick_count,p,qa_motion_slot[p],modelP->motAttr,qa_motion_serial[p],
                   oldTime,modelP->motWork.time,modelP->motWork.end);
        if (modelP->motWork.time>=modelP->motWork.end && ended[p]!=qa_motion_serial[p]) {
            ended[p]=qa_motion_serial[p];
            printf("[BOARD-QA] motion.end tick=%ld p=%d slot=%d attr=%08x serial=%u at=%.2f end=%.2f\n",
                   mp6_tick_count,p,qa_motion_slot[p],modelP->motAttr,qa_motion_serial[p],
                   modelP->motWork.time,modelP->motWork.end);
        }
    }
}'''
    return replace_once(text, fn, changed)


def gx_state_probe(text):
    marker = '/* ---- intercepted wrappers: log + forward ---- */'
    text = replace_once(text, marker, r'''
static int qa_stage_count;
static GXTexCoordID qa_coord[16];
static GXTexMapID qa_map[16];
static GXTexGenSrc qa_source[8];
static void *qa_source_caller[8];
void qa_gx_check(void) {
    static int reports;
    for (int s=0;s<qa_stage_count && s<16;s++) {
        int tc=qa_coord[s];
        if (tc<0 || tc>=8 || qa_map[s]==GX_TEXMAP_NULL || qa_map[s]&GX_TEX_DISABLE) continue;
        int src=qa_source[tc];
        if (src<GX_TG_TEX0 || src>GX_TG_TEX7) continue;
        GXAttrType type;
        GXGetVtxDesc(GX_VA_TEX0+src-GX_TG_TEX0,&type);
        if (type==GX_NONE && reports++<20) {
            extern void mp6_symbolize_addr(void *,char *,size_t);
            char symbol[256];
            mp6_symbolize_addr(qa_source_caller[tc],symbol,sizeof(symbol));
            printf("[BOARD-QA] missing-uv tick=%ld draw=%u stage=%d tc=%d src=%d setter=%s\n",
                   mp6_tick_count,mp6_current_draw_index(),s,tc,src,symbol);
            fflush(stdout);
        }
    }
}
''' + marker)
    marker = '{ fs_log("NumTevStages %d", n);'
    text = replace_once(text,marker,'{ qa_stage_count=n; '+marker[2:])
    marker = '{ fs_log("TevOrder    st%d  texcoord=%d TEXMAP=%d chan=%d", st, tc, tm, ch);'
    text = replace_once(text,marker,'{ if ((unsigned)st<16) {qa_coord[st]=tc;qa_map[st]=tm;} '+marker[2:])
    marker = 'void mp6_fs_GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType fn, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 postMtx)\n{'
    text = replace_once(text,marker,marker+'\n    if ((unsigned)dst<8) {qa_source[dst]=src;qa_source_caller[dst]=__builtin_return_address(0);}')
    return text


build.configure_windows = configure
build.collect_units = collect
sys.argv = [str(ROOT / 'tools/build.py'), '--configuration', 'release', '-j6']
result = build.main()
if result == 0:
    exe = OUTPUT / 'release/mp6native.exe'
    READY.write_text(json.dumps({'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                                'builder_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                'capsule_fixture_sha256': hashlib.sha256((ROOT/'tests/integration/capsule_audit_fixture.c').read_bytes()).hexdigest()}))
raise SystemExit(result)
