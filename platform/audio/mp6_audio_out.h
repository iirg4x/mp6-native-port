/* MP6 native port -- the live-playback half of the audio backend split
 * (see msm_mixer.h for the other half, the shared mixer entry point).
 *
 * Exactly ONE of these two definitions is ever compiled into a given
 * build, matching the established VIInit/VIWaitForRetrace/GXInit split
 * (platform/null/shims_manual.c) already used throughout this port:
 *   - --headless: msm_bridge.c itself defines both as inert no-ops
 *     (#ifdef MP6_HEADLESS_BUILD) -- there is no live device to open, and
 *     nothing else needs tearing down.
 *   - default (aurora): platform/audio/audio_out_sdl.c (tools/build.py's
 *     PLATFORM_AURORA_ONLY, compiled with Aurora's own flags/include path
 *     for <SDL3/SDL.h>) provides the real SDL3 device/stream.
 *
 * Called from msm_bridge.c's msmSysInit rather than platform/main_native.c
 * -- msmSysInit is already the natural "audio subsystem is coming up"
 * moment.
 */
#ifndef MP6_AUDIO_OUT_H
#define MP6_AUDIO_OUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent -- safe to call more than once (msmSysInit could in theory
 * run again if the game ever called it twice; real callers here don't,
 * but there's no reason to make that a hard requirement). */
void mp6_audio_out_init(void);

/* Not currently called from anywhere -- there is no process-exit hook
 * reachable from here without touching platform/os/*.c or
 * platform/gx/aurora_bridge.c. Declared for completeness/future use;
 * the OS reclaims the audio device on process exit either way. */
void mp6_audio_out_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_AUDIO_OUT_H */
