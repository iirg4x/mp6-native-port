/* Exact audio declarations mdsel.c needs but does not include itself.
 * The full game/audio.h conflicts with two older local declarations in that
 * recovered TU, so keep this compatibility surface intentionally minimal. */
#ifndef MP6_MDSEL_AUDIO_COMPAT_H
#define MP6_MDSEL_AUDIO_COMPAT_H

#include <dolphin/types.h>

void HuAudFXPanning(int seNo, s16 pan);
s32 HuAudFXVolSet(int seNo, s16 vol);

#endif
