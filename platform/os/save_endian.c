/* MP6 native port -- field-wise big-endian marshal for the persisted
 * save-box structs GW_COMMON / GW_SYSTEM / GW_PLAYER.
 * See shim/include/mp6_save_endian.h for the design contract
 * (why field-wise and not an in-place byte swapper: MWCC bitfields
 * allocate MSB-first, native LE compilers LSB-first, so bitfield units
 * need BIT-level remapping that no byte swap can express).
 *
 * Method (deliberately mechanical, transcribed with include/game/
 * gamework.h open):
 *   - GW_COMMON / GW_SYSTEM: every non-bitfield member's buffer offset is
 *     offsetof(...) on the NATIVE struct -- valid only because the native
 *     (zig cc, x86_64-windows-gnu) layout provably equals the MWCC/GC
 *     layout for these two structs. "Provably": the _Static_asserts below
 *     pin EVERY non-bitfield member to the GC offset documented in
 *     gamework.h's own //0xNN comments (independently re-derived by hand
 *     for this file), plus the struct sizes that the SAVE_BOX_* layout
 *     macros and the already-validating box checksum depend on. A future
 *     compiler/ABI change that shifts anything fails the build loudly
 *     instead of writing silently-corrupt cards.
 *   - GW_PLAYER: offsetof is NOT usable -- the native interior layout
 *     genuinely diverges from the on-card one (MWCC lets `s8 handicap`
 *     live inside the 0x02 u16 bitfield container's unused tail byte;
 *     clang gives that container both bytes, shifting everything until
 *     mgScore's 4-alignment re-syncs at 0x2C -- found BY these asserts
 *     firing on first compile). Its buffer offsets are the explicit
 *     GWP_*_OFS on-card table below; only sizeof (the box stride) is
 *     asserted, which is all the surrounding SAVE_BOX layout consumes.
 *   - Bitfield units can't be offsetof'd; their offsets are constants
 *     bracketed by asserted neighbors, and each field's shift/mask
 *     transcribes MWCC's MSB-first allocation (declaration order from the
 *     top bit down), cross-checked against gamework.h's per-field //bit
 *     comments. The one comment TYPO found (veryHardUnlock and
 *     m562VeryHardUnlock both annotated "Bit 4") resolves by allocation
 *     order to bits 5 and 4 respectively.
 *   - u8/char arrays copy as bytes; nested GW_DECA_SCORE recurses the same
 *     way; struct padding bytes are zero-filled on BOTH directions (the
 *     console memcpy'd whatever RAM garbage sat in the padding -- every
 *     reader on every platform treats those bytes as don't-care, and
 *     deterministic zeros beat garbage for byte-diffing test saves).
 *
 * Round-trip contract (verified by tools/save_endian_selftest.c against a
 * real Dolphin .gci AND a synthetic all-fields-distinct pattern): from_be
 * followed by to_be reproduces every non-padding byte exactly; to_be
 * followed by from_be reproduces every field value exactly.
 */
#include <stddef.h>
#include <string.h>

#include "dolphin/types.h"
#include "game/gamework.h"
#include "mp6_save_endian.h"

/* ---------------------------------------------------------------- *
 *  layout proof: native offsets/sizes == GC on-card offsets/sizes  *
 * ---------------------------------------------------------------- */
#define MP6_SL_ASSERT(expr) _Static_assert(expr, "save_endian: native struct layout diverged from the GC on-card layout: " #expr)

/* GW_DECA_SCORE -- 0x18 bytes */
MP6_SL_ASSERT(offsetof(GW_DECA_SCORE, charNo) == 0x00);
MP6_SL_ASSERT(offsetof(GW_DECA_SCORE, mgScore) == 0x02);
MP6_SL_ASSERT(offsetof(GW_DECA_SCORE, finalScore) == 0x16);
MP6_SL_ASSERT(sizeof(GW_DECA_SCORE) == 0x18);

/* GW_COMMON -- 0x5D0 bytes */
MP6_SL_ASSERT(offsetof(GW_COMMON, magic) == 0x00);
MP6_SL_ASSERT(offsetof(GW_COMMON, unk4) == 0x04);
/* option bitfield byte at 0x06 (languageNo/outputMode/vibrateF/mic) */
MP6_SL_ASSERT(offsetof(GW_COMMON, time) == 0x08);
MP6_SL_ASSERT(offsetof(GW_COMMON, name) == 0x10);
MP6_SL_ASSERT(offsetof(GW_COMMON, mgUnlock) == 0x24);
MP6_SL_ASSERT(offsetof(GW_COMMON, record) == 0x34);
MP6_SL_ASSERT(offsetof(GW_COMMON, charPlayNum) == 0x54);
MP6_SL_ASSERT(offsetof(GW_COMMON, boardPlayNum) == 0x188);
MP6_SL_ASSERT(offsetof(GW_COMMON, boardMaxStar) == 0x19E);
MP6_SL_ASSERT(offsetof(GW_COMMON, boardMaxCoin) == 0x2D2);
MP6_SL_ASSERT(offsetof(GW_COMMON, singleMgWinNum) == 0x406);
MP6_SL_ASSERT(offsetof(GW_COMMON, singleBoardPlayNum) == 0x40A);
MP6_SL_ASSERT(offsetof(GW_COMMON, singleBoardFlag) == 0x410);
/* flag/config bitfield bytes at 0x428..0x42D */
MP6_SL_ASSERT(offsetof(GW_COMMON, lastBoard) == 0x42E);
MP6_SL_ASSERT(offsetof(GW_COMMON, decaScore) == 0x430);
MP6_SL_ASSERT(offsetof(GW_COMMON, decaMgRecord) == 0x520);
MP6_SL_ASSERT(offsetof(GW_COMMON, renshoMgRecordNum) == 0x534);
MP6_SL_ASSERT(offsetof(GW_COMMON, renshoMgRecord) == 0x535);
MP6_SL_ASSERT(offsetof(GW_COMMON, bankStar) == 0x59A);
MP6_SL_ASSERT(offsetof(GW_COMMON, bankStarAward) == 0x59C);
MP6_SL_ASSERT(offsetof(GW_COMMON, bankFlag) == 0x5A0);
MP6_SL_ASSERT(offsetof(GW_COMMON, miracleBookFlag) == 0x5A8);
MP6_SL_ASSERT(offsetof(GW_COMMON, mikeActRecord) == 0x5B0);
MP6_SL_ASSERT(offsetof(GW_COMMON, unk5BC) == 0x5BC);
MP6_SL_ASSERT(offsetof(GW_COMMON, singlePrizeFlag) == 0x5C0);
MP6_SL_ASSERT(offsetof(GW_COMMON, unk5C8) == 0x5C8);
MP6_SL_ASSERT(sizeof(GW_COMMON) == 0x5D0);

