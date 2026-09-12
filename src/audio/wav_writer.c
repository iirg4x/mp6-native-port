/* See wav_writer.h. */
#include "wav_writer.h"
#include "mp6_utf8_file.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MP6_WAV_WRITER_SELFTEST
extern int mp6_wav_writer_test_fail_after_header;
#endif

static void put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put_u16le(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

int mp6_wav_write(const char *path, const int16_t *interleaved, uint32_t frames,
                   uint32_t sampleRate, uint32_t channels)
{
    FILE *f;
    unsigned char hdr[44];
    uint64_t dataBytes64;
    uint64_t byteRate64;
    uint64_t blockAlign64;
    uint32_t dataBytes;
    uint32_t byteRate;
    uint16_t blockAlign;
    int failed = 0;
    char *temporary = NULL;
    size_t pathLen;
    unsigned int attempt;

    if (path == NULL || path[0] == '\0' || sampleRate == 0 || channels == 0 ||
        channels > UINT16_MAX || (frames != 0 && interleaved == NULL)) {
        return -1;
    }
    blockAlign64 = (uint64_t)channels * sizeof(int16_t);
    dataBytes64 = (uint64_t)frames * blockAlign64;
    byteRate64 = (uint64_t)sampleRate * blockAlign64;
    if (blockAlign64 > UINT16_MAX || dataBytes64 > UINT32_MAX - 36u ||
        byteRate64 > UINT32_MAX) {
        return -1;
    }
    blockAlign = (uint16_t)blockAlign64;
    dataBytes = (uint32_t)dataBytes64;
    byteRate = (uint32_t)byteRate64;

    pathLen = strlen(path);
    if (pathLen > SIZE_MAX - 32u) return -1;
    temporary = (char *)malloc(pathLen + 32u);
    if (temporary == NULL) return -1;
    f = NULL;
    for (attempt = 0; attempt < 32u; ++attempt) {
        int written = snprintf(temporary, pathLen + 32u, "%s.tmp.%02u", path, attempt);
        if (written < 0 || (size_t)written >= pathLen + 32u) break;
        f = mp6_fopen_utf8(temporary, "wbx");
        if (f != NULL) break;
    }
    if (f == NULL) {
        free(temporary);
        return -1;
    }

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    put_u32le(hdr + 4, 36u + dataBytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_u32le(hdr + 16, 16); /* fmt chunk size */
    put_u16le(hdr + 20, 1);  /* PCM */
    put_u16le(hdr + 22, (uint16_t)channels);
    put_u32le(hdr + 24, sampleRate);
    put_u32le(hdr + 28, byteRate);
    put_u16le(hdr + 32, blockAlign);
    put_u16le(hdr + 34, 16); /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    put_u32le(hdr + 40, dataBytes);

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        failed = 1;
    }
#ifdef MP6_WAV_WRITER_SELFTEST
    if (!failed && mp6_wav_writer_test_fail_after_header) failed = 1;
#endif
    if (!failed && dataBytes > 0 &&
        fwrite(interleaved, 1, (size_t)dataBytes, f) != (size_t)dataBytes) {
        failed = 1;
    }
    if (fflush(f) != 0 || ferror(f)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        (void)mp6_remove_utf8(temporary);
        free(temporary);
        return -1;
    }
    if (mp6_replace_utf8(temporary, path) != 0) {
        (void)mp6_remove_utf8(temporary);
        free(temporary);
        return -1;
    }
    free(temporary);
    return 0;
}
