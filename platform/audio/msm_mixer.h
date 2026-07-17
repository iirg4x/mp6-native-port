/* MP6 native port -- the one shared entry point
 * between the msm stream engine (msm_bridge.c, compiled in BOTH build
 * modes) and whichever backend actually drives it:
 *   - --headless: msm_bridge.c's own msmSysRegularProc pumps this directly
 *     (a real SDL device would have nobody to listen to it anyway) --
 *     purely so MP6_AUDIO_WAV_DUMP can capture real, non-silent audio
 *     without needing a display OR a live audio device.
 *   - default (aurora): platform/audio/audio_out_sdl.c's SDL3 audio
 *     callback pulls from this on SDL's own audio thread, in real time.
 *
 * Kept dependency-free at the header level (plain stdint.h) even though
 * its one implementation (msm_bridge.c) obviously isn't -- audio_out_sdl.c
 * only needs this one declaration, not msm_bridge.c's own internal state.
 */
#ifndef MP6_MSM_MIXER_H
#define MP6_MSM_MIXER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches every stream defined in the real sound/MP6_Str.pdt (all 110
 * tracks report frq==32000 in their own pack header). Individual streams
 * whose OWN pack->frq differs are still decoded at their real native rate
 * and resampled (simple linear interpolation) to this fixed mix rate
 * inside msm_bridge.c -- this constant is just the single shared OUTPUT
 * format both backends above open their device/WAV file at. */
#define MP6_MSM_OUT_RATE 32000
#define MP6_MSM_OUT_CHANNELS 2

/* Mixes every active (non-paused) msm stream channel into `frames` worth
 * of interleaved S16 stereo samples at MP6_MSM_OUT_RATE, advancing each
 * channel's own playback position (with loop wraparound) by that many
 * output frames. Always fully defined (writes silence) even with zero
 * active channels -- safe to call unconditionally once msmSysInit has run.
 * Not reentrant / not thread-safe against the msm control-plane calls
 * (msmStreamPlay/Stop/...): headless builds use a single-threaded tick
 * pump, and in the default build SDL's callback and the game's own
 * thread only ever touch channel state at well-separated points --
 * Play/Stop write small, aligned fields the mixer only ever reads, never
 * a torn multi-field update. */
void mp6_msm_render(int16_t *outInterleaved, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* MP6_MSM_MIXER_H */
