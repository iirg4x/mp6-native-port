/* Tiny standalone canonical-PCM WAV file writer, used by the
 * MP6_AUDIO_WAV_DUMP verification path (see msm_bridge.c).
 * Dependency-free (plain stdint.h + libc stdio) -- no SDL, no
 * dolphin/game headers -- so it compiles identically in both build
 * modes and needs nothing beyond what every other platform/audio/ file
 * already links.
 */
#ifndef MP6_WAV_WRITER_H
#define MP6_WAV_WRITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a canonical 44-byte-header, 16-bit signed PCM RIFF/WAVE file.
 * `interleaved` holds `frames * channels` int16_t samples (channel-
 * interleaved, e.g. L,R,L,R,... for channels==2). Returns 0 on success,
 * -1 on any I/O failure (bad path, disk full, ...). */
int mp6_wav_write(const char *path, const int16_t *interleaved, uint32_t frames,
                   uint32_t sampleRate, uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif /* MP6_WAV_WRITER_H */