/* GW_SYSTEM -- 0x2C0 bytes */
/* board-game-type bitfield byte at 0x00 (partyF/tagF) */
MP6_SL_ASSERT(offsetof(GW_SYSTEM, storyComDif) == 0x01);
/* board-menu-config u16 bitfield unit at 0x02..0x03 */
MP6_SL_ASSERT(offsetof(GW_SYSTEM, turnNo) == 0x04);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, turnMax) == 0x05);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, starFlag) == 0x06);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, starTotal) == 0x07);
/* starPos/boardNo bitfield byte at 0x08 */
MP6_SL_ASSERT(offsetof(GW_SYSTEM, last5Effect) == 0x09);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, turnPlayerNo) == 0x0A);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, storyCharBit) == 0x0B);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, storyChar) == 0x0C);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, hiddenBlockMasuId) == 0x0E);
/* nextTime/curTime bitfield byte at 0x10 */
MP6_SL_ASSERT(offsetof(GW_SYSTEM, timeTurn) == 0x11);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, timeTurnMax) == 0x12);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, boardWork) == 0x14);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, comKeyDelay) == 0x34);
/* mgEvent bitfield byte at 0x35 */
MP6_SL_ASSERT(offsetof(GW_SYSTEM, playerMode) == 0x36);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, mgNo) == 0x38);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, subGameNo) == 0x3A);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, bankCoin) == 0x3C);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, masuCapsule) == 0x3E);
MP6_SL_ASSERT(offsetof(GW_SYSTEM, flag) == 0x23E);
MP6_SL_ASSERT(sizeof(GW_SYSTEM) == 0x2C0);

/* GW_PLAYER -- 0x108 bytes (0x106 data + 2 tail pad to the s32 align).
 *
 * !! NATIVE INTERIOR LAYOUT DIVERGES FROM THE ON-CARD LAYOUT !!
 * MWCC packs the 6-bit `u16 team..metalF` bitfield run into the single
 * byte 0x02 and places the next plain member INSIDE the u16 container's
 * unused tail byte (handicap at on-card 0x03); clang/x86_64-windows-gnu
 * gives that run its full 2-byte container instead, so natively handicap
 * lands at 0x04 (verified by probe: +1 through capsule, then the second
 * bitfield unit shifts to 0x0A..0x0B making the drift +2, until mgScore's
 * s32 4-alignment re-syncs both layouts at 0x2C; starGraph/coinGraph/
 * sizeof match again). offsetof() is therefore NOT usable as the buffer
 * offset for this struct -- the GWP_* table below is the on-card layout,
 * transcribed from gamework.h's //0xNN comments and cross-checked against
 * the SAVE_BOX_SIZE arithmetic (0x5D0+0x2C0+4*0x108 -> the 0xCB2 box the
 * user's validating Dolphin save carries). Only the size (the box stride)
 * must -- and does -- agree natively: */
MP6_SL_ASSERT(sizeof(GW_PLAYER) == 0x108);

/* GW_PLAYER on-card (GC/MWCC) field offsets */
#define GWP_HANDICAP_OFS 0x03
#define GWP_PADNO_OFS 0x04
#define GWP_CAPSULE_OFS 0x05
#define GWP_MOVENUM_OFS 0x0A
#define GWP_MASUID_OFS 0x0C
#define GWP_MASUIDPREV_OFS 0x0E
#define GWP_MASUIDNEXT_OFS 0x10
#define GWP_CAPSULEUSE_OFS 0x12
#define GWP_PLUSMASUNUM_OFS 0x14
#define GWP_MINUSMASUNUM_OFS 0x15
#define GWP_CAPSULEMASUNUM_OFS 0x16
#define GWP_HATENAMASUNUM_OFS 0x17
#define GWP_KOOPAMASUNUM_OFS 0x18
#define GWP_MIRACLEMASUNUM_OFS 0x19
#define GWP_KETTOUMASUNUM_OFS 0x1A
#define GWP_DONKEYMASUNUM_OFS 0x1B
#define GWP_COIN_OFS 0x1C
#define GWP_COINTOTALMG_OFS 0x1E
#define GWP_COINTOTAL_OFS 0x20
#define GWP_COINMAX_OFS 0x22
#define GWP_COINBATTLE_OFS 0x24
#define GWP_MGCOIN_OFS 0x26
#define GWP_MGCOINBONUS_OFS 0x28
#define GWP_MGSCORE_OFS 0x2C
#define GWP_STAR_OFS 0x30
#define GWP_STARMAX_OFS 0x32
#define GWP_CAPSULEUSENUM_OFS 0x34
#define GWP_STARGRAPH_OFS 0x36
#define GWP_COINGRAPH_OFS 0x9E

