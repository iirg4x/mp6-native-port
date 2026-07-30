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

/* The Treetop capsule/shop/board surface that `abi_warning_audit --all
 * --target windows --deep` caught src/board/capevent.c, capselect.c and
 * config.c calling with NO declaration in scope at all.
 *
 * Every signature below is copied from the DEFINITION, cited, not inferred
 * from the call:
 *   mbWipeWait              src/board/wipe.c:154
 *   mbCapValidCheck         src/board/capsule.c:2472
 *   mbCapEffUseCreate       src/board/capsule.c:1105
 *   mbCapEffUseModeGet      src/board/capsule.c:1127
 *   mbCapSelectMasuInit     src/board/capsule.c:2299
 *   mbCapListRead           src/board/capsule.c:1754
 *   mbCapThrowColCreate     src/board/capsule.c:880
 *   mbCapThrowHookSet       src/board/capsule.c:1533
 *   mbev_CapBonusCoinCheck  src/board/capevent.c:1793 (used at :1768, i.e.
 *                           before its own definition in the same TU)
 *   mbev_ShopEnableSet      src/board/shopevent.c:103
 *   mbev_CapTeresaStealSet  src/board/capspecial.c:53
 *   mbBGReadWait            src/board/board.c:702
 *
 * They are declared HERE rather than patched into each caller because the
 * decomp has no header for them at all -- src/board/board.c, opening.c,
 * tutorial.c, pause.c and capmove.c each carry their own private `extern`
 * for the ones they use, which is exactly the translation-unit-ordering
 * habit this header exists to replace. Those private externs are left in
 * place and agree with these (checked: same parameter and return types), so
 * nothing is redeclared incompatibly.
 *
 * Why it matters beyond tidiness: an implicit declaration returns `int`. For
 * mbCapEffUseModeGet/mbCapThrowColCreate/mbCapValidCheck that happens to be
 * ABI-compatible, but the habit is not survivable in general -- the same
 * audit's src/board/config.c row had `fabs(mbCosDeg(...))` where the
 * implicit `int` return silently truncated a float and clang could see the
 * absolute-value bug that fell out of it. Declaring the real prototypes is
 * what turns that class from "compiles, misbehaves" into a compile error.
 *
 * The two hook parameter types are spelled inline as function-pointer types
 * because their typedefs (CAPSULE_THROW_HOOK in src/board/capsule.c:34,
 * TERESA_STEAL_BEGIN_HOOK/TERESA_STEAL_HOOK in src/board/capspecial.c:10-11)
 * are file-local to the defining TUs and are not reachable from a header.
 * They are ABI-identical to the typedefs, which is what a prototype needs. */
void mbWipeWait(void);
BOOL mbCapValidCheck(int capsuleNo);
BOOL mbCapEffUseCreate(int playerNo, int capsuleNo);
int mbCapEffUseModeGet(int playerNo);
void mbCapSelectMasuInit(void);
void mbCapListRead(void);
int mbCapThrowColCreate(int dataNum);
void mbCapThrowHookSet(void (*hook)(BOOL startF));
BOOL mbev_CapBonusCoinCheck(int playerNo);
void mbev_ShopEnableSet(BOOL enableF);
void mbev_CapTeresaStealSet(int mesId, int coinNum,
                            void (*beginHook)(int, int), int (*hook)(int));
void mbBGReadWait(s32 statId);

/* mbSinDeg/mbCosDeg (src/board/math.c:26,42) ARE declared by the decomp, in
 * include/game/board/guide.h:80-81 -- but that header cannot be force-included
 * into src/board/config.c, the TU that needs them: guide.h:55 declares `int
 * mbObjMotionShiftIDGet(int modelId)` while include/game/board/object.h:63
 * declares the same function as `int mbObjMotionShiftIDGet(MBMODELID)`, and
 * config.c already includes object.h. The two decomp headers are mutually
 * exclusive by accident, and no decomp TU includes both, so the conflict has
 * never had to be resolved. Declaring just these two here avoids reopening it.
 *
 * These are the ABI-critical pair of the whole implicit-declaration set: they
 * return FLOAT. config.c:375 (`work->scale = work->scaleStart =
 * work->scaleTarget = mbCosDeg(90.0f * weight)`) and config.c:440
 * (`flipScale = fabs(mbCosDeg(180.0f * weight))`) called them through an
 * implicit `int` declaration, so the cosine was read out of the integer
 * return register instead of f1 -- and the second one then handed an int to
 * fabs, which is how the audit's -Wabsolute-value row fell out of the same
 * root cause. */
float mbSinDeg(float deg);
float mbCosDeg(float deg);

/* The second wave, from the same audit run once tools/abi_warning_audit.py
 * stopped silently skipping the ~85 decomp TUs that carry no patch (see
 * docs/DECOMP_DEPENDENCY.md). Same rule: signature copied from the
 * definition, and declared here only where the decomp has no header for it.
 *   mbCapSelectMasuFrontNum  src/board/capsule.c:2524 (used at :2520)
 *   mbCapSelectMasuBackNum   src/board/capsule.c:2546 (used at :1976, :2521)
 *   mbWipeSpecialFadeOutCreate src/board/wipe.c:1170
 *   mbSingleStepGet          src/board/single.c:812 (used at :793)
 * mbWipeFadeIn IS declared, but only in include/REL/w01Dll_world01.h -- a REL
 * header no board TU includes -- so it joins mbWipeWait above for the same
 * reason. */
void mbWipeFadeIn(void);
int mbCapSelectMasuFrontNum(int masuId);
int mbCapSelectMasuBackNum(int masuId);
void mbWipeSpecialFadeOutCreate(int type, int time);
int mbSingleStepGet(void);

/* mbObjHookReset IS declared, in include/game/board/object.h:53, but src/board/
 * last5.c cannot include that header: it already has game/board/guide.h, and
 * the two disagree about mbObjKill (guide.h:71 `int` vs object.h:12
 * `MBMODELID`). Spelled with the underlying ABI type -- MBMODELID is `s16` in
 * all three of its definitions (game/board/{camera,model,object_data}.h) -- so
 * this stays compatible with object.h for every TU that does include it, the
 * same technique Hu3DModelShadowMapReset above already uses. */
void mbObjHookReset(s16 modelId);

/* Seam bridge, NOT decomp surface.  Defined by
 * patches/decomp/src/board/config.c.patch inside src/board/config.c (the
 * only scope where the file-static configDoneF exists) and called by
 * platform/os/board_placeholders.c's ConfigMain placeholder, which cannot
 * otherwise satisfy mbConfigExec's `while (!configDoneF)` at config.c:143.
 * Declared here so the patched definition and the placeholder's call are
 * checked against one prototype.  Both sides go away together when
 * ConfigMain lands on decomp main. */
void mp6_board_config_seam_done(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_BOARD_COMPAT_H */
