#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef int16_t s16;
typedef float Mtx[3][4];
#define HU_DISP_WIDTH 576
/* WIDE_BACKDROP_ATTRIBUTE */
static int enabled;
static float delta;
static int mp6_widescreen_enabled(void) { return enabled; }
static float mp6_widescreen_half_width_delta(void) { return delta; }
#include "backdrop_subject.inc"

int main(void)
{
    /* Both authored backings cover 640 HUD units, centred at 288, with
     * vertical overscan. At 16:9 the canvas is 768: the old hole is 64/side. */
    const Mtx native = {{40, 0, 0, 288}, {0, 40, 0, 240}, {0, 0, 1, 0}};
    Mtx matrix;
    const float deltas[] = {0, 96, 216, 96, 0, 216};
    for (unsigned i = 0; i < sizeof(deltas)/sizeof(deltas[0]); ++i) {
        delta = deltas[i];
        for (enabled = 0; enabled <= 1; ++enabled) {
            memcpy(matrix, native, sizeof matrix);
            mp6SprWideBackdrop(matrix, 0);
            assert(memcmp(matrix, native, sizeof matrix) == 0);
            memcpy(matrix, native, sizeof matrix);
            mp6SprWideBackdrop(matrix, MP6_HUSPR_ATTR_WIDE_BACKDROP);
            assert(memcmp(matrix[1], native[1], 2 * sizeof matrix[1]) == 0);
            assert(matrix[0][3] == 288);
            if (!enabled || delta == 0) {
                assert(memcmp(matrix, native, sizeof matrix) == 0);
            } else {
                float left = matrix[0][3] - 8 * matrix[0][0];
                float right = matrix[0][3] + 8 * matrix[0][0];
                assert(left <= -delta && right >= HU_DISP_WIDTH + delta);
                /* Negative control: the original backing misses the wings. */
                assert(288 - 8 * native[0][0] > -delta);
                assert(288 + 8 * native[0][0] < HU_DISP_WIDTH + delta);
            }
        }
    }
    return 0;
}
