#ifndef MP6_BOARD_RUNTIME_H
#define MP6_BOARD_RUNTIME_H

#include "dolphin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-only observability for the real shared board camera path.  These calls
 * do not mutate game state; they make the W01 smoke gate prove sustained
 * object execution and a genuine camera-layer draw after setup. */
void mp6_board_runtime_reset(s32 boardNo);
void mp6_board_runtime_tick(void);
void mp6_board_runtime_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_BOARD_RUNTIME_H */
