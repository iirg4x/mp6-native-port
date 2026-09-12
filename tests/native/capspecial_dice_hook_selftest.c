#include <assert.h>
#include <stdio.h>

typedef unsigned short u16;
#define PAD_BUTTON_A 0x100
static int diceHitTimer;

#include "board_subject.inc"

int main(void)
{
    u16 (*hook)(int) = ev_CapKoopaDicePadBtnHook;
    diceHitTimer = 3;
    assert(hook(-1) == 0 && diceHitTimer == 2);
    assert(hook(-1) == 0 && diceHitTimer == 1);
    assert(hook(-1) == PAD_BUTTON_A && diceHitTimer == 0);
    assert(hook(2) == PAD_BUTTON_A && diceHitTimer == -1);
    puts("PASS: Bowser dice hook uses the caller ABI and preserves timing");
    return 0;
}
