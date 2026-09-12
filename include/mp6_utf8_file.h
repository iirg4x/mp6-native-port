/* UTF-8 operational file paths. Windows uses wide CRT/Win32 APIs explicitly.
 *
 * This port does not yet opt into extended-length Windows paths, so the
 * supported Windows contract is deliberately capped at 259 UTF-16 code units
 * (plus NUL). Longer targets fail closed rather than being truncated or
 * accidentally relying on process-manifest state.
 */
#ifndef MP6_UTF8_FILE_H
#define MP6_UTF8_FILE_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>

#define MP6_WINDOWS_PATH_WCHARS 260

static inline int mp6_wide_is_device_namespace(const wchar_t *path)
{
    if (path == NULL) return 0;
    return (path[0] == L'\\' || path[0] == L'/') &&
           (path[1] == L'\\' || path[1] == L'/') &&
           (path[2] == L'?' || path[2] == L'.') &&
           (path[3] == L'\\' || path[3] == L'/');
}

static inline int mp6_utf8_to_wide_path(const char *src, wchar_t *dst, size_t dstCount)
{
    int needed;
    if (src == NULL || dst == NULL || dstCount == 0 || dstCount > (size_t)INT_MAX) return -1;
    needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (needed <= 0 || needed > MP6_WINDOWS_PATH_WCHARS || (size_t)needed > dstCount) return -1;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst,
                               (int)dstCount) == needed ? 0 : -1;
}

static inline int mp6_wide_to_utf8_path(const wchar_t *src, char *dst, size_t dstSize)
{
    size_t wideLen;
    int needed;
    if (src == NULL || dst == NULL || dstSize == 0 || dstSize > (size_t)INT_MAX) return -1;
    wideLen = wcslen(src);
    if (wideLen >= MP6_WINDOWS_PATH_WCHARS) return -1;
    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                                 NULL, 0, NULL, NULL);
    if (needed <= 0 || (size_t)needed > dstSize) return -1;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst,
                               (int)dstSize, NULL, NULL) == needed ? 0 : -1;
}

/* Resolve an operational target before enforcing MAX_PATH. Checking only the
 * spelling handed to a Win32 API is not enough: a short relative name can
 * resolve past 259 UTF-16 code units under a long current directory. Keep
 * extended/device namespace paths out of this deliberately non-long-path-aware
 * port as well, even when their literal spelling happens to fit. */
static inline int mp6_utf8_to_wide_full_path(const char *src, wchar_t *dst,
                                              size_t dstCount)
{
    wchar_t relativeOrAbsolute[MP6_WINDOWS_PATH_WCHARS];
    if (dst == NULL || dstCount < MP6_WINDOWS_PATH_WCHARS ||
        mp6_utf8_to_wide_path(src, relativeOrAbsolute,
                              MP6_WINDOWS_PATH_WCHARS) != 0) {
        return -1;
    }
    if (mp6_wide_is_device_namespace(relativeOrAbsolute)) return -1;
    if (_wfullpath(dst, relativeOrAbsolute, MP6_WINDOWS_PATH_WCHARS) == NULL) return -1;
    return mp6_wide_is_device_namespace(dst) ? -1 : 0;
}

static inline FILE *mp6_fopen_utf8(const char *path, const char *mode)
{
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    wchar_t wideMode[16];
    if (mp6_utf8_to_wide_full_path(path, widePath, MP6_WINDOWS_PATH_WCHARS) != 0 ||
        mp6_utf8_to_wide_path(mode, wideMode, sizeof(wideMode) / sizeof(wideMode[0])) != 0) {
        return NULL;
    }
    return _wfopen(widePath, wideMode);
}

static inline int mp6_utf8_path_supported(const char *path)
{
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    return mp6_utf8_to_wide_full_path(path, widePath, MP6_WINDOWS_PATH_WCHARS) == 0;
}

static inline int mp6_remove_utf8(const char *path)
{
    wchar_t widePath[MP6_WINDOWS_PATH_WCHARS];
    if (mp6_utf8_to_wide_full_path(path, widePath, MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    return _wremove(widePath);
}

static inline int mp6_replace_utf8(const char *replacement, const char *destination)
{
    wchar_t wideReplacement[MP6_WINDOWS_PATH_WCHARS];
    wchar_t wideDestination[MP6_WINDOWS_PATH_WCHARS];
    if (mp6_utf8_to_wide_full_path(replacement, wideReplacement,
                                   MP6_WINDOWS_PATH_WCHARS) != 0 ||
        mp6_utf8_to_wide_full_path(destination, wideDestination,
                                   MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    return MoveFileExW(wideReplacement, wideDestination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

static inline int mp6_fullpath_utf8(char *dst, size_t dstSize, const char *path)
{
    wchar_t wideFull[MP6_WINDOWS_PATH_WCHARS];
    if (mp6_utf8_to_wide_full_path(path, wideFull,
                                   MP6_WINDOWS_PATH_WCHARS) != 0) return -1;
    return mp6_wide_to_utf8_path(wideFull, dst, dstSize);
}

#else

static inline FILE *mp6_fopen_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static inline int mp6_utf8_path_supported(const char *path)
{
    return path != NULL;
}

static inline int mp6_remove_utf8(const char *path)
{
    return remove(path);
}

static inline int mp6_replace_utf8(const char *replacement, const char *destination)
{
    return rename(replacement, destination);
}

static inline int mp6_fullpath_utf8(char *dst, size_t dstSize, const char *path)
{
    (void)dst;
    (void)dstSize;
    (void)path;
    return -1;
}

#endif

#endif /* MP6_UTF8_FILE_H */
