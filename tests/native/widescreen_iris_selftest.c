#include <assert.h>
#include <math.h>

static float delta;
static float mp6_widescreen_half_width_delta(void) { return delta; }
#include "iris_subject.inc"

int main(void)
{
    const int widths[] = {640, 848, 1120, 2560, 640};
    const float nativeDiagonal = sqrtf(288.0f*288.0f + 240.0f*240.0f);
    for (unsigned i=0; i<sizeof(widths)/sizeof(widths[0]); ++i) {
        delta = (576.0f * widths[i] / 640.0f - 576.0f) * 0.5f;
        float halfWidth = 288.0f + delta;
        float diagonal = sqrtf(halfWidth*halfWidth + 240.0f*240.0f);
        for (int frame=0; frame<=45; ++frame) {
            float size = 1000.0f * frame / 45.0f;
            float radius = WipeCircleHalfSize(size);
            if (widths[i] == 640) assert(radius == 0.5f*size);
            assert(fabsf(radius/diagonal - 0.5f*size/nativeDiagonal) < 0.000001f);
            if (frame >= 35) assert(radius > diagonal);
        }
    }
    delta = -30.0f;
    assert(WipeCircleHalfSize(400.0f) == 200.0f);
    return 0;
}
