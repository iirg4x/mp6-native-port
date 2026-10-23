#ifndef MP6_M631_NATIVE_H
#define MP6_M631_NATIVE_H

/* One native ABI contract for Conveyor Bolt.  The recovered overlay keeps
 * callbacks and owner payloads as real host pointers; do not redeclare these
 * entry points with integer-sized placeholders in individual TUs. */
#include "REL/m631Dll/m631_work.h"
#include "REL/m631Dll/utilities.h"

/* Shared actor/game entry points. */
f32 fn_1_2528(void);
void fn_1_AFF8(M631MotionPairNode *object, f32 radius, s32 contactKind);
void fn_1_AF2C(M631MotionPairNode *object, void *owner,
               void (*beforeUpdate)(void *),
               s32 (*afterUpdate)(void *),
               void (*resolvePair)(M631MotionPairNode *,
                                   M631MotionPairNode *, f32),
               HuVecF *position);

M631AnimationNode *fn_1_31D8(M631AnimationQueue *owner);
void fn_1_32C8(void *callbackData);
s32 fn_1_37FC(void *callbackData);
void fn_1_38E8(M631MotionPairNode *callback,
               M631MotionPairNode *context, f32 distance);
void fn_1_228C(HU3D_MODELID *models);
M631AnimationQueue *fn_1_2E9C(void);
void fn_1_3B44(M631ActorWork *actor);
void fn_1_3B7C(HuVecF *position);
void fn_1_3BC4(void);
void fn_1_3BF4(M631ActorMotionState *hitEffect);
s32 fn_1_51D0(f32 position);
void fn_1_5014(HuVecF *destination);
void fn_1_5054(void);
void fn_1_5084(void);
s16 fn_1_50B4(void);
void fn_1_50DC(HuVecF *direction, f32 magnitude);
s32 fn_1_AAD8(void *callbackData);
M631ActorWork *fn_1_469C(const HuVecF *position);
void fn_1_5284(OMOBJ *object);
void fn_1_5AAC(M631ActorWork *actor, s32 teamIndex, s32 isGroundPlayer);
void fn_1_A8F0(M631ActorWork *actor);
void fn_1_AA78(M631ActorWork *actor);

/* Utility-owned list and job-pool entry points. */
void fn_1_AB4C(void);
void fn_1_AB60(void);
void fn_1_AF94(M631MotionPairNode *object);
M631MotionPairNode *fn_1_B004(void);
void fn_1_B014(void);
M631Job *fn_1_B0B8(s32 (*callback)(void *), void *data);
void fn_1_B120(M631Job **job);
void fn_1_B1AC(void);

/* Native reset owners are called once before each overlay entry. */
void mp6_m631_actor_reset(void);
void mp6_m631_game_reset(void);
void mp6_m631_utilities_reset(void);
void mp6_m631_reset(void);

#endif
