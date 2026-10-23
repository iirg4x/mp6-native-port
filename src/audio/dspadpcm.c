/* GameCube DSP-ADPCM decode core -- see dspadpcm.h for the format
 * writeup.
 *
 * Per-sample formula (the standard, widely-documented GC DSP-ADPCM decode,
 * matching e.g. Nintendo's own dsptool and every open-source reimplementation
 * of it):
 *
 *     sample = signedNibble * scale * 2048
 *     sample += 1024                              (round-to-nearest before
 *                                                    the final >>11 -- 1024
 *                                                    == 1 << (11-1))
 *     sample += coef1*hist1 + coef2*hist2          (Q11 fixed-point coefs)
 *     sample >>= 11
 *     sample = clamp to [-32768, 32767]
 *     hist2 = hist1; hist1 = sample                (history is the
 *                                                    CLAMPED output, not
 *                                                    the pre-clamp value --
 *                                                    matches real DSP
 *                                                    hardware, which only
 *                                                    ever sees the final
 *                                                    16-bit PCM output)
 */
#include "dspadpcm.h"

/* Arithmetic right shift of a negative value is implementation-defined in C;
 * every compiler this port builds with (clang via zig, the NDK and
 * Emscripten) shifts arithmetically, which is the floor division the DSP's
 * fixed point needs. Checked here so a compiler that did not would fail to
 * build rather than decode wrongly. */
_Static_assert((-2049 >> 11) == -2 && ((int64_t)-1 >> 1) == -1,
               "DSP-ADPCM decode needs an arithmetic right shift");

static inline int16_t mp6_adpcm_sample(int32_t nibble, int32_t scale2048, int32_t c1, int32_t c2,
                                       int32_t *hist1, int32_t *hist2)
{
    int64_t v = (int64_t)nibble * scale2048 + 1024 + (int64_t)c1 * *hist1 + (int64_t)c2 * *hist2;
    v >>= 11;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    *hist2 = *hist1;
    *hist1 = (int32_t)v;
    return (int16_t)v;
}

void mp6_dspadpcm_decode(const uint8_t *src, uint32_t numFrames,
                          const MP6AdpcmCoefTable *coef, MP6AdpcmState *state,
                          int16_t *out)
{
    int32_t hist1 = state->hist1;
    int32_t hist2 = state->hist2;
    uint32_t f;

    for (f = 0; f < numFrames; f++, src += 8, out += 14) {
        const uint8_t header = src[0];
        const int predictor = (header >> 4) & 0x7;
        const int32_t scale2048 = 2048 << (header & 0xF);
        const int32_t c1 = coef->coef[predictor][0];
        const int32_t c2 = coef->coef[predictor][1];
        int b;

        for (b = 0; b < 7; b++) {
            /* Two signed nibbles, high first: sign-extend through int8_t. */
            const uint8_t byte = src[1 + b];
            out[2 * b] = mp6_adpcm_sample((int8_t)byte >> 4, scale2048, c1, c2, &hist1, &hist2);
            out[2 * b + 1] = mp6_adpcm_sample((int8_t)(uint8_t)(byte << 4) >> 4, scale2048, c1, c2,
                                              &hist1, &hist2);
        }
    }

    state->hist1 = (int16_t)hist1;
    state->hist2 = (int16_t)hist2;
}
