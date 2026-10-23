#include <assert.h>
#include <math.h>
static float ratio;
static float mp6_widescreen_scale_factor(void) { return ratio; }
#include "badge_subject.inc"
int main(void)
{
    const float widths[] = {640, 848, 1120, 2560, 640};
    const float home = -260.0f;
    for (int w = 0; w < 5; ++w) {
        ratio = widths[w] / 640.0f;
        assert(mp6_filesel_badge_slide_x(home, 0) == home);
        for (int i = 0; i <= 100; ++i) {
            const float t = i / 100.0f;
            const float native = home - 200.0f * t;
            const float actual = mp6_filesel_badge_slide_x(home, t);
            if (ratio == 1) assert(actual == native);
            else assert(actual <= native);
        }
        assert(fabsf(mp6_filesel_badge_slide_x(home, 1) - (home - 200) * ratio) < 0.001f);
    }
    ratio = 0.75f;
    assert(mp6_filesel_badge_slide_x(home, 1) == home - 200);
    return 0;
}
