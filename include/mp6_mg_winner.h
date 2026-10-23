#ifndef MP6_MG_WINNER_H
#define MP6_MG_WINNER_H

#include "dolphin/types.h"

/* Port addition (game/seqman.c): show MgSeq's "<name> Won!" message for the
 * given character numbers (negative = none, all none = draw) at the WINNER
 * phase, without MgSeq's jingle. For minigames whose original code plays
 * its own result jingle and never names the winner. */
void mp6_mg_winner_message(s16 charNo1, s16 charNo2, s16 charNo3, s16 charNo4);

#endif
