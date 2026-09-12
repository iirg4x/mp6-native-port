#ifndef MP6_FI_TIMING_H
#define MP6_FI_TIMING_H

#include <stdint.h>

/* The count/spacing policy is only for display-paced presentation. An
 * unlocked immediate/mailbox window runs until its deadline or GPU pressure,
 * not a fixed number of frames (eight previously capped it below 540 FPS). */
#define MP6_FI_PACED_REPLAYS 8
static inline int mp6_fi_replay_limit_reached(int vsync, uint32_t replays)
{
    return vsync && replays >= MP6_FI_PACED_REPLAYS;
}

static inline int64_t mp6_fi_replay_spacing_ns(int vsync, int64_t period_ns)
{
    return vsync && period_ns > 0 ? period_ns / (MP6_FI_PACED_REPLAYS + 1) : 0;
}

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
