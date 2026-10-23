/* MP6 native port -- the model loader's input check, callable off the game thread.
 *
 * src/hsf/hsf_load_native.c validates every decoded HSF file before reading
 * it (hsf_validate_internal.h). The check depends only on the bytes, so
 * decode-ahead's workers run it on the models they decode, and the loader
 * reuses the result for the buffer decode-ahead just filled
 * (mp6_decode_ahead_hsf_preflight) instead of checking it again on the game
 * thread.
 */
#ifndef MP6_HSF_PREFLIGHT_H
#define MP6_HSF_PREFLIGHT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mp6HsfPreflight {
    uint64_t logicalSize;    /* bytes the file really uses (string table end) */
    uint32_t stringOffset;
    uint32_t stringSize;
    uint32_t oobFaceCorners; /* face corners the loader clamps */
} Mp6HsfPreflight;

/* 1 when the `capacity` bytes at data are a model the loader can read; any thread. */
int mp6_hsf_preflight_bytes(const void *data, size_t capacity, Mp6HsfPreflight *out);

/* 1 when decode-ahead filled `data` with a model it already checked, this
 * tick; *out is that result (src/os/decode_ahead.c). */
int mp6_decode_ahead_hsf_preflight(const void *data, size_t capacity, Mp6HsfPreflight *out);

#ifdef __cplusplus
}
#endif

#endif
