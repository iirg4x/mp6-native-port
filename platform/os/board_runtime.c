#include "mp6_board_runtime.h"
#include "mp6_events.h" /* w01.live / w01.render game events */
#include "mp6_diag_probe.h" /* pull-side board liveness -- see the getters below */

#include <stdio.h>

enum {
    MP6_W01_BOARD_NO = 0,
    MP6_W01_LIVE_TICK = 120,
};

static s32 s_boardNo = -1;
static u32 s_tickCount;
static u32 s_drawCount;
static BOOL s_liveLogged;
static BOOL s_renderLogged;

void mp6_board_runtime_reset(s32 boardNo)
{
    s_boardNo = boardNo;
    s_tickCount = 0;
    s_drawCount = 0;
    s_liveLogged = FALSE;
    s_renderLogged = FALSE;
}

void mp6_board_runtime_tick(void)
{
    if (s_boardNo != MP6_W01_BOARD_NO) {
        return;
    }
    ++s_tickCount;
    if (!s_liveLogged && s_tickCount >= MP6_W01_LIVE_TICK) {
        s_liveLogged = TRUE;
        printf("[W01] live tick=%u\n", (unsigned)s_tickCount);
        fflush(stdout);
        /* Also a game event: this and w01.render below are what a capture
         * driver waits on instead of guessing "the board is probably up by
         * tick N" -- the screenshot is taken because the board reported it
         * is running and has drawn, not because a timer expired. */
        mp6_event_post("w01.live", (long)s_tickCount, NULL);
    }
}

void mp6_board_runtime_draw(void)
{
    if (s_boardNo != MP6_W01_BOARD_NO) {
        return;
    }
    ++s_drawCount;
    if (s_liveLogged && !s_renderLogged) {
        s_renderLogged = TRUE;
        printf("[W01] render tick=%u draws=%u\n",
               (unsigned)s_tickCount, (unsigned)s_drawCount);
        fflush(stdout);
        mp6_event_post("w01.render", (long)s_drawCount, NULL);
    }
}

/* Pull-side liveness (shim/include/mp6_diag_probe.h). Both counters were
 * already running on every tick and every draw; they were simply unreadable
 * once the two one-shot [W01] lines had been printed, so "is the board still
 * ticking, and is it still drawing" had no answer after boot. Getters only. */
int mp6_diag_board_no(void)
{
    return (int)s_boardNo;
}

unsigned mp6_diag_board_ticks(void)
{
    return (unsigned)s_tickCount;
}

unsigned mp6_diag_board_draws(void)
{
    return (unsigned)s_drawCount;
}
