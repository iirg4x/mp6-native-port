/* MP6 native port -- checked fixed-buffer path construction.
 *
 * Runtime paths are operational targets, not display strings: silently
 * truncating one can open a different file or directory than the caller
 * validated.  These header-only helpers therefore have one contract:
 * return 0 only for the complete path, and clear the destination whenever
 * the requested result does not fit.
 */
#ifndef MP6_PATH_H
#define MP6_PATH_H

#include <stddef.h>
#include <string.h>

static inline int mp6_path_copy_checked(char *dst, size_t dstSize, const char *src)
{
    size_t len;
    if (dst == NULL || dstSize == 0) return -1;
    if (src == NULL) {
        dst[0] = '\0';
        return -1;
    }
    len = strlen(src);
    if (len >= dstSize) {
        dst[0] = '\0';
        return -1;
    }
    memmove(dst, src, len + 1);
    return 0;
}

/* Joins a base and relative path with exactly one separator at the seam.
 * Either slash spelling is recognized; a newly inserted separator is '/'.
 * The two input strings must not overlap the destination. */
static inline int mp6_path_join_checked(char *dst, size_t dstSize,
                                        const char *base, const char *relative)
{
    size_t baseLen, relativeLen, relativeSkip, tailLen, resultLen;
    int baseHasSep, relativeHasSep, addSep;

    if (dst == NULL || dstSize == 0) return -1;
    if (base == NULL || relative == NULL) {
        dst[0] = '\0';
        return -1;
    }

    baseLen = strlen(base);
    relativeLen = strlen(relative);
    baseHasSep = baseLen > 0 && (base[baseLen - 1] == '/' || base[baseLen - 1] == '\\');
    relativeHasSep = relativeLen > 0 && (relative[0] == '/' || relative[0] == '\\');
    relativeSkip = (baseHasSep && relativeHasSep) ? 1u : 0u;
    tailLen = relativeLen - relativeSkip;
    addSep = baseLen > 0 && relativeLen > 0 && !baseHasSep && !relativeHasSep;

    resultLen = baseLen;
    if (addSep) {
        if (resultLen == (size_t)-1) {
            dst[0] = '\0';
            return -1;
        }
        resultLen++;
    }
    if (tailLen > (size_t)-1 - resultLen) {
        dst[0] = '\0';
        return -1;
    }
    resultLen += tailLen;
    if (resultLen >= dstSize) {
        dst[0] = '\0';
        return -1;
    }

    memmove(dst, base, baseLen);
    if (addSep) dst[baseLen++] = '/';
    memmove(dst + baseLen, relative + relativeSkip, tailLen);
    dst[baseLen + tailLen] = '\0';
    return 0;
}

#endif /* MP6_PATH_H */
