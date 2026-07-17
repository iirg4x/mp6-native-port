/* MP6 native port -- standalone GameCube DSP-ADPCM decoder.
 *
 * Deliberately dependency-free (plain stdint.h, no dolphin/musyx/game
 * headers) so it can be unit-tested or reused outside this port entirely.
 * Implements the standard, widely-documented GC "DSP-ADPCM" codec used for
 * both streamed music (msm's own .pdt container, see msm_bridge.c) and
 * countless other GC formats (.dsp, .thp audio, .hps, ...): 8-byte frames
 * (1 header byte + 7 data bytes -> 14 4-bit samples), a per-frame
 * predictor-index/scale header nibble pair, and an 8-entry table of
 * (coef1, coef2) predictor pairs shared by a whole stream. The header
 * byte's high nibble selects the predictor (0-7, indexing the 8-entry
 * coef table) and the low nibble is the per-frame scale shift.
 */
#ifndef MP6_DSPADPCM_H
#define MP6_DSPADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One GC DSP-ADPCM stream's predictor-coefficient table: 8 (coef1, coef2)
 * pairs, selected per-frame by that frame's header byte's high nibble.
 * Matches musyx/stream.h's real SND_ADPCMSTREAM_INFO layout exactly
 * (s16 coefTab[8][2]) -- not redefined from that header only so this file
 * stays dependency-free; msm_bridge.c is responsible for populating one of
 * these from the real (big-endian) file bytes via mp6_be16.
 */
typedef struct {
    int16_t coef[8][2];
} MP6AdpcmCoefTable;

/* Decode history -- the last two OUTPUT (post-clamp) samples, carried
 * across frames within one continuous stream. Zero-initialize at the start
 * of a fresh stream (real hardware does the same -- there is no "loop
 * history" seed stored anywhere in this format; see msm_bridge.c's own
 * notes on why a hard loop-point restart is an accepted, minor
 * simplification). */
typedef struct {
    int16_t hist1;
    int16_t hist2;
} MP6AdpcmState;

/* Decodes `numFrames` consecutive 8-byte GC DSP-ADPCM frames from `src`
 * into `out` (must hold at least numFrames*14 int16_t samples), advancing
 * `state` in place so a caller can resume decoding a later, contiguous
 * range of the same stream across multiple calls. `coef` is looked up by
 * each frame's own header nibble (masked to 0-7 defensively -- real data
 * never carries a predictor outside the 8-entry table, but a corrupt/
 * unexpected file should not read out of bounds). */
void mp6_dspadpcm_decode(const uint8_t *src, uint32_t numFrames,
                          const MP6AdpcmCoefTable *coef, MP6AdpcmState *state,
                          int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MP6_DSPADPCM_H */
