#ifndef MP6_MINIGAME_H
#define MP6_MINIGAME_H
#ifdef __cplusplus
extern "C" {
#endif
/* Game-thread notifications, consumed once by the player-facing overlay. */
int mp6_minigame_take_skipped(void);
int mp6_unavailable_overlay_take_returned(void);
/* Missing overlay child: unwind to a supported ancestor, never idle forever. */
void mp6_unavailable_overlay_return(void);
#ifdef __cplusplus
}
#endif
#endif
