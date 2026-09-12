/* Compiled only into build/board-qa: explicit effect-entry fixtures, not a
 * certification of natural board routing, menu selection or random outcomes. */
static void qa_capsule_audit(int playerNo, const char *route)
{
    extern int mbev_CapCall(int,int,BOOL,BOOL);
    extern void mbev_CapCallTeresa(int,int);
    extern void mbev_CapCallKoopa(int);
    extern void mbev_CapCallMiracle(int,int);
    extern void mbCapPlayerThrow(int,int,int);
    extern void mbCapMasuCapsuleSet(int,int,int);
    extern s16 mbCapUseModeGet(s16);
    extern void qa_boo_house(int);
    static int ran;
    if (ran++) return;
    int id=atoi(getenv("MP6_QA_CAPSULE"));
    int source=GwPlayer[playerNo].masuId, target=-1;
    for (int i=1;i<mbMasuNumGet();i++) if (i!=source && mbMasuTypeGet(i)==1) {
        target=i; break;
    }
    if (target<0) { printf("[BOARD-QA] FATAL no blue fixture space\n"); exit(3); }
    for (int p=0;p<4;p++) {
        GwPlayer[p].comF=TRUE;
        GwPlayer[p].coin=50;
        GwPlayer[p].star=p+1;
        mbPlayerCapsuleAdd(p,0);
        mbPlayerCapsuleAdd(p,11);
        if (p!=playerNo) {
            int skip=p;
            for (int s=1;s<mbMasuNumGet();s++) if (mbMasuTypeGet(s)==1 && s!=source && !skip--) {
                GwPlayer[p].masuId=GwPlayer[p].masuIdNext=s;
                mbPlayerPosReset(p);
                break;
            }
        }
    }
    GwPlayer[playerNo].diceNum=1;
    GwPlayer[playerNo].moveNum=5;
    mbPlayerPosReset(playerNo);
    qa_state("audit.begin",playerNo,id);
    printf("[CAPSULE-AUDIT] route=%s id=%d source=%d target=%d\n",route,id,source,target);
    if (!strncmp(route,"audit-pass",10)) {
        /* The first authored walk from the start enters space 28. Use the
         * normal branch/movement/trap path, with a one-step test dice roll. */
        mbCapMasuCapsuleSet(28,id,(playerNo+1)%4);
        while (mbPlayerCapsuleNumGet(playerNo)) mbPlayerCapsuleRemove(playerNo,0);
        printf("[CAPSULE-AUDIT] pass-target=28 packed=%d\n",mbMasuCapsuleGet(28));
        return;
    } else if (!strcmp(route,"audit-bullet")) {
        while (mbPlayerCapsuleNumGet(playerNo)) mbPlayerCapsuleRemove(playerNo,0);
        int victim=(playerNo+1)%4;
        GwPlayer[victim].masuId=GwPlayer[victim].masuIdNext=28;
        mbPlayerPosReset(victim);
        mbev_CapCall(playerNo,4,TRUE,FALSE);
        return; /* Real DiceRun -> KillerMove -> turn cleanup. */
    } else if (!strcmp(route,"audit-place")) {
        mbPlayerCapsuleAdd(playerNo,id);
        mbCapPlayerThrow(playerNo,target,id);
        printf("[CAPSULE-AUDIT] placed=%d\n",mbMasuCapsuleGet(target));
    } else if (!strcmp(route,"audit-land") || !strcmp(route,"audit-land-owned")) {
        int owned=-1;
        if (!strcmp(route,"audit-land-owned")) {
            for (int s=1;s<mbMasuNumGet();s++) if (s!=source && s!=target && mbMasuTypeGet(s)==1) {
                owned=s; break;
            }
            if (owned<0) { printf("[BOARD-QA] FATAL missing owned-space fixture\n"); exit(3); }
            mbCapMasuCapsuleSet(owned,10,playerNo);
            printf("[CAPSULE-AUDIT] ownership.before space=%d owner=%d\n",owned,mbCapMasuPlayerGet(owned));
        }
        GwPlayer[playerNo].masuId=target;
        mbPlayerPosReset(playerNo);
        mbCapMasuCapsuleSet(target,id,(playerNo+1)%4);
        mbev_CapCall(playerNo,mbMasuCapsuleGet(target),FALSE,mbCapUseModeGet(id)==2);
        if (owned>=0) printf("[CAPSULE-AUDIT] ownership.after space=%d owner=%d\n",owned,mbCapMasuPlayerGet(owned));
    } else if (!strcmp(route,"audit-boo-house")) {
        if (GwSystem.curTime!=1) { printf("[BOARD-QA] FATAL Boo requires night assets\n"); exit(3); }
        GwPlayer[playerNo].masuId=mbMasuFind_MAttrIdGet(-1,0x01000000);
        mbPlayerPosReset(playerNo);
        mbCameraPlayerViewSet(playerNo,0);
        mbCameraMoveWait();
        qa_boo_house(playerNo);
    } else if (!strcmp(route,"audit-boo")) {
        if (id==31) for (int p=0;p<4;p++) if (p!=playerNo) {
            while (mbPlayerCapsuleNumGet(p)) mbPlayerCapsuleRemove(p,0);
            mbPlayerCapsuleAdd(p,31);
        }
        mbev_CapCallTeresa(playerNo,source);
    } else if (!strcmp(route,"audit-special")) {
        if (id==41) mbev_CapCallKettou(playerNo,source,TRUE);
        else if (id==42) mbev_CapCallMiracle(playerNo,source);
        else if (id==43) mbev_CapCallKoopa(playerNo);
        else if (id==44) mbev_CapCallDonkey(playerNo);
        else { printf("[BOARD-QA] FATAL invalid special fixture\n"); exit(3); }
    } else if (!strcmp(route,"audit-use")) {
        mbev_CapCall(playerNo,id,TRUE,FALSE);
    } else { printf("[BOARD-QA] FATAL invalid audit route\n"); exit(3); }
    HuPrcSleep(120);
    qa_state("audit.end",playerNo,id);
    mp6_event_post("qa.complete",id,"done");
    mp6_max_ticks=(int)mp6_tick_count+60;
    while (1) HuPrcVSleep();
}
