#ifndef MP6_M662_NATIVE_H
#define MP6_M662_NATIVE_H

/* One native ABI contract for Trick or Tree. Signatures follow the recovered
 * definitions; callers must not redeclare narrowed returns or void pointers. */
#include "REL/m662dll/game.h"
#include "game/mg/timer.h"

void fn_1_A0(void);
void fn_1_130(s16 mode, s16 frameNo);
void fn_1_260(s16 mode, s16 frameNo);
void fn_1_28C(s16 mode, s16 frameNo);
void fn_1_2B8(s16 mode, s16 frameNo);
void fn_1_2E4(s16 mode, s16 frameNo);
void fn_1_310(s16 mode, s16 frameNo);
void fn_1_33C(s16 mode, s16 frameNo);
void fn_1_368(s16 mode, s16 frameNo);
void fn_1_394(s16 mode, s16 frameNo);
void fn_1_3C0(void);
void fn_1_3C4(void);
void fn_1_808(void);
void fn_1_984(void);
void fn_1_F44(void);
void fn_1_10F0(void);
void fn_1_1918(s16 cameraIndex);
void fn_1_1DC8(s16 cameraIndex);
void fn_1_1DFC(void);
void fn_1_1FD0(void);
void fn_1_223C(void);
void fn_1_234C(void);
void fn_1_2670(void);
s32 fn_1_2DA0(f32 *currentValue, f32 durationFrames, f32 start, f32 end);
MGTIMER *fn_1_2EC0(int timerType, int durationSeconds, int endSeconds, float xPosition, float yPosition);
void fn_1_2F34(MGTIMER *timer);
s32 fn_1_2F60(MGTIMER *timer);
void fn_1_2F88(MGTIMER *timer);
void fn_1_2FC0(s16 padPort, s16 lower, s16 upper);
void fn_1_3258(s16 padPort, s16 selection);
s16 fn_1_3AC4(s16 modelIndex, s16 lightId);
s32 fn_1_3BDC(HU3D_MODELID modelId, HU3D_LIGHTID lightId);
f32 fn_1_3C74(f32 limit);
void fn_1_3CEC(void);
void fn_1_3F94(Vec *position);
s16 fn_1_4270(s16 effectIndex, s16 ownerIndex, Vec position, Vec velocity, f32 initialScale, s16 lifetimeFrames, M662ParticleParameters *parameters);
void fn_1_4470(HU3D_MODEL *model, HU3D_PARTICLE *particle, Mtx matrix);
void fn_1_48E8(M662Player *player, s16 spatialMode);
void fn_1_4D04(void);
void fn_1_4E48(void);
void fn_1_4EAC(void);
void fn_1_4EFC(s16 streamSlot, s16 fadeSpeed);
void fn_1_4FA8(s16 streamSlot);
void fn_1_5008(s16 soundSlot, s16 spatialMode, HuVecF *position);
void fn_1_5468(s16 soundSlot, s16 pan);
void fn_1_54F8(s16 soundSlot, s16 pitch);
void fn_1_5588(s16 soundSlot, s16 volume);
void fn_1_5618(s16 soundSlot);
void fn_1_56E0(s16 soundSlot);
void fn_1_57A8(void);
s16 fn_1_57C8(void);
void fn_1_5884(void);
s32 fn_1_58B0(void);
void fn_1_5A5C(void);
s16 fn_1_795C(M662Puzzle *puzzle);
s16 fn_1_7AF8(void);
void fn_1_7DE8(void);
s32 fn_1_8754(void);
void fn_1_887C(void);
int fn_1_8B34(void);
s16 fn_1_8B8C(void);
s16 fn_1_8BCC(void);
void fn_1_8C08(void);
void fn_1_90AC(void);
void fn_1_9910(void);
s16 fn_1_9C1C(void);
s32 fn_1_A170(void);
s32 fn_1_A430(void);
s16 fn_1_A474(void);
s32 fn_1_A4B0(void);
s32 fn_1_A758(void);
s32 fn_1_A780(void);
s32 fn_1_A7A4(void);
void fn_1_AC88(void);
void fn_1_AD68(void);

void mp6_m662_main_reset(void);
void mp6_m662_game_reset(void);
void mp6_m662_puzzle_reset(void);
void mp6_m662_reset(void);

#endif
