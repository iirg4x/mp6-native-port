#ifndef MP6_SAF_SAFE_H
#define MP6_SAF_SAFE_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int mp6_saf_copy_preserve(char *dst, size_t dstSize, const char *src)
{
    size_t len;
    if (dst == NULL || dstSize == 0 || src == NULL) return -1;
    len = strlen(src);
    if (len >= dstSize) return -1;
    memmove(dst, src, len + 1);
    return 0;
}

/* Java returns {state, bytesDone, bytesTotal, filesDone, filesTotal}. */
static inline int mp6_saf_progress_values_valid(const int64_t vals[5])
{
    if (vals == NULL || vals[0] < 0 || vals[0] > 4 ||
        vals[1] < 0 || vals[2] < 0 || vals[3] < 0 || vals[4] < 0 ||
        vals[3] > INT_MAX || vals[4] > INT_MAX) return 0;
    if ((vals[2] == 0 && vals[1] != 0) || vals[1] > vals[2]) return 0;
    if ((vals[4] == 0 && vals[3] != 0) || vals[3] > vals[4]) return 0;
    if (vals[0] == 0 && (vals[1] != 0 || vals[2] != 0 ||
                         vals[3] != 0 || vals[4] != 0)) return 0;
    if (vals[0] == 2 && (vals[2] == 0 || vals[4] == 0 ||
                         vals[1] != vals[2] || vals[3] != vals[4])) return 0;
    return 1;
}

#endif /* MP6_SAF_SAFE_H */
