#include "mp6_fi_timing.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const int64_t period = 1000000000 / 60;
    /* Immediate mode remains admissible past 1000 FPS, with no artificial
     * sleep. It still has to pass the identical absolute-deadline gate. */
    for (uint32_t i = 0; i < 10000; ++i) {
        assert(!mp6_fi_replay_limit_reached(0, i));
        assert(mp6_fi_replay_spacing_ns(0, period) == 0);
    }
    assert(!mp6_fi_replay_limit_reached(1, 7));
    assert(mp6_fi_replay_limit_reached(1, 8));
    assert(mp6_fi_replay_spacing_ns(1, period) == period / 9);
    assert(mp6_fi_replay_spacing_ns(1, 0) == 0);
    assert(mp6_fi_deadline_fits(1000, 500, 100, 400));
    assert(!mp6_fi_deadline_fits(1000, 501, 100, 400));
    assert(!mp6_fi_deadline_fits(1000, 1001, 0, 0));
    assert(!mp6_fi_deadline_fits(1000, 500, -1, 0));
    assert(!mp6_fi_deadline_fits(1000, 500, 0, -1));
    puts("PASS: uncapped/paced replay policy and absolute deadline admission");
    return 0;
}
