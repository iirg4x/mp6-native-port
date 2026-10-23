#ifndef MP6_M675_NATIVE_H
#define MP6_M675_NATIVE_H

#include "game/object.h"
#include "game/hu3d.h"

/* Shared native ownership contracts for Banana Shake. No serialized PPC
 * offsets: producers and consumers compile the same pointer-width layout. */
#define M675_COLLISION_PREFIX \
    u32 flags, mask, type; \
    s32 index, shapeType; \
    OMOBJ *owner; \
    void *excludedOwner; \
    OMOBJ_FUNC update;

typedef struct M675CollisionHeader {
    M675_COLLISION_PREFIX
} M675CollisionHeader;

/* Every shape owns a real header subobject. The anonymous view preserves the
 * recovered field spelling; the common initial sequence is shared in a union. */
#define M675_COLLISION_BASE \
    union { M675CollisionHeader header; struct { M675_COLLISION_PREFIX }; };

typedef struct M675Collision {
    M675_COLLISION_BASE
    s32 (*contact)(void *, void *);
    Vec *position, *velocity;
    Vec positionCopy;
    f32 radius, yieldOnContact;
    s32 handled;
} M675Collision;

typedef struct M675CollisionRegion {
    M675_COLLISION_BASE
    Vec center;
    f32 bottomOffsetY, height;
    f32 innerRadiusX, innerRadiusZ, outerRadiusX, outerRadiusZ;
} M675CollisionRegion;

typedef struct M675EllipseBoundary {
    M675_COLLISION_BASE
    Vec center;
    f32 radiusX, radiusZ;
    s32 direction;
} M675EllipseBoundary;

#undef M675_COLLISION_PREFIX
#undef M675_COLLISION_BASE

typedef struct M675CollisionWork {
    s32 initializationState, initializationTimer;
    void *collisionEntries[84];
    void *regionEntries[10];
} M675CollisionWork;

typedef struct M675EmitterRecord M675EmitterRecord;
typedef void (*M675CameraCallback)(OMOBJ *, u32, s32);

typedef struct M675EffectRegistration {
    s32 active; /* Nonzero enables drawing this registration's ground ring. */
    f32 radius; /* World-space radius around position. */
    Vec *position; /* Address of the registered effect's world position. */
} M675EffectRegistration;

typedef struct M675ActorState {
    M675Collision collision; /* Player contact data; prop instances leave it disabled. */
    s32 playerMappingIndex; /* Player mapping used by the player gameplay helpers. */
    s32 formationIndexAtCreation; /* Formation slot supplied when a player is created. */
    s32 formationIndex; /* Current player slot in the formation arrays. */
    s32 active; /* Gates human input, motion-offset easing, result arrival reporting and character
                 * feedback. */
    Vec basePosition; /* Player formation position before a motion offset is applied. */
    Vec position, velocity, rotation; /* World position, per-frame movement and Euler angles. */
    f32 baseYaw; /* Player facing angle before its motion offset is applied. */
    u32 buttonsDown, buttons; /* Buttons pressed this frame and currently held. */
    f32 stickAngle, stickMagnitude; /* Stick angle in degrees and its magnitude. */
    s32 motion, motionFrame; /* Selected motion and transform-update count since its selection. */
    s32 requestedMotion; /* Motion queued by the current frame's behavior. */
    f32 motionSpeed; /* Playback speed for the queued motion. */
} M675ActorState;

typedef struct M675PropMotionParameter {
    s32 resource; /* Motion resource index; -1 skips this slot and -2 ends the list. */
    f32 blend; /* Blend duration multiplier used when switching from another motion. */
    u32 attributes; /* Motion attributes applied when this motion starts. */
} M675PropMotionParameter;

typedef struct M675PropParameter {
    s32 count; /* Number of instances created from this entry. */
    s32 linkModel; /* Reuse an existing model when nonzero. */
    s32 type; /* Category used by the prop lookup table. */
    s16 firstId; /* ID assigned to the first instance. */
    s16 layer; /* Render layer for each instance's model. */
    u32 attributes; /* Model attributes enabled at creation. */
    s32 shadowMode; /* Selects no shadow, model shadow, or shadow-map reception. */
    s32 modelResource; /* Model data resource, or -1 when the prop has no model. */
    M675PropMotionParameter * motions; /* Motion resource and playback settings. */
    Vec * position; /* Optional initial world position. */
    f32 rotationY; /* Initial yaw in degrees. */
    OMOBJ_FUNC update; /* Per-frame callback assigned to each instance. */
} M675PropParameter;