/* array-extent sanity (loop bounds below bake these in) */
MP6_SL_ASSERT(GW_PLAYER_MAX == 4);
MP6_SL_ASSERT(GW_RECORD_MAX == 8);
MP6_SL_ASSERT(GW_BOARD_MAX == 11);
MP6_SL_ASSERT(GW_CHARA_MAX == 14);
MP6_SL_ASSERT(GW_DECA_SCORE_MAX == 10);
MP6_SL_ASSERT(GW_RENSHO_MG_MAX == 100);
MP6_SL_ASSERT(GW_BOARD_WORK_SIZE == 32);
MP6_SL_ASSERT(GW_PLAYER_GRAPH_SIZE == 52);
MP6_SL_ASSERT(sizeof(((GW_COMMON *)0)->name) == 0x11);
MP6_SL_ASSERT(sizeof(((GW_COMMON *)0)->unk5C8) == 0x08);
MP6_SL_ASSERT(sizeof(((GW_COMMON *)0)->singleMgWinNum) == 0x04);
MP6_SL_ASSERT(sizeof(((GW_SYSTEM *)0)->flag) == 0x80);
MP6_SL_ASSERT(sizeof(((GW_PLAYER *)0)->capsule) == 0x03);

/* bitfield unit offsets (not offsetof-able; bracketed by asserted
 * neighbors above) */
#define GWC_OPT_OFS 0x06    /* languageNo/outputMode/vibrateF/mic          */
#define GWC_FLAG_OFS 0x428  /* saveEnableF .. viewEnding                   */
#define GWC_PCFG1_OFS 0x429 /* partyMgInstDispF/partyMgComDispF/partyMgPack/partyMessSpeed */
#define GWC_PCFG2_OFS 0x42A /* partySaveMode/storyMgInstDispF/storyMgComDispF/storyMgPack  */
#define GWC_PCFG3_OFS 0x42B /* storyMessSpeed/storySaveMode                */
#define GWC_GCFG1_OFS 0x42C /* confTurnNum/confBonusStar                   */
#define GWC_GCFG2_OFS 0x42D /* confTag/confSingleDiff                      */
#define GWS_TYPE_OFS 0x00   /* partyF/tagF                                 */
#define GWS_MENU_OFS 0x02   /* u16: bonusStarF..saveMode                   */
#define GWS_STAR_OFS 0x08   /* starPos/boardNo                             */
#define GWS_TIME_OFS 0x10   /* nextTime/curTime                            */
#define GWS_MGEV_OFS 0x35   /* mgEvent                                     */
#define GWP_STAT0_OFS 0x00  /* u16: comDif..diceMode                       */
#define GWP_STAT1_OFS 0x02  /* byte: team..metalF (u16 unit, 6 bits, all in byte 0x02) */
#define GWP_STAT2_OFS 0x08  /* u16: statusColor..teamBackup                */

/* ------------------------- BE byte access ------------------------- */
static u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u64 rd64(const u8 *p) { return ((u64)rd32(p) << 32) | rd32(p + 4); }
static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static void wr64(u8 *p, u64 v)
{
    wr32(p, (u32)(v >> 32));
    wr32(p + 4, (u32)v);
}

/* =================================================================== *
 *  GW_COMMON                                                          *
 * =================================================================== */
