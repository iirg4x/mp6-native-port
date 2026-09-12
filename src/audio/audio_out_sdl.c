/* MP6 native port -- real-time SDL3 audio playback for the msm stream
 * mixer (msm_bridge.c). Default (aurora, non-headless) build ONLY --
 * mirrors src/gx/aurora_bridge.c's own split (tools/build.py's
 * PLATFORM_AURORA_ONLY), compiled with Aurora's own AURORA_FLAGS (its
 * own -I's, including SDL3's real headers -- NOT COMMON_FLAGS/decomp's
 * -I's, same reasoning as aurora_bridge.c's own header comment: mixing
 * both include roots in one TU would make -I ORDER decide which tree's
 * copy of an identically-relative-pathed header wins).
 * This file therefore has zero decomp/dolphin dependency of its own --
 * just plain C, <SDL3/SDL.h>, and msm_mixer.h/mp6_audio_out.h (both
 * dependency-free too, see their own header comments).
 *
 * --headless has no live device to feed at all; msm_bridge.c's own
 * msmSysRegularProc drives src/audio/msm_mixer.h's mp6_msm_render()
 * directly instead, purely for the MP6_AUDIO_WAV_DUMP verification path
 * (see that file) -- this file is simply never compiled into that build.
 *
 * SDL3's own audio model: SDL_OpenAudioDeviceStream opens a physical
 * device AND creates+binds a matching SDL_AudioStream in one call, with
 * an optional pull callback fired whenever the stream needs more data --
 * exactly the shape this mixer wants (msm_bridge.c already decoded every
 * active stream's PCM up front; this callback just needs to advance
 * playback position and hand back already-mixed samples, no I/O of its
 * own at all). The stream's logical format is fixed at
 * MP6_MSM_OUT_RATE Hz stereo S16 (matching the real .pdt's own native
 * rate for every track actually found on this disc); SDL's own internal
 * resampler converts to whatever the physical output device actually
 * wants, so this file never needs to know or care what that is.
 */
#include <SDL3/SDL.h>
#include "msm_mixer.h"
#include "mp6_audio_out.h"

#include <stdio.h>
#include <stdint.h>

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own
 * includes is load-bearing, not stylistic: it is a #pragma clang section that
 * redirects every file-scope definition FOLLOWING it. As a -include (before all
 * headers) it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES. */
#include "mp6_host_section.h"


static SDL_AudioStream *g_stream;
static int g_typicalAdditionalBytes;
static uint32_t g_starvationCount;
static uint32_t g_callbackCount;
static int g_putFailureReported;

/* Launcher-owned master gain, applied via
 * SDL's own per-stream gain (SDL_SetAudioStreamGain -- multiplies during
 * SDL's format conversion, so the mixer/decoder outputs above stay
 * untouched). 1.0 is neutral and is also SDL's default, and the setter is
 * only ever called by the launcher (menu-mode runs) -- automation runs
 * never call it, so their audio path is byte-identical, including never
 * issuing the SDL gain call at all. Stored here so a menu volume change
 * made BEFORE audio init (the launcher runs pre-GameMain) still lands
 * when the stream opens. */
static float g_mp6MasterGain = 1.0f;

void mp6_audio_set_master_gain(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    g_mp6MasterGain = gain;
    if (g_stream != NULL) {
        SDL_SetAudioStreamGain(g_stream, g_mp6MasterGain);
    }
}

/* additional_amount is in BYTES of the STREAM's (not the device's) format
 * -- MP6_MSM_OUT_CHANNELS * sizeof(int16_t) per frame, per SDL3's own
 * documented contract for this callback shape. A LARGER-than-typical
 * request is this file's own starvation heuristic: SDL3's pull callback
 * API has no explicit "an underrun already happened" flag at this level,
 * so this is a pragmatic proxy -- the device is asking for unusually much
 * more data than its own steady-state cadence, meaning its internal
 * buffer ran further down than usual before this callback fired. */
static void SDLCALL mp6_audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    int16_t scratch[2048];
    uint32_t remaining;

    (void)userdata;
    (void)total_amount;
    g_callbackCount++;
    if (additional_amount <= 0) return;

    if (g_typicalAdditionalBytes == 0) {
        g_typicalAdditionalBytes = additional_amount;
    } else if (g_typicalAdditionalBytes <= INT32_MAX / 2 &&
               additional_amount > g_typicalAdditionalBytes * 2) {
        g_starvationCount++;
    }

    remaining = (uint32_t)additional_amount / (MP6_MSM_OUT_CHANNELS * (int)sizeof(int16_t));
    while (remaining > 0) {
        uint32_t chunk = remaining;
        uint32_t maxChunk = (uint32_t)(sizeof(scratch) / sizeof(scratch[0])) / MP6_MSM_OUT_CHANNELS;
        if (chunk > maxChunk) chunk = maxChunk;
        mp6_msm_render(scratch, chunk);
        if (!SDL_PutAudioStreamData(stream, scratch,
                                    (int)(chunk * MP6_MSM_OUT_CHANNELS * sizeof(int16_t)))) {
            /* Do not spin through the rest of a request after the stream
             * has rejected the first chunk. The next callback may recover;
             * this one cannot publish useful audio anymore. */
            if (!g_putFailureReported) {
                g_putFailureReported = 1;
            }
            return;
        }
        remaining -= chunk;
    }
}

void mp6_audio_out_init(void)
{
    SDL_AudioSpec spec;

    g_putFailureReported = 0;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            printf("[AUDIO] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s -- no music will play\n",
                   SDL_GetError());
            return;
        }
    }

    spec.format = SDL_AUDIO_S16;
    spec.channels = MP6_MSM_OUT_CHANNELS;
    spec.freq = MP6_MSM_OUT_RATE;

    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mp6_audio_callback, NULL);
    if (!g_stream) {
        printf("[AUDIO] SDL_OpenAudioDeviceStream failed: %s -- no music will play\n", SDL_GetError());
        return;
    }
    /* A launcher-configured volume set before this init (see
     * g_mp6MasterGain's comment). Guarded so the default (1.0 -- every
     * automation run, and the launcher default) never issues the call. */
    if (g_mp6MasterGain != 1.0f) {
        SDL_SetAudioStreamGain(g_stream, g_mp6MasterGain);
    }
    if (!SDL_ResumeAudioStreamDevice(g_stream)) {
        printf("[AUDIO] SDL_ResumeAudioStreamDevice failed: %s -- no music will play\n",
               SDL_GetError());
        SDL_DestroyAudioStream(g_stream);
        g_stream = NULL;
        return;
    }
    printf("[AUDIO] SDL3 audio device open: %d Hz, %d ch, S16 -- real-time playback ACTIVE\n",
           spec.freq, spec.channels);
}

void mp6_audio_out_shutdown(void)
{
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = NULL;
    }
    printf("[AUDIO] audio_out_sdl shutdown: %u callback(s) total, %u possible underrun(s)\n",
           (unsigned)g_callbackCount, (unsigned)g_starvationCount);
    if (g_putFailureReported)
        fprintf(stderr, "[AUDIO] SDL rejected audio data during playback\n");
    mp6_msm_collect_finished();
}
