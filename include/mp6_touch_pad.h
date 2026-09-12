/* Port-owned Android controller. No game/decomp structure crosses this seam. */
#pragma once
#include <stdint.h>

enum Mp6TouchControl
{
    MP6_TOUCH_STICK,
    MP6_TOUCH_CSTICK,
    MP6_TOUCH_DPAD,
    MP6_TOUCH_A,
    MP6_TOUCH_B,
    MP6_TOUCH_X,
    MP6_TOUCH_Y,
    MP6_TOUCH_Z,
    MP6_TOUCH_L,
    MP6_TOUCH_R,
    MP6_TOUCH_START,
    MP6_TOUCH_COUNT
};
typedef struct Mp6TouchConfig
{
    int enabled, size, opacity, floating;
    unsigned int visible;
    /* Centers in ten-thousandths of the usable screen; -1 = edge-anchored default. */
    int x[MP6_TOUCH_COUNT], y[MP6_TOUCH_COUNT];
} Mp6TouchConfig;
typedef struct Mp6TouchState
{
    uint16_t buttons;
    int8_t stickX, stickY, cstickX, cstickY;
    uint8_t triggerL, triggerR;
} Mp6TouchState;
#ifdef __cplusplus
extern "C"
{
#endif
    union SDL_Event;
    void mp6_touch_pad_event(const union SDL_Event *event);
    void mp6_touch_pad_collect(Mp6TouchState *out);
    void mp6_touch_pad_draw(void);
    void mp6_touch_pad_savestate_reset(void);
    int mp6_touch_pad_control_at(float nx, float ny);
    void mp6_touch_pad_begin_edit(void);
    int mp6_touch_pad_editing(void);
#ifdef __cplusplus
}
#endif