typedef struct M675PropState {
    M675ActorState actor;
    /* Script phase and its frame timer or repeated-action counter; countdown and desired yaw. */
    s32 phase;
    s32 phaseTimer;
    f32 countdown; /* Remaining frames in a timed action. */
    f32 targetAngle;
    /* World-space destination and sub-action progress. */
    Vec targetPosition;
    s32 actionPhase;
    s32 actionTimer; /* Elapsed frames for the current action. */
    s32 soundTimer; /* Frames until the next repeated sound. */
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_0D0_to_0D3[4];
    s32 movementPhase; /* Substate of a turn, walk, or jump routine. */
    s32 movementTimer; /* Frames remaining or elapsed in that movement substate. */
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_0DC_to_0FB[32];
    s32 motionActive; /* Nonzero while the selected motion is being tracked. */
    s32 motionTimer; /* Cleared when motion tracking is inactive or the motion and blend have
                      * ended. */
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_104_to_107[4];
    s32 airborne; /* Whether display positioning uses the airborne depth bands. */
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_10C_to_127[28];
    s32 positionSyncEnabled; /* Follows the paired sequence actor's position when set. */
    Vec displayOffset; /* Temporary offset added when placing the model. */
    f32 movementSoundDistance; /* Accumulated requested walk speed, used to space walking sounds. */
    M675EffectRegistration effect; /* Shared ground-ring registration. */
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_148_to_1B3[108];
} M675PropState;

typedef struct M675PropWork {
    M675PropState state;
    M675PropParameter * parameters;
    s32 id;
    s32 updateCount;
    Vec initialPosition;
    /* Outer sequence phase, delayed-start frames, and phase timer. */
    s32 sequencePhase;
    s32 delay;
    s32 sequenceTimer;
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_1D8_to_1DB[4];
    /* Sequence selection values used by the prop callbacks. */
    union {
        s32 controlValue;
        s32 frameCount;
    };
    union {
        s32 secondaryControlValue;
        s32 lastRecordedFrame;
    };
    /* Bounce deformation scale and angle. */
    f32 bounceScale;
    f32 bounceAngle;
    /* Unused bytes cleared when the prop is created. */
    u8 unknown_1EC_to_283[152];
    /* Type 0's path recorder and types 2 through 4's emitters share this slot. */
    union {
        Vec *samples;
        M675EmitterRecord *records;
    };
    s32 soundHandle; /* Active sound-effect handle, or the stored inactive value. */
    s32 eventCount; /* Number of sequence events recorded for this prop. */
    s32 shuffleIndex; /* Current index in the shuffled effect-type list. */
    s32 effectActive; /* Whether the prop's triggered main/shape motion sequence is active. */
    f32 effectMotionSpeed; /* Shared main/shape motion playback-speed multiplier. */
    s32 alternateMotionStart; /* Main/shape motion start selector: zero uses frame 0, nonzero uses
                               * frame 15. */
    u8 shuffledEffectTypes[20]; /* Shuffled effect types emitted by this sequence. */
    Vec displayPosition; /* Ground-ring position; airborne props choose its Y from depth bands. */
} M675PropWork;

typedef struct M675EmitterRecord {
    /* Effect category, visibility/interaction state, and animation timer. */
    s32 type, enabled, state;
    f32 timer;
    u32 unmappedWordAfterTimer; /* Cleared with the emitter record; no later access is shown. */
    /* Phase count, model slot, and source player index. */
    s32 phaseCounter, modelId, playerIndex;
    /* Current transform and per-frame movement/rotation increments. */
    Vec position, rotation, velocity, rotationDelta;
    /* Uniform scale, model height adjustment, and floor cutoff offset from world Y=200. */
    f32 scale, verticalOffset, heightLimit;
    /* Endpoints used when moving the effect toward a screen-space target. */
    Vec sourcePosition, projectedPosition;
    M675Collision collision;
} M675EmitterRecord;

typedef struct M675AudioConfigView_s {
    u32 cameras; /* Cameras sharing this mapping; zero terminates the configuration list. */
    s32 panMin, panMax; /* Pan endpoints for the horizontal 0 to 576 mapping. */
    s32 volumeMin, volumeMiddle, volumeMax; /* Volume at the far, middle and near depths. */
    float distanceFar, distanceMiddle, distanceNear; /* Camera-space depths in world units. */
} M675AudioConfigView;

typedef struct M675FogParam_s {
    s32 layer; /* Render layer using this fog; -1 terminates the list. */
    u32 cameras; /* Camera mask supplying the near and far clipping distances. */
    GXFogType type; /* GX fog projection and falloff mode. */
    float start, end; /* Fog distances in world units before scaling the end distance. */
    GXColor color; /* Fog RGBA color passed to GX. */
} M675FogParam;

