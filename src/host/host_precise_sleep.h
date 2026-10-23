/* MP6 native port -- the wake-margin bookkeeping behind
 * mp6_host_sleep_until_ns (host.h), shared by the win32 and Android
 * backends: each wraps its own OS timer (a high-resolution waitable timer,
 * clock_nanosleep) in mp6_precise_sleep().
 *
 * An OS timer wakes a little late, by an amount that depends on the machine
 * and its load, so the sleep aims that much early: the margin tracks the
 * largest recent oversleep (decaying, so one spike does not cost forever)
 * and the caller spins only what is left. */
#ifndef MP6_HOST_PRECISE_SLEEP_H
#define MP6_HOST_PRECISE_SLEEP_H

#include <stdint.h>
#include <stdlib.h>

#include "host.h"

#define MP6_PRECISE_SLEEP_MIN_MARGIN_NS 200000   /* 0.2 ms */
#define MP6_PRECISE_SLEEP_MAX_MARGIN_NS 2000000  /* 2 ms: never worse than the old spin window */
#define MP6_PRECISE_SLEEP_MIN_NS 50000           /* shorter waits are not worth a timer */

typedef struct {
    int64_t margin;    /* how early to aim */
    int64_t oversleep; /* decaying maximum of recent oversleeps */
} Mp6PreciseSleep;

/* MP6_PRECISE_SLEEP=0/1 overrides the backend's default. */
static inline int mp6_precise_sleep_enabled(int platformDefault)
{
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char *e = getenv("MP6_PRECISE_SLEEP");
        s_enabled = (e != NULL && e[0] != '\0') ? (e[0] != '0') : platformDefault;
    }
    return s_enabled;
}

/* Sleeps through `sleepFn(targetNs)` (an absolute-deadline OS wait) to
 * just before deadlineNs, then updates the margin from how late it woke. */
static inline void mp6_precise_sleep(Mp6PreciseSleep *s, int64_t deadlineNs,
                                     void (*sleepFn)(int64_t targetNs))
{
    int64_t now = (int64_t)mp6_host_monotonic_ns();
    int64_t target, late;
    if (s->margin == 0) s->margin = MP6_PRECISE_SLEEP_MAX_MARGIN_NS / 2;
    target = deadlineNs - s->margin;
    if (target - now < MP6_PRECISE_SLEEP_MIN_NS) return;
    sleepFn(target);
    late = (int64_t)mp6_host_monotonic_ns() - target;
    if (late < 0) late = 0;
    s->oversleep -= s->oversleep / 256;
    if (late > s->oversleep) s->oversleep = late;
    s->margin = s->oversleep + s->oversleep / 4 + MP6_PRECISE_SLEEP_MIN_MARGIN_NS;
    if (s->margin > MP6_PRECISE_SLEEP_MAX_MARGIN_NS) s->margin = MP6_PRECISE_SLEEP_MAX_MARGIN_NS;
}

#endif /* MP6_HOST_PRECISE_SLEEP_H */