void mp6_save_common_from_be(GW_COMMON *dst, const void *srcv)
{
    const u8 *s = (const u8 *)srcv;
    u8 b;
    int i, j;

    memset(dst, 0, sizeof(GW_COMMON));
    memcpy(dst->magic, s + offsetof(GW_COMMON, magic), sizeof(dst->magic));
    dst->unk4 = rd16(s + offsetof(GW_COMMON, unk4));
    b = s[GWC_OPT_OFS];
    dst->languageNo = (b >> 5) & 0x7; /* bits 7-5 */
    dst->outputMode = (b >> 3) & 0x3; /* bits 4-3 */
    dst->vibrateF = (b >> 2) & 0x1;   /* bit 2    */
    dst->mic = b & 0x3;               /* bits 1-0 */
    dst->time = (OSTime)rd64(s + offsetof(GW_COMMON, time));
    memcpy(dst->name, s + offsetof(GW_COMMON, name), sizeof(dst->name));
    for (i = 0; i < 4; i++) {
        dst->mgUnlock[i] = rd32(s + offsetof(GW_COMMON, mgUnlock) + 4 * i);
    }
    for (i = 0; i < GW_RECORD_MAX; i++) {
        dst->record[i] = rd32(s + offsetof(GW_COMMON, record) + 4 * i);
    }
    for (i = 0; i < GW_BOARD_MAX; i++) {
        for (j = 0; j < GW_CHARA_MAX; j++) {
            dst->charPlayNum[i][j] = rd16(s + offsetof(GW_COMMON, charPlayNum) + 2 * (i * GW_CHARA_MAX + j));
            dst->boardMaxStar[i][j] = rd16(s + offsetof(GW_COMMON, boardMaxStar) + 2 * (i * GW_CHARA_MAX + j));
            dst->boardMaxCoin[i][j] = rd16(s + offsetof(GW_COMMON, boardMaxCoin) + 2 * (i * GW_CHARA_MAX + j));
        }
        dst->boardPlayNum[i] = rd16(s + offsetof(GW_COMMON, boardPlayNum) + 2 * i);
    }
    memcpy(dst->singleMgWinNum, s + offsetof(GW_COMMON, singleMgWinNum), sizeof(dst->singleMgWinNum));
    memcpy(dst->singleBoardPlayNum, s + offsetof(GW_COMMON, singleBoardPlayNum), sizeof(dst->singleBoardPlayNum));
    for (i = 0; i < 3; i++) {
        dst->singleBoardFlag[i] = rd64(s + offsetof(GW_COMMON, singleBoardFlag) + 8 * i);
    }
    b = s[GWC_FLAG_OFS];
    dst->saveEnableF = (b >> 7) & 1;       /* bit 7 */
    dst->map7Unlock = (b >> 6) & 1;        /* bit 6 */
    dst->veryHardUnlock = (b >> 5) & 1;    /* bit 5 (gamework.h comment "Bit 4" is a typo; MSB-first order puts it here) */
    dst->m562VeryHardUnlock = (b >> 4) & 1; /* bit 4 */
    /* bit 3: the anonymous `u8 : 1` pad bit -- preserved only as zero     */
    dst->unkFlag4 = (b >> 2) & 1;          /* bit 2 */
    dst->viewOpening = (b >> 1) & 1;       /* bit 1 */
    dst->viewEnding = b & 1;               /* bit 0 */
    b = s[GWC_PCFG1_OFS];
    dst->partyMgInstDispF = (b >> 7) & 1;  /* bit 7    */
    dst->partyMgComDispF = (b >> 6) & 1;   /* bit 6    */
    dst->partyMgPack = (b >> 3) & 0x7;     /* bits 5-3 */
    dst->partyMessSpeed = (b >> 1) & 0x3;  /* bits 2-1 */
    b = s[GWC_PCFG2_OFS];
    dst->partySaveMode = (b >> 6) & 0x3;   /* bits 7-6 */
    dst->storyMgInstDispF = (b >> 5) & 1;  /* bit 5    */
    dst->storyMgComDispF = (b >> 4) & 1;   /* bit 4    */
    dst->storyMgPack = (b >> 1) & 0x7;     /* bits 3-1 */
    b = s[GWC_PCFG3_OFS];
    dst->storyMessSpeed = (b >> 6) & 0x3;  /* bits 7-6 */
    dst->storySaveMode = (b >> 4) & 0x3;   /* bits 5-4 */
    b = s[GWC_GCFG1_OFS];
    dst->confTurnNum = (b >> 1) & 0x7F;    /* bits 7-1 */
    dst->confBonusStar = b & 1;            /* bit 0    */
    b = s[GWC_GCFG2_OFS];
    dst->confTag = (b >> 7) & 1;           /* bit 7    */
    dst->confSingleDiff = (b >> 5) & 0x3;  /* bits 6-5 */
    dst->lastBoard = (s8)s[offsetof(GW_COMMON, lastBoard)];
    for (i = 0; i < GW_DECA_SCORE_MAX; i++) {
        const u8 *ds = s + offsetof(GW_COMMON, decaScore) + i * sizeof(GW_DECA_SCORE);
        dst->decaScore[i].charNo = (s8)ds[offsetof(GW_DECA_SCORE, charNo)];
        for (j = 0; j < 10; j++) {
            dst->decaScore[i].mgScore[j] = rd16(ds + offsetof(GW_DECA_SCORE, mgScore) + 2 * j);
        }
        dst->decaScore[i].finalScore = rd16(ds + offsetof(GW_DECA_SCORE, finalScore));
    }
    for (i = 0; i < 10; i++) {
        dst->decaMgRecord[i] = rd16(s + offsetof(GW_COMMON, decaMgRecord) + 2 * i);
    }
    dst->renshoMgRecordNum = s[offsetof(GW_COMMON, renshoMgRecordNum)];
    memcpy(dst->renshoMgRecord, s + offsetof(GW_COMMON, renshoMgRecord), sizeof(dst->renshoMgRecord));
    dst->bankStar = rd16(s + offsetof(GW_COMMON, bankStar));
    dst->bankStarAward = rd16(s + offsetof(GW_COMMON, bankStarAward));
    for (i = 0; i < 2; i++) {
        dst->bankFlag[i] = rd32(s + offsetof(GW_COMMON, bankFlag) + 4 * i);
        dst->miracleBookFlag[i] = rd32(s + offsetof(GW_COMMON, miracleBookFlag) + 4 * i);
        dst->singlePrizeFlag[i] = rd32(s + offsetof(GW_COMMON, singlePrizeFlag) + 4 * i);
    }
    for (i = 0; i < 3; i++) {
        dst->mikeActRecord[i] = rd32(s + offsetof(GW_COMMON, mikeActRecord) + 4 * i);
    }
    dst->unk5BC = s[offsetof(GW_COMMON, unk5BC)];
    memcpy(dst->unk5C8, s + offsetof(GW_COMMON, unk5C8), sizeof(dst->unk5C8));
}

