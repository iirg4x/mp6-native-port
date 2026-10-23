#ifndef MP6_WINDOW_H
#define MP6_WINDOW_H

#include "game/window.h"

void mp6_window_measure_insert_text(const char *text, s16 slot);
/* [MP6] Quick Party: the window tells players what to press right now (a
 * board event's help panel); every tick it is up runs at normal speed when a
 * person plays (mp6_quick_play_action_prompt). A new message clears it. */
void mp6_window_quick_action_set(HUWINID window);
/* Set borrowed inline text without packing a native pointer into a u32
 * archive ID. Message callbacks receive source ID zero for inline text. */
void mp6_window_set_text(HUWINID window, const char *text);

/* Native counterpart of HuWinInsertMesSet's inline-pointer branch. The game
 * API's u32 argument also encodes archive IDs and cannot carry host pointers.
 * Text is borrowed, not copied: its storage must outlive the window's use. */
static inline void mp6_window_insert_text(HUWINID window, const char *text, s16 slot)
{
    if (window < 0 || window >= HUWIN_MAX || slot < 0 || slot >= HUWIN_INSERTMES_MAX)
        return;
    winData[window].messDataInsert[slot] = (char *)text;
}

#endif
