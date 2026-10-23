#ifndef MP6_MGMTOURNAMENT_NATIVE_H
#define MP6_MGMTOURNAMENT_NATIVE_H

/* Mount Duel has one cross-unit ABI; recovered code retains gameplay ownership. */
#include "REL/mgmtournamentdll/module_types.h"
#include "game/hu3d.h"
#include "game/window.h"

/* bracket */
void fn_1_2E28(void);
void fn_1_31B8(void);
void fn_1_3394(s16 modelIndex, s16 enabled);
void fn_1_34B0(s16 modelIndex);
void fn_1_34F0(void);
void fn_1_3E04(void);
s16 fn_1_3F00(void);
void fn_1_3F18(s16 side);
void fn_1_3F8C(void);
void fn_1_43BC(void);
void fn_1_4424(OMOBJ *object);
void fn_1_4850(void);
void fn_1_4A1C(void);
void fn_1_4A94(s16 playerNo, s16 characterNo, s16 groupNo, Point3d worldPosition);

/* camera */
void fn_1_E9C(void);
void fn_1_FE8(void);
void fn_1_1134(void);
void fn_1_1138(void);
void fn_1_13A4(u32 durationFrames);
void fn_1_143C(s16 viewIndex, u32 durationFrames);
void fn_1_14C8(OMOBJ *object);
void fn_1_16F8( HU3D_CAMERA *camera, Vec *startPosition, Vec *startTarget, const Vec *endPosition, const Vec *endTarget, f32 durationFrames, f32 elapsedFrames);

/* characters */
void fn_1_6278(OMOBJ *object, s16 modeNo);
void fn_1_64A8(void);
void fn_1_68A4(Vec *result, const Vec *point0, const Vec *point1, f32 duration, f32 time);
s32 fn_1_6A24(void);
void fn_1_6D78(void);
void fn_1_71E8(s16 *firstPlayer, s16 *secondPlayer);
void fn_1_74BC(void);
void fn_1_7924(void);
s16 fn_1_7BC0(s16 player);
s16 fn_1_7C00(s16 playerNo);
s16 fn_1_7C44(s16 playerNo);
void fn_1_7C78(Vec *center);
void fn_1_7D58(s16 playerNo, s16 isWinner);
void fn_1_7DA8(s16 playerNo, s16 isWinner);
void fn_1_7DF8(s16 playerNo);
void fn_1_7E50(s16 playerNo);
void fn_1_7EA8(s16 playerNo);
void fn_1_7F78(s16 playerNo, Vec *position);
void fn_1_7FC8(s16 playerNo, Vec *position);
void fn_1_8018(s16 player);
void fn_1_8094(s16 player);
s16 fn_1_80CC(s16 player);
void fn_1_812C(s16 playerNo, Vec *position);
void fn_1_818C(void);
void fn_1_8238(OMOBJ *object);
void fn_1_A1F4(void);
void fn_1_A35C(s16 playerNo, const Vec *position);
void fn_1_A3B0(s16 playerNo, s32 showEffect);
s16 fn_1_A42C(void);
void fn_1_A6B4(void);
void fn_1_AA04(void);
void fn_1_AB48(void);
void fn_1_AD50(void);
void fn_1_B014(void);
void fn_1_B2E0(void);
void fn_1_B57C(void);
void fn_1_B628(void);
void fn_1_B80C(void);
void fn_1_B850(void);
s16 fn_1_BAD4(void);
void fn_1_CF6C(void);
void fn_1_D544(void);
s32 _prolog(void);
void _epilog(void);

/* host */
void fn_1_5154(void);
void fn_1_5590(void);
void fn_1_56B4(u16 enabled);
void fn_1_5728(void);
void fn_1_5740(s16 positionIndex, s16 duration);
void fn_1_57E0(s16 duration);
void fn_1_5A44(s16 duration);
void fn_1_5B84(OMOBJ *object);
void fn_1_60F8(Vec *interpolatedPosition, const Vec *beforeStart, const Vec *startPosition, const Vec *destination, const Vec *pastDestination, f32 durationFrames, f32 elapsedFrames);

/* math */
s32 fn_1_E378(Point3d *direction, Point3d *unitDirection);
void fn_1_E4D0(f32 angleX, f32 angleY, f32 angleZ, const Vec *source, Vec *destination);

/* preview */
HU3D_MODELID fn_1_0(s16 gameNo, s16 timeOfDay, Point3d *position);
void fn_1_2A4(s16 gameCount, u8 *gameOrder);
void fn_1_43C(void);
void fn_1_4F8(void);
void fn_1_5E0(s16 gameNo, s16 timeOfDay);
void fn_1_6F4(void);
void fn_1_7F0(void);
void fn_1_824(void);
void fn_1_854(s16 layerNo);
void fn_1_898(HU3D_MODEL *model, Mtx *matrix);

/* scene */
void fn_1_D69C(void);
void fn_1_DBB0(void);
void fn_1_DC28(void);

/* window */
void fn_1_199C(s16 windowId, s16 insertionSlot);
void fn_1_19F4(s16 windowId);
void fn_1_1A8C(void);
void fn_1_1BA0(void);
void fn_1_1BD0(void);
void fn_1_1C84(void);
void fn_1_1D54(void);
void fn_1_1D74(void);
void fn_1_1E80(void);
void fn_1_1F90(void);
s16 fn_1_2128(void);
void fn_1_2728(s16 player);
void fn_1_2948(void);
s16 fn_1_2BA0(void);
void fn_1_2D1C(HUWINID windowId, u32 messageId, s16 callbackCode);
void fn_1_2DF8(s16 pan);

void mp6_mgmtournament_bracket_reset(void);
void mp6_mgmtournament_camera_reset(void);
void mp6_mgmtournament_characters_reset(void);
void mp6_mgmtournament_host_reset(void);
void mp6_mgmtournament_preview_reset(void);
void mp6_mgmtournament_scene_reset(void);
void mp6_mgmtournament_window_reset(void);
void mp6_mgmtournament_reset(void);

#endif