void mp6_save_common_to_be(void *dstv, const GW_COMMON *src)
{
    u8 *d = (u8 *)dstv;
    int i, j;

    memset(d, 0, sizeof(GW_COMMON));
    memcpy(d + offsetof(GW_COMMON, magic), src->magic, sizeof(src->magic));
    wr16(d + offsetof(GW_COMMON, unk4), src->unk4);
    d[GWC_OPT_OFS] = (u8)(((src->languageNo & 0x7) << 5) | ((src->outputMode & 0x3) << 3) |
                          ((src->vibrateF & 0x1) << 2) | (src->mic & 0x3));
    wr64(d + offsetof(GW_COMMON, time), (u64)src->time);
    memcpy(d + offsetof(GW_COMMON, name), src->name, sizeof(src->name));
    for (i = 0; i < 4; i++) {
        wr32(d + offsetof(GW_COMMON, mgUnlock) + 4 * i, src->mgUnlock[i]);
    }
    for (i = 0; i < GW_RECORD_MAX; i++) {
        wr32(d + offsetof(GW_COMMON, record) + 4 * i, src->record[i]);
    }
    for (i = 0; i < GW_BOARD_MAX; i++) {
        for (j = 0; j < GW_CHARA_MAX; j++) {
            wr16(d + offsetof(GW_COMMON, charPlayNum) + 2 * (i * GW_CHARA_MAX + j), src->charPlayNum[i][j]);
            wr16(d + offsetof(GW_COMMON, boardMaxStar) + 2 * (i * GW_CHARA_MAX + j), src->boardMaxStar[i][j]);
            wr16(d + offsetof(GW_COMMON, boardMaxCoin) + 2 * (i * GW_CHARA_MAX + j), src->boardMaxCoin[i][j]);
        }
        wr16(d + offsetof(GW_COMMON, boardPlayNum) + 2 * i, src->boardPlayNum[i]);
    }
    memcpy(d + offsetof(GW_COMMON, singleMgWinNum), src->singleMgWinNum, sizeof(src->singleMgWinNum));
    memcpy(d + offsetof(GW_COMMON, singleBoardPlayNum), src->singleBoardPlayNum, sizeof(src->singleBoardPlayNum));
    for (i = 0; i < 3; i++) {
        wr64(d + offsetof(GW_COMMON, singleBoardFlag) + 8 * i, src->singleBoardFlag[i]);
    }
    d[GWC_FLAG_OFS] = (u8)(((src->saveEnableF & 1) << 7) | ((src->map7Unlock & 1) << 6) |
                           ((src->veryHardUnlock & 1) << 5) | ((src->m562VeryHardUnlock & 1) << 4) |
                           ((src->unkFlag4 & 1) << 2) | ((src->viewOpening & 1) << 1) |
                           (src->viewEnding & 1));
    d[GWC_PCFG1_OFS] = (u8)(((src->partyMgInstDispF & 1) << 7) | ((src->partyMgComDispF & 1) << 6) |
                            ((src->partyMgPack & 0x7) << 3) | ((src->partyMessSpeed & 0x3) << 1));
    d[GWC_PCFG2_OFS] = (u8)(((src->partySaveMode & 0x3) << 6) | ((src->storyMgInstDispF & 1) << 5) |
                            ((src->storyMgComDispF & 1) << 4) | ((src->storyMgPack & 0x7) << 1));
    d[GWC_PCFG3_OFS] = (u8)(((src->storyMessSpeed & 0x3) << 6) | ((src->storySaveMode & 0x3) << 4));
    d[GWC_GCFG1_OFS] = (u8)(((src->confTurnNum & 0x7F) << 1) | (src->confBonusStar & 1));
    d[GWC_GCFG2_OFS] = (u8)(((src->confTag & 1) << 7) | ((src->confSingleDiff & 0x3) << 5));
    d[offsetof(GW_COMMON, lastBoard)] = (u8)src->lastBoard;
    for (i = 0; i < GW_DECA_SCORE_MAX; i++) {
        u8 *ds = d + offsetof(GW_COMMON, decaScore) + i * sizeof(GW_DECA_SCORE);
        ds[offsetof(GW_DECA_SCORE, charNo)] = (u8)src->decaScore[i].charNo;
        for (j = 0; j < 10; j++) {
            wr16(ds + offsetof(GW_DECA_SCORE, mgScore) + 2 * j, src->decaScore[i].mgScore[j]);
        }
        wr16(ds + offsetof(GW_DECA_SCORE, finalScore), src->decaScore[i].finalScore);
    }
    for (i = 0; i < 10; i++) {
        wr16(d + offsetof(GW_COMMON, decaMgRecord) + 2 * i, src->decaMgRecord[i]);
    }
    d[offsetof(GW_COMMON, renshoMgRecordNum)] = src->renshoMgRecordNum;
    memcpy(d + offsetof(GW_COMMON, renshoMgRecord), src->renshoMgRecord, sizeof(src->renshoMgRecord));
    wr16(d + offsetof(GW_COMMON, bankStar), src->bankStar);
    wr16(d + offsetof(GW_COMMON, bankStarAward), src->bankStarAward);
    for (i = 0; i < 2; i++) {
        wr32(d + offsetof(GW_COMMON, bankFlag) + 4 * i, src->bankFlag[i]);
        wr32(d + offsetof(GW_COMMON, miracleBookFlag) + 4 * i, src->miracleBookFlag[i]);
        wr32(d + offsetof(GW_COMMON, singlePrizeFlag) + 4 * i, src->singlePrizeFlag[i]);
    }
    for (i = 0; i < 3; i++) {
        wr32(d + offsetof(GW_COMMON, mikeActRecord) + 4 * i, src->mikeActRecord[i]);
    }
    d[offsetof(GW_COMMON, unk5BC)] = src->unk5BC;
    memcpy(d + offsetof(GW_COMMON, unk5C8), src->unk5C8, sizeof(src->unk5C8));
}

