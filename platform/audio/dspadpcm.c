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

static int16_t mp6_clamp16(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* C's right shift of a negative signed integer is implementation-defined.
 * DSP fixed-point wants an arithmetic >> 11 (floor division), so spell that
 * operation portably after doing every multiply in int64_t. */
static int64_t mp6_arshift11(int64_t v)
{
    if (v >= 0) return v / 2048;
    return -((-v + 2047) / 2048);
}

void mp6_dspadpcm_decode(const uint8_t *src, uint32_t numFrames,
                          const MP6AdpcmCoefTable *coef, MP6AdpcmState *state,
                          int16_t *out)
{
    int32_t hist1 = state->hist1;
    int32_t hist2 = state->hist2;
    uint32_t f;

    for (f = 0; f < numFrames; f++) {
        const uint8_t *frame = src + (size_t)f * 8;
        uint8_t header = frame[0];
        /* Predictor index is 4 bits (0-15) in the header byte, but only 8
         * coef pairs exist -- mask defensively (see dspadpcm.h). Real
         * data keeps the predictor in 0-7 and never sets the top bit. */
        int predictor = (header >> 4) & 0x7;
        int scaleShift = header & 0xF;
        int32_t scale = 1 << scaleShift;
        int32_t c1 = coef->coef[predictor][0];
        int32_t c2 = coef->coef[predictor][1];
        int i;

        for (i = 0; i < 14; i++) {
            uint8_t byte = frame[1 + i / 2];
            int nibble = (i & 1) ? (byte & 0xF) : (byte >> 4);
            int32_t signedNibble = (nibble > 7) ? (nibble - 16) : nibble;
            int64_t sample = (int64_t)signedNibble * (int64_t)scale * 2048;
            int16_t s16;

            sample += 1024;
            sample += (int64_t)c1 * hist1 + (int64_t)c2 * hist2;
            sample = mp6_arshift11(sample);

            s16 = mp6_clamp16(sample);
            out[f * 14 + i] = s16;
            hist2 = hist1;
            hist1 = s16;
        }
    }

    state->hist1 = (int16_t)hist1;
    state->hist2 = (int16_t)hist2;
}