typedef struct M675CameraEntry_s {
    s32 active, motion, model; /* Motion-active flag, motion resource and camera model ID. */
    OM_CAMERA_VIEW view, previousView; /* Current view and view saved when camera motion stops. */
    Vec position, up, target; /* World-space camera pose read before this frame's view update. */
    float fov, nearPlane, farPlane, aspect; /* FOV in degrees, world clipping planes and aspect. */
    float viewport[4]; /* Pixel x, y, width and height. */
    float scissor[4]; /* Pixel x, y, width and height, converted to integers when applied. */
    s32 setupState; /* Zero requests the shadow callback's one-time camera-motion stop. */
    s32 setupTimer; /* Reset when the shadow callback advances the entry's setup state. */
    u8 unmappedCameraStorage[24]; /* Remaining storage cleared during camera setup. */
} M675CameraEntry;

typedef struct M675CameraWork_s {
    u32 cameras; /* Mask of cameras updated by this controller. */
    float shadowRadius, shadowFov; /* Shadow coverage in world units and FOV in degrees. */
    Vec shadowDirection; /* Light direction used to place the shadow camera. */
    GXColor shadowColor; /* Shadow tint; the update applies its RGB components. */
    void (*callback)(OMOBJ *, u32, s32); /* Per-camera update, passed its mask and index. */
    M675CameraEntry entries[16]; /* One view and optional motion model per camera bit. */
    s32 initializationCount; /* Shared guard for the shadow callback's one-time setup. */
    u8 unmappedManagerStorage[4]; /* Remaining storage cleared with the manager allocation. */
} M675CameraWork;

/* Cross-owner function contracts. */
void fn_1_BBC(OMOBJMAN *manager);
void fn_1_C84(OMOBJMAN *manager, Vec *position, Vec *target, GXColor *color);
void fn_1_E48(s16 modelId, char *lightName, s32 unusedMode);
void fn_1_136C(OMOBJMAN *manager, u32 cameras, float fov, float nearPlane, float farPlane,
    float shadowRadius, OM_CAMERA_VIEW *view);
void fn_1_1DD8(s32 camera, Vec *direction, Vec *target, float fov, float radius);
void fn_1_1FC8(M675CameraCallback callback);
void fn_1_2414(u32 cameras, void *data);
void fn_1_24CC(u32 cameraMask);
void fn_1_2F38(HUPROCESS *objectManager, M675AudioConfigView *audioConfig);
s32 fn_1_32EC(Vec *position);
void fn_1_3C70(s32 effect, Vec *position);
void fn_1_4424(int effect, int handle, Vec *position, int panEnabled, int volumeEnabled);
int fn_1_45C4(int dataNum);
s32 fn_1_46CC(s32 model, s32 dataNum);
float fn_1_47DC(float start, float end, float factor);
float fn_1_48F8(float start, float end);
float fn_1_49AC(HuVecF *currentPosition, HuVecF *nextPosition);
float fn_1_4A48(Vec *first, Vec *second);
void fn_1_4BB4(Point3d *start, Point3d *end, f32 factor, Point3d *position);
s32 fn_1_4C0C(Vec *start, Vec *end, f32 gravity, f32 lengthSeedY, Vec *velocity);
void fn_1_F118(HUPROCESS *process);
s32 fn_1_10170(void *registration);
Vec *fn_1_D550(s32 type, s32 index);
void fn_1_D638(HUPROCESS *process);
void fn_1_E0F0(Vec *origin);
void fn_1_E4F8(const Vec *position, f32 radius, s32 count);
void fn_1_E918(M675EffectRegistration *effect);
void fn_1_7554(HUPROCESS *process);
void fn_1_766C(void);
s32 fn_1_D3BC(void);
s32 fn_1_D3CC(void);
void fn_1_D494(s32 type, s32 value);
void fn_1_A0(void);
s32 fn_1_A38(s32 resource);
s32 fn_1_A40(void);
s32 fn_1_A50(void);
s32 fn_1_A60(void);
u32 fn_1_A98(u32 playerToken);
void fn_1_B14(s32 playerIndex, s32 playerToken, s32 characterId);
void fn_1_B4C(unsigned int playerIndex);
int fn_1_B6C(void);
void fn_1_4F2C(HUPROCESS *manager);
void fn_1_5048(void);
void fn_1_61F4(M675ActorState *state);
void fn_1_6210(M675ActorState *state, s32 motionIndex, f32 speed);
void fn_1_74C8(M675ActorState *state);
void fn_1_74FC(M675ActorState *state, s32 modelId);
void fn_1_E9A8(HUPROCESS *process);
void fn_1_EA30(void);
void fn_1_EF88(s32 playerIndex, s32 amount);
void fn_1_F004(void);
s32 fn_1_F058(s32 playerIndex);
void fn_1_F0B0(s32 playerIndex, f32 *xPosition, f32 *yPosition);

#endif