/* =================================================================== *
 *  GW_SYSTEM                                                          *
 * =================================================================== */
void mp6_save_system_from_be(GW_SYSTEM *dst, const void *srcv)
{
    const u8 *s = (const u8 *)srcv;
    u16 v;
    u8 b;
    int i;

    memset(dst, 0, sizeof(GW_SYSTEM));
    b = s[GWS_TYPE_OFS];
    dst->partyF = (b >> 7) & 1; /* bit 7 */
    dst->tagF = (b >> 6) & 1;   /* bit 6 */
    dst->storyComDif = s[offsetof(GW_SYSTEM, storyComDif)];
    v = rd16(s + GWS_MENU_OFS);
    dst->bonusStarF = (v >> 15) & 1;  /* 0x02 bit 7  */
    dst->mgInstDispF = (v >> 14) & 1; /* 0x02 bit 6  */
    dst->mgComDispF = (v >> 13) & 1;  /* 0x02 bit 5  */
    dst->mgPack = (v >> 10) & 0x7;    /* 0x02 bits 4-2 */
    dst->messSpeed = (v >> 8) & 0x3;  /* 0x02 bits 1-0 */
    dst->saveMode = (v >> 6) & 0x3;   /* 0x03 bits 7-6 */
    dst->turnNo = s[offsetof(GW_SYSTEM, turnNo)];
    dst->turnMax = s[offsetof(GW_SYSTEM, turnMax)];
    dst->starFlag = s[offsetof(GW_SYSTEM, starFlag)];
    dst->starTotal = s[offsetof(GW_SYSTEM, starTotal)];
    b = s[GWS_STAR_OFS];
    dst->starPos = (b >> 5) & 0x7; /* bits 7-5 */
    dst->boardNo = b & 0x1F;       /* bits 4-0 */
    dst->last5Effect = (s8)s[offsetof(GW_SYSTEM, last5Effect)];
    dst->turnPlayerNo = (s8)s[offsetof(GW_SYSTEM, turnPlayerNo)];
    dst->storyCharBit = s[offsetof(GW_SYSTEM, storyCharBit)];
    dst->storyChar = (s8)s[offsetof(GW_SYSTEM, storyChar)];
    dst->hiddenBlockMasuId = (s16)rd16(s + offsetof(GW_SYSTEM, hiddenBlockMasuId));
    b = s[GWS_TIME_OFS];
    dst->nextTime = (b >> 7) & 1; /* bit 7 */
    dst->curTime = (b >> 6) & 1;  /* bit 6 */
    dst->timeTurn = s[offsetof(GW_SYSTEM, timeTurn)];
    dst->timeTurnMax = s[offsetof(GW_SYSTEM, timeTurnMax)];
    for (i = 0; i < GW_BOARD_WORK_SIZE / 4; i++) {
        dst->boardWork[i] = rd32(s + offsetof(GW_SYSTEM, boardWork) + 4 * i);
    }
    dst->comKeyDelay = s[offsetof(GW_SYSTEM, comKeyDelay)];
    dst->mgEvent = (s[GWS_MGEV_OFS] >> 4) & 0xF; /* bits 7-4 */
    dst->playerMode = (s8)s[offsetof(GW_SYSTEM, playerMode)];
    dst->mgNo = rd16(s + offsetof(GW_SYSTEM, mgNo));
    dst->subGameNo = (s16)rd16(s + offsetof(GW_SYSTEM, subGameNo));
    dst->bankCoin = rd16(s + offsetof(GW_SYSTEM, bankCoin));
    for (i = 0; i < 256; i++) {
        dst->masuCapsule[i] = (s16)rd16(s + offsetof(GW_SYSTEM, masuCapsule) + 2 * i);
    }
    memcpy(dst->flag, s + offsetof(GW_SYSTEM, flag), sizeof(dst->flag));
}

