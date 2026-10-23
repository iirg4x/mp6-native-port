#include "mp6_thp_video.h"
#include <string.h>
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

static size_t tile(unsigned x, unsigned y, unsigned width)
{
    return ((size_t)(y / 4) * ((width + 7) / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8;
}

static uint8_t byte(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

int mp6_thp_decode_video_rgb(const uint8_t *frame, size_t bytes,
                        uint8_t *scratch, size_t capacity,
                        uint8_t *y, uint8_t *u, uint8_t *v,
                        unsigned width, unsigned height, uint8_t *rgbOut)
{
    if (!frame || !scratch || !y || !u || !v || bytes < 4 || bytes > capacity / 2 ||
        !width || !height || width > 1024 || height > 1024 ||
        (width & 15) || (height & 15) || frame[0] != 0xff || frame[1] != 0xd8)
        return 0;
    /* THP stores unescaped JPEG entropy bytes. Locate SOS by segment lengths,
     * then restore JPEG byte stuffing through the terminal EOI (before padding).
     * Never scan entropy for a marker: arbitrary FF xx values are legal there. */
    size_t scan = 2;
    int foundScan = 0;
    while (scan + 4 <= bytes) {
        if (frame[scan] != 0xff) return 0;
        unsigned marker = frame[scan + 1];
        size_t length = ((size_t)frame[scan + 2] << 8) | frame[scan + 3];
        if (length < 2 || length > bytes - scan - 2) return 0;
        scan += length + 2;
        if (marker == 0xda) { foundScan = 1; break; }
    }
    if (!foundScan || scan >= bytes) return 0;
    size_t end = bytes;
    while (end >= scan + 2 && !(frame[end - 2] == 0xff && frame[end - 1] == 0xd9)) --end;
    if (end < scan + 2) return 0;
    memcpy(scratch, frame, scan);
    size_t out = scan;
    for (size_t i = scan; i < end - 2; ++i) {
        scratch[out++] = frame[i];
        if (frame[i] == 0xff) scratch[out++] = 0;
    }
    scratch[out++] = 0xff;
    scratch[out++] = 0xd9;
    int decodedWidth, decodedHeight, components;
    unsigned char *rgb = stbi_load_from_memory(scratch, (int)out,
                                              &decodedWidth, &decodedHeight, &components, 3);
    if (!rgb) return 0;
    if (decodedWidth != (int)width || decodedHeight != (int)height) {
        stbi_image_free(rgb);
        return 0;
    }
    for (unsigned row = 0; row < height; ++row) {
        for (unsigned col = 0; col < width; ++col) {
            const uint8_t *p = rgb + ((size_t)row * width + col) * 3;
            y[tile(col, row, width)] = byte((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
        }
    }
    for (unsigned row = 0; row < height; row += 2) {
        for (unsigned col = 0; col < width; col += 2) {
            int r = 0, g = 0, b = 0;
            for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                const uint8_t *p = rgb + ((size_t)(row + dy) * width + col + dx) * 3;
                r += p[0]; g += p[1]; b += p[2];
            }
            size_t at = tile(col / 2, row / 2, width / 2);
            u[at] = byte(128 + ((-43 * r - 85 * g + 128 * b + 512) >> 10));
            v[at] = byte(128 + ((128 * r - 107 * g - 21 * b + 512) >> 10));
        }
    }
    if (rgbOut) memcpy(rgbOut, rgb, (size_t)width * height * 3);
    stbi_image_free(rgb);
    return 1;
}

int mp6_thp_decode_video(const uint8_t *frame, size_t bytes,
                        uint8_t *scratch, size_t capacity,
                        uint8_t *y, uint8_t *u, uint8_t *v,
                        unsigned width, unsigned height)
{
    return mp6_thp_decode_video_rgb(frame, bytes, scratch, capacity,
                                   y, u, v, width, height, NULL);
}
