#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;

typedef struct PADStatus_s {
    u16 button;
    s8 stickX;
    s8 stickY;
    s8 substickX;
    s8 substickY;
    u8 triggerL;
    u8 triggerR;
    u8 err;
} PADStatus;

enum {
    PAD_ERR_NONE = 0,
    PAD_ERR_TRANSFER = 1,
    PAD_ERR_NOT_READY = 2,
    PAD_ERR_NO_CONTROLLER = 3,
    PAD_CHAN0_BIT = 1,
    PAD_CHAN1_BIT = 2,
    PAD_CHAN2_BIT = 4,
    PAD_CHAN3_BIT = 8,
    SI_GC_RECEIVER = 0x88000000,
    PAD_MOTOR_RUMBLE = 1,
    PAD_MOTOR_STOP = 2,
    PAD_MOTOR_STOP_HARD = 3,
    PAD_BUTTON_TRIGGER_L = 0x4000,
    PAD_BUTTON_TRIGGER_R = 0x2000,
};

typedef struct PadRumble_s {
    s16 maxTime;
    s16 offTime;
    s16 onTime;
    s16 time;
    s16 numRumble;
} RUMBLEDATA;

static u32 GlobalCounterOld = (u32)-1;
static u32 GlobalCounter;
static s16 RumbleCounter;
static u32 RumbleBit;
static int HuDvdErrWait;
static int padStatErrOld[4];
static RUMBLEDATA rumbleData[4];
static u32 chanTbl[4] = {
    PAD_CHAN0_BIT, PAD_CHAN1_BIT, PAD_CHAN2_BIT, PAD_CHAN3_BIT
};

static u16 _PadRepCnt[4];
static s8 _PadStkX[4];
static s8 _PadStkY[4];
static s8 _PadSubStkX[4];
static s8 _PadSubStkY[4];
static u8 _PadTrigL[4];
static u8 _PadTrigR[4];
static u8 _PadDStk[4];
static u8 _PadDStkRep[4];
static u8 _PadDStkDown[4];
static s8 _PadErr[4];
static u16 _PadBtn[4];
static u16 _PadBtnDown[4];
static u16 HuPadBtnRep[4];
static float HuPadStkXf[4];
static float HuPadStkYf[4];
static float HuPadSubStkXf[4];
static float HuPadSubStkYf[4];
static float HuPadTrigLf[4];
static float HuPadTrigRf[4];
static u16 HuPadBtnMask;
static s32 VCounter;

static int motorCount;
static s16 motorPads[32];
static u32 motorCommands[32];

u32 PADRead(PADStatus *status)
{
    memset(status, 0, sizeof(PADStatus) * 4);
    return 0;
}

void PADClampCircle(PADStatus *status)
{
    (void)status;
}

void PADControlMotor(s16 pad, u32 command)
{
    assert(motorCount < (int)(sizeof(motorPads) / sizeof(motorPads[0])));
    motorPads[motorCount] = pad;
    motorCommands[motorCount] = command;
    ++motorCount;
}

u16 PADButtonDown(u16 oldButtons, u16 newButtons)
{
    return (u16)(newButtons & (u16)~oldButtons);
}

u32 SIProbe(s16 pad)
{
    (void)pad;
    return 0;
}

void PADReset(u32 channels)
{
    (void)channels;
}

void PadADConv(s16 pad, PADStatus *status)
{
    (void)pad;
    (void)status;
}

void msmSysRegularProc(void)
{
}

void HuMCPeriodicProc(void)
{
}

/* PAD_RUMBLE_NATIVE_SUBJECT */

static void reset_fixture(void)
{
    memset(padStatErrOld, 0, sizeof(padStatErrOld));
    memset(rumbleData, 0, sizeof(rumbleData));
    memset(_PadErr, 0, sizeof(_PadErr));
    memset(_PadBtn, 0, sizeof(_PadBtn));
    memset(_PadBtnDown, 0, sizeof(_PadBtnDown));
    memset(_PadRepCnt, 0, sizeof(_PadRepCnt));
    memset(_PadStkX, 0, sizeof(_PadStkX));
    memset(_PadStkY, 0, sizeof(_PadStkY));
    memset(_PadSubStkX, 0, sizeof(_PadSubStkX));
    memset(_PadSubStkY, 0, sizeof(_PadSubStkY));
    memset(_PadTrigL, 0, sizeof(_PadTrigL));
    memset(_PadTrigR, 0, sizeof(_PadTrigR));
    memset(_PadDStk, 0, sizeof(_PadDStk));
    memset(_PadDStkRep, 0, sizeof(_PadDStkRep));
    memset(_PadDStkDown, 0, sizeof(_PadDStkDown));
    memset(HuPadBtnRep, 0, sizeof(HuPadBtnRep));
    GlobalCounterOld = (u32)-1;
    GlobalCounter = 0;
    RumbleCounter = 0;
    RumbleBit = 0;
    HuDvdErrWait = 0;
    HuPadBtnMask = 0;
    VCounter = 0;
    motorCount = 0;
    memset(motorPads, 0, sizeof(motorPads));
    memset(motorCommands, 0, sizeof(motorCommands));
}

static void test_zero_cycle_is_continuous_until_cutoff(void)
{
    reset_fixture();
    rumbleData[0].maxTime = 6;
    rumbleData[0].offTime = 0;
    rumbleData[0].onTime = 0;
    rumbleData[0].numRumble = 3;

    for (int frame = 0; frame < 7; ++frame) {
        PadReadVSync((u32)frame);
    }

    /* The first callback starts the motor; zero/zero leaves it running. */
    assert(motorCount == 2);
    assert(motorPads[0] == 0 && motorCommands[0] == PAD_MOTOR_RUMBLE);
    assert(motorPads[1] == 0 && motorCommands[1] == PAD_MOTOR_STOP_HARD);
    assert(rumbleData[0].maxTime == 0);
}

static void test_nonzero_cycle_keeps_phase_behavior(void)
{
    reset_fixture();
    rumbleData[0].maxTime = 6;
    rumbleData[0].offTime = 2;
    rumbleData[0].onTime = 3;
    rumbleData[0].numRumble = 0;

    for (int frame = 0; frame < 7; ++frame) {
        PadReadVSync((u32)frame);
    }

    assert(motorCount == 4);
    assert(motorCommands[0] == PAD_MOTOR_RUMBLE);
    assert(motorCommands[1] == PAD_MOTOR_STOP);
    assert(motorCommands[2] == PAD_MOTOR_RUMBLE);
    assert(motorCommands[3] == PAD_MOTOR_STOP_HARD);
}

int main(void)
{
    test_zero_cycle_is_continuous_until_cutoff();
    test_nonzero_cycle_keeps_phase_behavior();
    return 0;
}
