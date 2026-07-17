/* Round-trip self-test for platform/os/save_endian.c (docs/ARCHITECTURE.md's
 * Save system section).
 * 1) Real-data test (optional argv[1] = a console/Dolphin .gci): box 0's
 *    GW_COMMON image -> from_be -> print key fields -> to_be -> byte-diff vs
 *    the original image (must be 0 differing bytes).
 * 2) Synthetic test: fill each struct's every field with a distinct rolling
 *    pattern via native member access -> to_be -> from_be -> compare every
 *    field; then to_be again and require image equality (catches asymmetric
 *    transcription bugs in either direction). Plus absolute on-card offset
 *    spot-checks.
 *
 * Not part of the game build -- compile standalone whenever save_endian.c or
 * the gamework.h structs change (from the repo root; zig path per tools/build.py):
 *
 *   <zig> cc -target x86_64-windows-gnu -funsigned-char -fcommon -fwrapv -w
 *     -I build/msl_override -I build/patched_include
 *     -I ../../external_refs/repos/marioparty6/include
 *     -I ../../external_refs/repos/marioparty6/build/GP6E01/include
 *     -I shim/include -include dolphin_compat.h
 *     tools/save_endian_selftest.c platform/os/save_endian.c -o build/save_endian_selftest.exe
 *   build/save_endian_selftest.exe build/testsaves/pristine_dolphin.gci
 *
 * NOTE: the real-data check asserts the pristine fixture's known content
 * (name "AAA") as an extra guard against reading the wrong box offset; drop
 * that one line if pointing it at a different .gci.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dolphin/types.h"
#include "game/gamework.h"
#include "mp6_save_endian.h"

static int fails;
#define CHK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static u32 pat_state = 1;
static u32 pat(u32 mask)
{
    pat_state = pat_state * 1103515245u + 12345u;
    return (pat_state >> 8) & mask;
}

static void fill_common(GW_COMMON *c)
{
    int i, j;
    memset(c, 0, sizeof(*c));
    memcpy(c->magic, "SAVE", 4);
    c->unk4 = (u16)pat(0xFFFF);
    c->languageNo = pat(7); c->outputMode = pat(3); c->vibrateF = pat(1); c->mic = pat(3);
    c->time = ((u64)pat(0xFFFFFFFF) << 32) | pat(0xFFFFFFFF);
    for (i = 0; i < 17; i++) c->name[i] = (char)pat(0xFF);
    for (i = 0; i < 4; i++) c->mgUnlock[i] = pat(0xFFFFFFFF);
    for (i = 0; i < GW_RECORD_MAX; i++) c->record[i] = pat(0xFFFFFFFF);
    for (i = 0; i < GW_BOARD_MAX; i++) {
        for (j = 0; j < GW_CHARA_MAX; j++) {
            c->charPlayNum[i][j] = (u16)pat(0xFFFF);
            c->boardMaxStar[i][j] = (u16)pat(0xFFFF);
            c->boardMaxCoin[i][j] = (u16)pat(0xFFFF);
        }
        c->boardPlayNum[i] = (u16)pat(0xFFFF);
    }
    for (i = 0; i < 4; i++) c->singleMgWinNum[i] = (u8)pat(0xFF);
    for (i = 0; i < 3; i++) c->singleBoardPlayNum[i] = (u8)pat(0xFF);
    for (i = 0; i < 3; i++) c->singleBoardFlag[i] = ((u64)pat(0xFFFFFFFF) << 32) | pat(0xFFFFFFFF);
    c->saveEnableF = pat(1); c->map7Unlock = pat(1); c->veryHardUnlock = pat(1);
    c->m562VeryHardUnlock = pat(1); c->unkFlag4 = pat(1); c->viewOpening = pat(1); c->viewEnding = pat(1);
    c->partyMgInstDispF = pat(1); c->partyMgComDispF = pat(1); c->partyMgPack = pat(7); c->partyMessSpeed = pat(3);
    c->partySaveMode = pat(3); c->storyMgInstDispF = pat(1); c->storyMgComDispF = pat(1); c->storyMgPack = pat(7);
    c->storyMessSpeed = pat(3); c->storySaveMode = pat(3);
    c->confTurnNum = pat(0x7F); c->confBonusStar = pat(1);
    c->confTag = pat(1); c->confSingleDiff = pat(3);
    c->lastBoard = (s8)pat(0x7F);
    for (i = 0; i < GW_DECA_SCORE_MAX; i++) {
        c->decaScore[i].charNo = (s8)pat(0x7F);
        for (j = 0; j < 10; j++) c->decaScore[i].mgScore[j] = (u16)pat(0xFFFF);
        c->decaScore[i].finalScore = (u16)pat(0xFFFF);
    }
    for (i = 0; i < 10; i++) c->decaMgRecord[i] = (u16)pat(0xFFFF);
    c->renshoMgRecordNum = (u8)pat(0xFF);
    for (i = 0; i < GW_RENSHO_MG_MAX; i++) c->renshoMgRecord[i] = (u8)pat(0xFF);
    c->bankStar = (u16)pat(0xFFFF); c->bankStarAward = (u16)pat(0xFFFF);
    for (i = 0; i < 2; i++) { c->bankFlag[i] = pat(0xFFFFFFFF); c->miracleBookFlag[i] = pat(0xFFFFFFFF); c->singlePrizeFlag[i] = pat(0xFFFFFFFF); }
    for (i = 0; i < 3; i++) c->mikeActRecord[i] = pat(0xFFFFFFFF);
    c->unk5BC = (u8)pat(0xFF);
    for (i = 0; i < 8; i++) c->unk5C8[i] = (u8)pat(0xFF);
}

#define FEQ(a, b, f) CHK((a).f == (b).f, "GW field mismatch: %s (%ld != %ld)", #f, (long)(a).f, (long)(b).f)

static void cmp_common(const GW_COMMON *a, const GW_COMMON *b)
{
    int i, j;
    CHK(memcmp(a->magic, b->magic, 4) == 0, "common.magic");
    FEQ(*a, *b, unk4);
    FEQ(*a, *b, languageNo); FEQ(*a, *b, outputMode); FEQ(*a, *b, vibrateF); FEQ(*a, *b, mic);
    CHK(a->time == b->time, "common.time");
    CHK(memcmp(a->name, b->name, 17) == 0, "common.name");
    for (i = 0; i < 4; i++) CHK(a->mgUnlock[i] == b->mgUnlock[i], "mgUnlock[%d]", i);
    for (i = 0; i < GW_RECORD_MAX; i++) CHK(a->record[i] == b->record[i], "record[%d]", i);
    for (i = 0; i < GW_BOARD_MAX; i++) {
        for (j = 0; j < GW_CHARA_MAX; j++) {
            CHK(a->charPlayNum[i][j] == b->charPlayNum[i][j], "charPlayNum[%d][%d]", i, j);
            CHK(a->boardMaxStar[i][j] == b->boardMaxStar[i][j], "boardMaxStar[%d][%d]", i, j);
            CHK(a->boardMaxCoin[i][j] == b->boardMaxCoin[i][j], "boardMaxCoin[%d][%d]", i, j);
        }
        CHK(a->boardPlayNum[i] == b->boardPlayNum[i], "boardPlayNum[%d]", i);
    }
    CHK(memcmp(a->singleMgWinNum, b->singleMgWinNum, 4) == 0, "singleMgWinNum");
    CHK(memcmp(a->singleBoardPlayNum, b->singleBoardPlayNum, 3) == 0, "singleBoardPlayNum");
    for (i = 0; i < 3; i++) CHK(a->singleBoardFlag[i] == b->singleBoardFlag[i], "singleBoardFlag[%d]", i);
    FEQ(*a, *b, saveEnableF); FEQ(*a, *b, map7Unlock); FEQ(*a, *b, veryHardUnlock);
    FEQ(*a, *b, m562VeryHardUnlock); FEQ(*a, *b, unkFlag4); FEQ(*a, *b, viewOpening); FEQ(*a, *b, viewEnding);
    FEQ(*a, *b, partyMgInstDispF); FEQ(*a, *b, partyMgComDispF); FEQ(*a, *b, partyMgPack); FEQ(*a, *b, partyMessSpeed);
    FEQ(*a, *b, partySaveMode); FEQ(*a, *b, storyMgInstDispF); FEQ(*a, *b, storyMgComDispF); FEQ(*a, *b, storyMgPack);
    FEQ(*a, *b, storyMessSpeed); FEQ(*a, *b, storySaveMode);
    FEQ(*a, *b, confTurnNum); FEQ(*a, *b, confBonusStar); FEQ(*a, *b, confTag); FEQ(*a, *b, confSingleDiff);
    FEQ(*a, *b, lastBoard);
    for (i = 0; i < GW_DECA_SCORE_MAX; i++) {
        CHK(a->decaScore[i].charNo == b->decaScore[i].charNo, "decaScore[%d].charNo", i);
        for (j = 0; j < 10; j++) CHK(a->decaScore[i].mgScore[j] == b->decaScore[i].mgScore[j], "decaScore[%d].mgScore[%d]", i, j);
        CHK(a->decaScore[i].finalScore == b->decaScore[i].finalScore, "decaScore[%d].finalScore", i);
    }
    for (i = 0; i < 10; i++) CHK(a->decaMgRecord[i] == b->decaMgRecord[i], "decaMgRecord[%d]", i);
    FEQ(*a, *b, renshoMgRecordNum);
    CHK(memcmp(a->renshoMgRecord, b->renshoMgRecord, GW_RENSHO_MG_MAX) == 0, "renshoMgRecord");
    FEQ(*a, *b, bankStar); FEQ(*a, *b, bankStarAward);
    for (i = 0; i < 2; i++) {
        CHK(a->bankFlag[i] == b->bankFlag[i], "bankFlag[%d]", i);
        CHK(a->miracleBookFlag[i] == b->miracleBookFlag[i], "miracleBookFlag[%d]", i);
        CHK(a->singlePrizeFlag[i] == b->singlePrizeFlag[i], "singlePrizeFlag[%d]", i);
    }
    for (i = 0; i < 3; i++) CHK(a->mikeActRecord[i] == b->mikeActRecord[i], "mikeActRecord[%d]", i);
    FEQ(*a, *b, unk5BC);
    CHK(memcmp(a->unk5C8, b->unk5C8, 8) == 0, "unk5C8");
}

static void fill_system(GW_SYSTEM *s)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->partyF = pat(1); s->tagF = pat(1);
    s->storyComDif = (u8)pat(0xFF);
    s->bonusStarF = pat(1); s->mgInstDispF = pat(1); s->mgComDispF = pat(1);
    s->mgPack = pat(7); s->messSpeed = pat(3); s->saveMode = pat(3);
    s->turnNo = (u8)pat(0xFF); s->turnMax = (u8)pat(0xFF);
    s->starFlag = (u8)pat(0xFF); s->starTotal = (u8)pat(0xFF);
    s->starPos = pat(7); s->boardNo = pat(0x1F);
    s->last5Effect = (s8)pat(0x7F); s->turnPlayerNo = (s8)pat(0x7F);
    s->storyCharBit = (u8)pat(0xFF); s->storyChar = (s8)pat(0x7F);
    s->hiddenBlockMasuId = (s16)pat(0x7FFF);
    s->nextTime = pat(1); s->curTime = pat(1);
    s->timeTurn = (u8)pat(0xFF); s->timeTurnMax = (u8)pat(0xFF);
    for (i = 0; i < GW_BOARD_WORK_SIZE / 4; i++) s->boardWork[i] = pat(0xFFFFFFFF);
    s->comKeyDelay = (u8)pat(0xFF);
    s->mgEvent = pat(0xF);
    s->playerMode = (s8)pat(0x7F);
    s->mgNo = (u16)pat(0xFFFF); s->subGameNo = (s16)pat(0x7FFF); s->bankCoin = (u16)pat(0xFFFF);
    for (i = 0; i < 256; i++) s->masuCapsule[i] = (s16)(pat(0xFFFF) - 0x8000);
    for (i = 0; i < 128; i++) ((u8 *)s->flag)[i] = (u8)pat(0xFF);
}

static void cmp_system(const GW_SYSTEM *a, const GW_SYSTEM *b)
{
    int i;
    FEQ(*a, *b, partyF); FEQ(*a, *b, tagF); FEQ(*a, *b, storyComDif);
    FEQ(*a, *b, bonusStarF); FEQ(*a, *b, mgInstDispF); FEQ(*a, *b, mgComDispF);
    FEQ(*a, *b, mgPack); FEQ(*a, *b, messSpeed); FEQ(*a, *b, saveMode);
    FEQ(*a, *b, turnNo); FEQ(*a, *b, turnMax); FEQ(*a, *b, starFlag); FEQ(*a, *b, starTotal);
    FEQ(*a, *b, starPos); FEQ(*a, *b, boardNo); FEQ(*a, *b, last5Effect); FEQ(*a, *b, turnPlayerNo);
    FEQ(*a, *b, storyCharBit); FEQ(*a, *b, storyChar); FEQ(*a, *b, hiddenBlockMasuId);
    FEQ(*a, *b, nextTime); FEQ(*a, *b, curTime); FEQ(*a, *b, timeTurn); FEQ(*a, *b, timeTurnMax);
    for (i = 0; i < GW_BOARD_WORK_SIZE / 4; i++) CHK(a->boardWork[i] == b->boardWork[i], "boardWork[%d]", i);
    FEQ(*a, *b, comKeyDelay); FEQ(*a, *b, mgEvent); FEQ(*a, *b, playerMode);
    FEQ(*a, *b, mgNo); FEQ(*a, *b, subGameNo); FEQ(*a, *b, bankCoin);
    for (i = 0; i < 256; i++) CHK(a->masuCapsule[i] == b->masuCapsule[i], "masuCapsule[%d]", i);
    CHK(memcmp(a->flag, b->flag, 128) == 0, "system.flag");
}

static void fill_player(GW_PLAYER *p)
{
    int i;
    memset(p, 0, sizeof(*p));
    p->comDif = pat(3); p->comF = pat(1); p->charNo = pat(0xF);
    p->autoSize = pat(3); p->deadF = pat(1); p->diceMode = pat(0x3F);
    p->team = pat(1); p->skipEventF = pat(1); p->playerNo = pat(3); p->biriQF = pat(1); p->metalF = pat(1);
    p->handicap = (s8)pat(0x7F); p->padNo = (s8)pat(0x7F);
    for (i = 0; i < 3; i++) p->capsule[i] = (s8)pat(0x7F);
    p->statusColor = pat(7); p->moveF = pat(1); p->jumpF = pat(1); p->dispLightF = pat(1);
    p->orderNo = pat(3); p->diceNum = pat(3); p->rank = pat(3); p->koopaSuit = pat(1); p->teamBackup = pat(1);
    p->moveNum = (s8)pat(0x7F);
    p->masuId = (s16)pat(0x7FFF); p->masuIdPrev = (s16)pat(0x7FFF); p->masuIdNext = (s16)pat(0x7FFF);
    p->capsuleUse = (s16)pat(0x7FFF);
    p->plusMasuNum = (s8)pat(0x7F); p->minusMasuNum = (s8)pat(0x7F); p->capsuleMasuNum = (s8)pat(0x7F);
    p->hatenaMasuNum = (s8)pat(0x7F); p->koopaMasuNum = (s8)pat(0x7F); p->miracleMasuNum = (s8)pat(0x7F);
    p->kettouMasuNum = (s8)pat(0x7F); p->donkeyMasuNum = (s8)pat(0x7F);
    p->coin = (s16)pat(0x7FFF); p->coinTotalMg = (s16)pat(0x7FFF); p->coinTotal = (s16)pat(0x7FFF);
    p->coinMax = (s16)pat(0x7FFF); p->coinBattle = (s16)pat(0x7FFF);
    p->mgCoin = (s16)(pat(0xFFFF) - 0x8000); p->mgCoinBonus = (s16)(pat(0xFFFF) - 0x8000);
    p->mgScore = (s32)pat(0x7FFFFFFF);
    p->star = (s16)pat(0x7FFF); p->starMax = (s16)pat(0x7FFF); p->capsuleUseNum = (s16)pat(0x7FFF);
    for (i = 0; i < GW_PLAYER_GRAPH_SIZE; i++) {
        p->starGraph[i] = (s16)(pat(0xFFFF) - 0x8000);
        p->coinGraph[i] = (s16)(pat(0xFFFF) - 0x8000);
    }
}

static void cmp_player(const GW_PLAYER *a, const GW_PLAYER *b)
{
    int i;
    FEQ(*a, *b, comDif); FEQ(*a, *b, comF); FEQ(*a, *b, charNo); FEQ(*a, *b, autoSize);
    FEQ(*a, *b, deadF); FEQ(*a, *b, diceMode);
    FEQ(*a, *b, team); FEQ(*a, *b, skipEventF); FEQ(*a, *b, playerNo); FEQ(*a, *b, biriQF); FEQ(*a, *b, metalF);
    FEQ(*a, *b, handicap); FEQ(*a, *b, padNo);
    CHK(memcmp(a->capsule, b->capsule, 3) == 0, "player.capsule");
    FEQ(*a, *b, statusColor); FEQ(*a, *b, moveF); FEQ(*a, *b, jumpF); FEQ(*a, *b, dispLightF);
    FEQ(*a, *b, orderNo); FEQ(*a, *b, diceNum); FEQ(*a, *b, rank); FEQ(*a, *b, koopaSuit); FEQ(*a, *b, teamBackup);
    FEQ(*a, *b, moveNum); FEQ(*a, *b, masuId); FEQ(*a, *b, masuIdPrev); FEQ(*a, *b, masuIdNext);
    FEQ(*a, *b, capsuleUse);
    FEQ(*a, *b, plusMasuNum); FEQ(*a, *b, minusMasuNum); FEQ(*a, *b, capsuleMasuNum); FEQ(*a, *b, hatenaMasuNum);
    FEQ(*a, *b, koopaMasuNum); FEQ(*a, *b, miracleMasuNum); FEQ(*a, *b, kettouMasuNum); FEQ(*a, *b, donkeyMasuNum);
    FEQ(*a, *b, coin); FEQ(*a, *b, coinTotalMg); FEQ(*a, *b, coinTotal); FEQ(*a, *b, coinMax); FEQ(*a, *b, coinBattle);
    FEQ(*a, *b, mgCoin); FEQ(*a, *b, mgCoinBonus); FEQ(*a, *b, mgScore);
    FEQ(*a, *b, star); FEQ(*a, *b, starMax); FEQ(*a, *b, capsuleUseNum);
    for (i = 0; i < GW_PLAYER_GRAPH_SIZE; i++) {
        CHK(a->starGraph[i] == b->starGraph[i], "starGraph[%d]", i);
        CHK(a->coinGraph[i] == b->coinGraph[i], "coinGraph[%d]", i);
    }
}

int main(int argc, char **argv)
{
    static u8 img[0x5D0], img2[0x5D0], simg[0x2C0], simg2[0x2C0], pimg[0x108], pimg2[0x108];
    static GW_COMMON c1, c2;
    static GW_SYSTEM sy1, sy2;
    static GW_PLAYER pl1, pl2;
    int i, iter;

    /* ---- 1) real-data test on the user's pristine .gci, box 0 ---- */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        static u8 gci[0xA040];
        size_t n;
        if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
        n = fread(gci, 1, sizeof(gci), f);
        fclose(f);
        if (n != sizeof(gci)) { printf("short read %zu\n", n); return 2; }
        memcpy(img, gci + 0x40 + 0x2042, 0x5D0);
        mp6_save_common_from_be(&c1, img);
        printf("real save box0: magic=%.4s name=\"%s\" bankStar=%u bankStarAward=%u lastBoard=%d\n",
               c1.magic, c1.name, c1.bankStar, c1.bankStarAward, c1.lastBoard);
        printf("  time=0x%016llX languageNo=%u outputMode=%u vibrateF=%u mic=%u saveEnableF=%u\n",
               (unsigned long long)c1.time, c1.languageNo, c1.outputMode, c1.vibrateF, c1.mic, c1.saveEnableF);
        {
            /* independent expectations, composed straight from the raw bytes */
            u16 bs = (u16)((img[0x59A] << 8) | img[0x59B]);
            u64 t = 0; for (i = 0; i < 8; i++) t = (t << 8) | img[8 + i];
            CHK(c1.bankStar == bs, "real bankStar %u != %u", c1.bankStar, bs);
            CHK((u64)c1.time == t, "real time");
            CHK(strcmp(c1.name, "AAA") == 0, "real name '%s'", c1.name);
            CHK(memcmp(c1.magic, "SAVE", 4) == 0, "real magic");
        }
        mp6_save_common_to_be(img2, &c1);
        {
            int diffs = 0;
            for (i = 0; i < 0x5D0; i++) {
                if (img[i] != img2[i]) {
                    if (diffs < 24) printf("  byte diff @0x%03X: file=%02X remarshal=%02X\n", i, img[i], img2[i]);
                    diffs++;
                }
            }
            printf("real-data GW_COMMON re-marshal: %d differing byte(s) of 0x5D0\n", diffs);
            CHK(diffs == 0, "real-data image round-trip");
        }
        /* and the from_be(to_be(x)) direction on the real data */
        mp6_save_common_from_be(&c2, img2);
        cmp_common(&c1, &c2);
    }

    /* ---- 2) synthetic full-field round-trips, several seeds ---- */
    for (iter = 0; iter < 8; iter++) {
        pat_state = 0x1234 + iter * 977;
        fill_common(&c1);
        mp6_save_common_to_be(img, &c1);
        mp6_save_common_from_be(&c2, img);
        cmp_common(&c1, &c2);
        mp6_save_common_to_be(img2, &c2);
        CHK(memcmp(img, img2, 0x5D0) == 0, "synthetic common image stability (iter %d)", iter);

        fill_system(&sy1);
        mp6_save_system_to_be(simg, &sy1);
        mp6_save_system_from_be(&sy2, simg);
        cmp_system(&sy1, &sy2);
        mp6_save_system_to_be(simg2, &sy2);
        CHK(memcmp(simg, simg2, 0x2C0) == 0, "synthetic system image stability (iter %d)", iter);

        fill_player(&pl1);
        mp6_save_player_to_be(pimg, &pl1);
        mp6_save_player_from_be(&pl2, pimg);
        cmp_player(&pl1, &pl2);
        mp6_save_player_to_be(pimg2, &pl2);
        CHK(memcmp(pimg, pimg2, 0x108) == 0, "synthetic player image stability (iter %d)", iter);
    }

    /* ---- 3) spot-check absolute on-card offsets in the composed image ---- */
    pat_state = 999;
    fill_player(&pl1);
    mp6_save_player_to_be(pimg, &pl1);
    CHK((u8)pimg[0x03] == (u8)pl1.handicap, "player handicap not at on-card 0x03");
    CHK((s16)((pimg[0x30] << 8) | pimg[0x31]) == pl1.star, "player star not at on-card 0x30");
    CHK((s16)((pimg[0x1C] << 8) | pimg[0x1D]) == pl1.coin, "player coin not at on-card 0x1C");
    fill_common(&c1);
    mp6_save_common_to_be(img, &c1);
    CHK((u16)((img[0x59A] << 8) | img[0x59B]) == c1.bankStar, "common bankStar not at on-card 0x59A");
    fill_system(&sy1);
    mp6_save_system_to_be(simg, &sy1);
    CHK((u16)((simg[0x3C] << 8) | simg[0x3D]) == sy1.bankCoin, "system bankCoin not at on-card 0x3C");

    if (fails == 0) {
        printf("save_endian round-trip: ALL TESTS PASS\n");
        return 0;
    }
    printf("save_endian round-trip: %d FAILURE(S)\n", fails);
    return 1;
}