void mp6_save_system_to_be(void *dstv, const GW_SYSTEM *src)
{
    u8 *d = (u8 *)dstv;
    int i;

    memset(d, 0, sizeof(GW_SYSTEM));
    d[GWS_TYPE_OFS] = (u8)(((src->partyF & 1) << 7) | ((src->tagF & 1) << 6));
    d[offsetof(GW_SYSTEM, storyComDif)] = src->storyComDif;
    wr16(d + GWS_MENU_OFS,
         (u16)(((src->bonusStarF & 1) << 15) | ((src->mgInstDispF & 1) << 14) |
               ((src->mgComDispF & 1) << 13) | ((src->mgPack & 0x7) << 10) |
               ((src->messSpeed & 0x3) << 8) | ((src->saveMode & 0x3) << 6)));
    d[offsetof(GW_SYSTEM, turnNo)] = src->turnNo;
    d[offsetof(GW_SYSTEM, turnMax)] = src->turnMax;
    d[offsetof(GW_SYSTEM, starFlag)] = src->starFlag;
    d[offsetof(GW_SYSTEM, starTotal)] = src->starTotal;
    d[GWS_STAR_OFS] = (u8)(((src->starPos & 0x7) << 5) | (src->boardNo & 0x1F));
    d[offsetof(GW_SYSTEM, last5Effect)] = (u8)src->last5Effect;
    d[offsetof(GW_SYSTEM, turnPlayerNo)] = (u8)src->turnPlayerNo;
    d[offsetof(GW_SYSTEM, storyCharBit)] = src->storyCharBit;
    d[offsetof(GW_SYSTEM, storyChar)] = (u8)src->storyChar;
    wr16(d + offsetof(GW_SYSTEM, hiddenBlockMasuId), (u16)src->hiddenBlockMasuId);
    d[GWS_TIME_OFS] = (u8)(((src->nextTime & 1) << 7) | ((src->curTime & 1) << 6));
    d[offsetof(GW_SYSTEM, timeTurn)] = src->timeTurn;
    d[offsetof(GW_SYSTEM, timeTurnMax)] = src->timeTurnMax;
    for (i = 0; i < GW_BOARD_WORK_SIZE / 4; i++) {
        wr32(d + offsetof(GW_SYSTEM, boardWork) + 4 * i, src->boardWork[i]);
    }
    d[offsetof(GW_SYSTEM, comKeyDelay)] = src->comKeyDelay;
    d[GWS_MGEV_OFS] = (u8)((src->mgEvent & 0xF) << 4);
    d[offsetof(GW_SYSTEM, playerMode)] = (u8)src->playerMode;
    wr16(d + offsetof(GW_SYSTEM, mgNo), src->mgNo);
    wr16(d + offsetof(GW_SYSTEM, subGameNo), (u16)src->subGameNo);
    wr16(d + offsetof(GW_SYSTEM, bankCoin), src->bankCoin);
    for (i = 0; i < 256; i++) {
        wr16(d + offsetof(GW_SYSTEM, masuCapsule) + 2 * i, (u16)src->masuCapsule[i]);
    }
    memcpy(d + offsetof(GW_SYSTEM, flag), src->flag, sizeof(src->flag));
}

/* =================================================================== *
 *  GW_PLAYER                                                          *
 * =================================================================== */
void mp6_save_player_from_be(GW_PLAYER *dst, const void *srcv)
{
    const u8 *s = (const u8 *)srcv;
    u16 v;
    u8 b;
    int i;

    memset(dst, 0, sizeof(GW_PLAYER));
    v = rd16(s + GWP_STAT0_OFS);
    dst->comDif = (v >> 14) & 0x3;  /* 0x00 bits 7-6 */
    dst->comF = (v >> 13) & 1;      /* 0x00 bit 5    */
    dst->charNo = (v >> 9) & 0xF;   /* 0x00 bits 4-1 */
    dst->autoSize = (v >> 7) & 0x3; /* 0x00 bit 0 + 0x01 bit 7 */
    dst->deadF = (v >> 6) & 1;      /* 0x01 bit 6    */
    dst->diceMode = v & 0x3F;       /* 0x01 bits 5-0 */
    b = s[GWP_STAT1_OFS];
    dst->team = (b >> 7) & 1;       /* bit 7    */
    dst->skipEventF = (b >> 6) & 1; /* bit 6    */
    dst->playerNo = (b >> 4) & 0x3; /* bits 5-4 */
    dst->biriQF = (b >> 3) & 1;     /* bit 3    */
    dst->metalF = (b >> 2) & 1;     /* bit 2    */
    dst->handicap = (s8)s[GWP_HANDICAP_OFS];
    dst->padNo = (s8)s[GWP_PADNO_OFS];
    memcpy(dst->capsule, s + GWP_CAPSULE_OFS, sizeof(dst->capsule));
    v = rd16(s + GWP_STAT2_OFS);
    dst->statusColor = (v >> 13) & 0x7; /* 0x08 bits 7-5 */
    dst->moveF = (v >> 12) & 1;         /* 0x08 bit 4    */
    dst->jumpF = (v >> 11) & 1;         /* 0x08 bit 3    */
    dst->dispLightF = (v >> 10) & 1;    /* 0x08 bit 2    */
    dst->orderNo = (v >> 8) & 0x3;      /* 0x08 bits 1-0 */
    dst->diceNum = (v >> 6) & 0x3;      /* 0x09 bits 7-6 */
    dst->rank = (v >> 4) & 0x3;         /* 0x09 bits 5-4 */
    dst->koopaSuit = (v >> 3) & 1;      /* 0x09 bit 3    */
    dst->teamBackup = (v >> 2) & 1;     /* 0x09 bit 2    */
    dst->moveNum = (s8)s[GWP_MOVENUM_OFS];
    dst->masuId = (s16)rd16(s + GWP_MASUID_OFS);
    dst->masuIdPrev = (s16)rd16(s + GWP_MASUIDPREV_OFS);
    dst->masuIdNext = (s16)rd16(s + GWP_MASUIDNEXT_OFS);
    dst->capsuleUse = (s16)rd16(s + GWP_CAPSULEUSE_OFS);
    dst->plusMasuNum = (s8)s[GWP_PLUSMASUNUM_OFS];
    dst->minusMasuNum = (s8)s[GWP_MINUSMASUNUM_OFS];
    dst->capsuleMasuNum = (s8)s[GWP_CAPSULEMASUNUM_OFS];
    dst->hatenaMasuNum = (s8)s[GWP_HATENAMASUNUM_OFS];
    dst->koopaMasuNum = (s8)s[GWP_KOOPAMASUNUM_OFS];
    dst->miracleMasuNum = (s8)s[GWP_MIRACLEMASUNUM_OFS];
    dst->kettouMasuNum = (s8)s[GWP_KETTOUMASUNUM_OFS];
    dst->donkeyMasuNum = (s8)s[GWP_DONKEYMASUNUM_OFS];
    dst->coin = (s16)rd16(s + GWP_COIN_OFS);
    dst->coinTotalMg = (s16)rd16(s + GWP_COINTOTALMG_OFS);
    dst->coinTotal = (s16)rd16(s + GWP_COINTOTAL_OFS);
    dst->coinMax = (s16)rd16(s + GWP_COINMAX_OFS);
    dst->coinBattle = (s16)rd16(s + GWP_COINBATTLE_OFS);
    dst->mgCoin = (s16)rd16(s + GWP_MGCOIN_OFS);
    dst->mgCoinBonus = (s16)rd16(s + GWP_MGCOINBONUS_OFS);
    dst->mgScore = (s32)rd32(s + GWP_MGSCORE_OFS);
    dst->star = (s16)rd16(s + GWP_STAR_OFS);
    dst->starMax = (s16)rd16(s + GWP_STARMAX_OFS);
    dst->capsuleUseNum = (s16)rd16(s + GWP_CAPSULEUSENUM_OFS);
    for (i = 0; i < GW_PLAYER_GRAPH_SIZE; i++) {
        dst->starGraph[i] = (s16)rd16(s + GWP_STARGRAPH_OFS + 2 * i);
        dst->coinGraph[i] = (s16)rd16(s + GWP_COINGRAPH_OFS + 2 * i);
    }
}

