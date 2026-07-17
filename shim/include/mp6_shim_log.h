/* MP6 native port -- shim logging helper.
 *
 * Every null-platform shim logs the SDK call it stands in for exactly once
 * (rate-limited per distinct call site, not per call). This header is
 * shared by the generated shims (platform/null/shims_generated.c) and the
 * hand-written ones (platform/null/shims_manual.c, platform/os/*.c).
 */
#ifndef MP6_SHIM_LOG_H
#define MP6_SHIM_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Prints "[SDK] family::name(...)" the first time it's called for a given
 * call site; every subsequent call from the SAME call site (thanks to the
 * `static` flag in MP6_LOG_ONCE) is silent. Safe to call redundantly. */
void mp6_shim_log(const char *family, const char *name);

#ifdef __cplusplus
}
#endif

#define MP6_LOG_ONCE(family, name)               \
    do {                                          \
        static int mp6_logged__ = 0;              \
        if (!mp6_logged__) {                      \
            mp6_logged__ = 1;                     \
            mp6_shim_log((family), (name));        \
        }                                          \
    } while (0)

#endif /* MP6_SHIM_LOG_H */
