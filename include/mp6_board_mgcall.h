#ifndef MP6_BOARD_MGCALL_H
#define MP6_BOARD_MGCALL_H

/* Prototypes board/mgcall.c reached through the kernel jump table or
 * translation-unit order on the console, with no decomp header in scope.
 * Force-included for that TU only (tools/build.py BOARD_NATIVE_ABI_FLAGS),
 * because other board TUs carry their own, slightly differently spelled
 * externs for some of these. Each signature is the definition's:
 *   mbSingleMgUnlockGet     src/board/single.c
 *   mbWipeDissolveFadeOut   src/board/wipe.c
 *   mbPos2Dto3D             src/board/math.c
 *   mbLanguageGet           src/board/telop.c
 *   mbCoinAddAllExec        src/board/coin.c
 *   HuMCProbe, HuMCMicGet   src/game/mic.c
 *   mbObj*Set, mbObjHookSet src/board/object.c
 * mbObjPosSet/mbObjScaleSet take floats: implicitly declared, the caller
 * would pass doubles and the model would be placed at garbage coordinates.
 * game/board/object.h has them but conflicts with game/board/model.h, which
 * mgcall.c includes (mbObjCreate/mbObjPosSetV are spelled differently). */
#include "dolphin/types.h"
#include "dolphin/mtx.h"
#include "humath.h"

typedef s16 MBMODELID; /* same as game/board/model.h */

void mbObjPosSet(MBMODELID modelId, float x, float y, float z);
void mbObjScaleSet(MBMODELID modelId, float x, float y, float z);
void mbObjMtxSet(MBMODELID modelId, Mtx *mtx);
void mbObjLayerSet(MBMODELID modelId, u8 layer);
void mbObjHookSet(MBMODELID modelId, char *objName, MBMODELID hookModelId);
void mbObjAlphaSet(MBMODELID modelId, u8 alpha);

BOOL mbSingleMgUnlockGet(int mgNo);
void mbWipeDissolveFadeOut(void);
void mbPos2Dto3D(HuVecF *screenPos, HuVecF *worldPos);
s32 mbLanguageGet(void);
void mbCoinAddAllExec(int player0Change, int player1Change, int player2Change,
                      int player3Change);
int HuMCProbe(u32 channel);
s32 HuMCMicGet(void);

#endif