void mp6_save_player_to_be(void *dstv, const GW_PLAYER *src)
{
    u8 *d = (u8 *)dstv;
    int i;

    memset(d, 0, sizeof(GW_PLAYER));
    wr16(d + GWP_STAT0_OFS,
         (u16)(((src->comDif & 0x3) << 14) | ((src->comF & 1) << 13) |
               ((src->charNo & 0xF) << 9) | ((src->autoSize & 0x3) << 7) |
               ((src->deadF & 1) << 6) | (src->diceMode & 0x3F)));
    d[GWP_STAT1_OFS] = (u8)(((src->team & 1) << 7) | ((src->skipEventF & 1) << 6) |
                            ((src->playerNo & 0x3) << 4) | ((src->biriQF & 1) << 3) |
                            ((src->metalF & 1) << 2));
    d[GWP_HANDICAP_OFS] = (u8)src->handicap;
    d[GWP_PADNO_OFS] = (u8)src->padNo;
    memcpy(d + GWP_CAPSULE_OFS, src->capsule, sizeof(src->capsule));
    wr16(d + GWP_STAT2_OFS,
         (u16)(((src->statusColor & 0x7) << 13) | ((src->moveF & 1) << 12) |
               ((src->jumpF & 1) << 11) | ((src->dispLightF & 1) << 10) |
               ((src->orderNo & 0x3) << 8) | ((src->diceNum & 0x3) << 6) |
               ((src->rank & 0x3) << 4) | ((src->koopaSuit & 1) << 3) |
               ((src->teamBackup & 1) << 2)));
    d[GWP_MOVENUM_OFS] = (u8)src->moveNum;
    wr16(d + GWP_MASUID_OFS, (u16)src->masuId);
    wr16(d + GWP_MASUIDPREV_OFS, (u16)src->masuIdPrev);
    wr16(d + GWP_MASUIDNEXT_OFS, (u16)src->masuIdNext);
    wr16(d + GWP_CAPSULEUSE_OFS, (u16)src->capsuleUse);
    d[GWP_PLUSMASUNUM_OFS] = (u8)src->plusMasuNum;
    d[GWP_MINUSMASUNUM_OFS] = (u8)src->minusMasuNum;
    d[GWP_CAPSULEMASUNUM_OFS] = (u8)src->capsuleMasuNum;
    d[GWP_HATENAMASUNUM_OFS] = (u8)src->hatenaMasuNum;
    d[GWP_KOOPAMASUNUM_OFS] = (u8)src->koopaMasuNum;
    d[GWP_MIRACLEMASUNUM_OFS] = (u8)src->miracleMasuNum;
    d[GWP_KETTOUMASUNUM_OFS] = (u8)src->kettouMasuNum;
    d[GWP_DONKEYMASUNUM_OFS] = (u8)src->donkeyMasuNum;
    wr16(d + GWP_COIN_OFS, (u16)src->coin);
    wr16(d + GWP_COINTOTALMG_OFS, (u16)src->coinTotalMg);
    wr16(d + GWP_COINTOTAL_OFS, (u16)src->coinTotal);
    wr16(d + GWP_COINMAX_OFS, (u16)src->coinMax);
    wr16(d + GWP_COINBATTLE_OFS, (u16)src->coinBattle);
    wr16(d + GWP_MGCOIN_OFS, (u16)src->mgCoin);
    wr16(d + GWP_MGCOINBONUS_OFS, (u16)src->mgCoinBonus);
    wr32(d + GWP_MGSCORE_OFS, (u32)src->mgScore);
    wr16(d + GWP_STAR_OFS, (u16)src->star);
    wr16(d + GWP_STARMAX_OFS, (u16)src->starMax);
    wr16(d + GWP_CAPSULEUSENUM_OFS, (u16)src->capsuleUseNum);
    for (i = 0; i < GW_PLAYER_GRAPH_SIZE; i++) {
        wr16(d + GWP_STARGRAPH_OFS + 2 * i, (u16)src->starGraph[i]);
        wr16(d + GWP_COINGRAPH_OFS + 2 * i, (u16)src->coinGraph[i]);
    }
}
