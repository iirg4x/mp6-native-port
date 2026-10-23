#ifndef MP6_BOARD_WINDOW_H
#define MP6_BOARD_WINDOW_H

#include "game/board/window.h"

/* Borrowed text must remain alive until the board window finishes. Separate
 * native pointers from the u32 archive-message IDs, including queued windows. */
void mp6_board_window_insert_text(s16 window, const char *text, int slot);
void mp6_board_window_top_insert_text(const char *text, int slot);

#endif
