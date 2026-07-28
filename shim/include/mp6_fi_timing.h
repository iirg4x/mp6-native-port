#ifndef MP6_FI_TIMING_H
#define MP6_FI_TIMING_H

#include <stdint.h>

/* Pure deadline predicate shared by the runtime and deterministic self-test.
 * Subtraction is used only after ordering checks, avoiding signed overflow. */
static inline int mp6_fi_deadline_fits(int64_t deadline_ns, int64_t now_ns,
                                       int64_t margin_ns, int64_t budget_ns)
{
    int64_t slack;
    if (deadline_ns < now_ns || margin_ns < 0 || budget_ns < 0) return 0;
    slack = deadline_ns - now_ns;
    if (slack < margin_ns) return 0;
    return slack - margin_ns >= budget_ns;
}

#endif /* MP6_FI_TIMING_H */
