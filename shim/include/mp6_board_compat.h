#ifndef MP6_BOARD_COMPAT_H
#define MP6_BOARD_COMPAT_H

/* Exact declarations that the original REL/DOL build obtained through the
 * kernel jump table or translation-unit ordering.  Native clang must see the
 * real host ABI before each call; an implicit declaration is not compatible
 * with 64-bit hosts or float return values. */
#ifdef __cplusplus
extern "C" {
#endif

double __fabs(double value);
float __fabsf(float value);

/* The native force-include prelude has already supplied Dolphin scalar types.
 * Spell ID aliases by their exact underlying ABI here so this header does not
 * pull host <math.h> in before board/player.c and board/camera.c establish
 * their original MSL include guards. */
void Hu3DModelShadowMapReset(s16 modelId);
s16 GWPlayerStarGet(s32 playerNo);

void mbCapMasuObjInit(void);
BOOL mbDiceNumStopCheck(int playerNo);
void mbPauseEnableReset(void);
int mbStarDispPlayerCreate(int playerNo, int num);
BOOL mbStarDispCheck(int playerNo);
int mbCoinAddDispExec(int playerNo, int coinNum, BOOL dispF, BOOL fastF);
s16 mbMasuFind_AttrIdGet(s16 id, u16 attr);

#ifdef __cplusplus
}
#endif

#endif /* MP6_BOARD_COMPAT_H */
