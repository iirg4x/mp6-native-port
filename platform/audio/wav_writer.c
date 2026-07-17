/* See wav_writer.h. */
#include "wav_writer.h"

#include <stdio.h>
#include <string.h>

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
    uint32_t dataBytes = frames * channels * (uint32_t)sizeof(int16_t);
    uint32_t byteRate = sampleRate * channels * (uint32_t)sizeof(int16_t);
    uint16_t blockAlign = (uint16_t)(channels * sizeof(int16_t));

    f = fopen(path, "wb");
    if (!f) return -1;

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
        fclose(f);
        return -1;
    }
    if (dataBytes > 0 && fwrite(interleaved, 1, dataBytes, f) != dataBytes) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
