/* MP6 native port -- raw mic audio for the game's volume meter (Options >
 * Mic test, HuMCVolGet), through SDL3's recording API on every windowed
 * platform (include/mp6_mic.h has the seam). Opened only while the game
 * keeps reading samples, so it never competes with a speech recognizer for
 * the microphone. On Android, SDL asks for the record permission itself. */

#include <SDL3/SDL.h>

#include "mp6_mic.h"

#include <stdio.h>

/* SAVESTATE CARVE-OUT: an SDL stream of the running process. */
#include "mp6_host_section.h"

static SDL_AudioStream *g_stream;

int mp6_mic_pcm_open(void)
{
    SDL_AudioSpec spec;
    if (g_stream != NULL) return 1;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = 22050;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
    if (g_stream == NULL) {
        printf("[MIC] no recording device for the volume meter: %s\n", SDL_GetError());
        fflush(stdout);
        return 0;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return 1;
}

int mp6_mic_pcm_read(int16_t *dst, int maxSamples)
{
    int got;
    if (g_stream == NULL) return 0;
    got = SDL_GetAudioStreamData(g_stream, dst, maxSamples * (int)sizeof(int16_t));
    return got > 0 ? got / (int)sizeof(int16_t) : 0;
}

void mp6_mic_pcm_close(void)
{
    if (g_stream != NULL) SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}
