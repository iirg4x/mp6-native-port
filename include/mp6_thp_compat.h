/* Exact public THP entry points used by game/THPSimple.c.
 * dolphin/thp.h also declares an unrelated internal static VideoDecode,
 * which collides with THPSimple.c's own helper, so force-including the
 * umbrella header is not viable for this one translation unit. */
#ifndef MP6_THP_COMPAT_H
#define MP6_THP_COMPAT_H

#include <dolphin/types.h>

BOOL THPInit(void);
u32 THPAudioDecode(s16 *audioBuffer, u8 *audioFrame, s32 flag);
s32 THPVideoDecode(void *file, void *tileY, void *tileU, void *tileV, void *work);

#endif
