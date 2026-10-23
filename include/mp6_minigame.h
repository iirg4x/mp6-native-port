#ifndef MP6_MINIGAME_H
#define MP6_MINIGAME_H
#ifdef __cplusplus
extern "C" {
#endif
/* What a missing module is about to do, consumed once by the player-facing
 * overlay, which shows it as a notice. */
enum {
    MP6_UNAVAILABLE_NONE = 0,
    /* Unwound to a supported menu (Mini-Game mode, mode select, ...). */
    MP6_UNAVAILABLE_RETURNED = 1,
    /* A board's minigame ended at once with nobody scoring; the board's own
     * results/hand-back flow continues. */
    MP6_UNAVAILABLE_BOARD_MINIGAME = 2,
};
int mp6_unavailable_overlay_take_returned(void);
/* The overlay's notice for the taken result has gone. */
void mp6_unavailable_notice_closed(void);
/* Missing overlay child, on the stub's black screen: queue the notice. */
void mp6_unavailable_overlay_announce(void);
/* The notice is queued, not yet taken by the overlay. */
int mp6_unavailable_notice_pending(void);
/* The overlay took the notice and it is still on screen. */
int mp6_unavailable_notice_open(void);
/* Missing overlay child: unwind to a supported ancestor, never idle forever. */
void mp6_unavailable_overlay_return(void);
#ifdef __cplusplus
}
#endif
#endif
