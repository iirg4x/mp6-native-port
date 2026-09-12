#ifndef MP6_TIME_REBASE_H
#define MP6_TIME_REBASE_H

#include <stdint.h>

#define MP6_OS_TIMER_HZ 40500000ll

/* Convert nanoseconds without multiplying the full 64-bit value first.
 * Splitting whole seconds/remainder avoids overflow and avoids the precision
 * loss of converting a long-running monotonic counter through double. */
static inline int64_t mp6_os_monotonic_ticks(uint64_t monotonicNs)
{
    uint64_t seconds = monotonicNs / 1000000000ull;
    uint64_t remainder = monotonicNs % 1000000000ull;
    return (int64_t)(seconds * (uint64_t)MP6_OS_TIMER_HZ
                   + (remainder * (uint64_t)MP6_OS_TIMER_HZ) / 1000000000ull);
}

/* Return 0 instead of evaluating a signed-underflowing subtraction.  A state
 * header is uncompressed and may be corrupt, so logicalTicks is not trusted
 * until this check succeeds. */
static inline int mp6_os_rebased_rtc_base(int64_t logicalTicks, uint64_t monotonicNs,
                                          int64_t *rtcBaseOut)
{
    int64_t elapsedTicks;
    if (rtcBaseOut == 0) return 0;
    elapsedTicks = mp6_os_monotonic_ticks(monotonicNs);
    if (logicalTicks < INT64_MIN + elapsedTicks) return 0;
    *rtcBaseOut = logicalTicks - elapsedTicks;
    return 1;
}

#endif /* MP6_TIME_REBASE_H */
