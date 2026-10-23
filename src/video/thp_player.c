/* Native THPSimple adapter. One bounded compressed frame and two GX YUV frames
 * replace the hardware DVD/locked-cache/DMA rings. Reads open and close within
 * each call, so no host file handle survives a game tick or savestate. */
#include "game/THPSimple.h"
#include "game/THPDraw.h"
#include "mp6_dvd_files.h"
#include "mp6_thp_video.h"
#include "mp6_boot.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

UnkThpStruct10 SimpleControl;
s32 bufIdx;
static int initialized;
static char moviePath[256];
static uint32_t fileBytes;
static uint8_t *compressed, *jpeg, *rgbFrame;
static uint32_t generation, copiedGeneration;
static void *copiedDestination;
static unsigned copiedWidth, copiedHeight;
static size_t jpegCapacity;
static double frameClock;
static long lastTick;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int read_range(void *target, uint32_t length, uint32_t offset)
{
    DVDFileInfo file;
    if (offset > fileBytes || length > fileBytes - offset ||
        length > INT32_MAX || offset > INT32_MAX ||
        !mp6_dvd_open_by_path(moviePath, &file)) return 0;
    int ok = mp6_dvd_read(&file, target, (s32)length, (s32)offset);
    mp6_dvd_close(&file);
    return ok;
}

BOOL THPSimpleInit(s32 mode)
{
    (void)mode;
    if (SimpleControl.movieOpen) return FALSE;
    memset(&SimpleControl, 0, sizeof(SimpleControl));
    SimpleControl.videoBuffers[0].frameIndex = -1;
    compressed = jpeg = NULL;
    rgbFrame = NULL;
    generation = copiedGeneration = 0;
    copiedDestination = NULL;
    bufIdx = 0;
    initialized = 1;
    return TRUE;
}

void THPSimpleQuit(void) { initialized = 0; }

s32 THPSimpleOpen(char *path)
{
    uint8_t header[48], components[20], info[12];
    DVDFileInfo file;
    if (!initialized || SimpleControl.movieOpen || !path || strlen(path) >= sizeof(moviePath))
        return 0;
    if (!mp6_dvd_open_by_path(path, &file)) return 0;
    fileBytes = file.length;
    mp6_dvd_close(&file);
    strcpy(moviePath, path);
    if (!read_range(header, sizeof(header), 0) || memcmp(header, "THP\0", 4)) return 0;
    THPHeader *h = &SimpleControl.header;
    memcpy(h->mMagic, header, 4);
    h->mVersion = be32(header + 4);
    h->mBufferSize = be32(header + 8);
    h->mAudioMaxSamples = be32(header + 12);
    uint32_t rate = be32(header + 16);
    memcpy(&h->mFrameRate, &rate, sizeof(rate));
    h->mNumFrames = be32(header + 20);
    h->mFirstFrameSize = be32(header + 24);
    h->mMovieDataSize = be32(header + 28);
    h->mCompInfoDataOffsets = be32(header + 32);
    h->mOffsetDataOffsets = be32(header + 36);
    h->mMovieDataOffsets = be32(header + 40);
    h->mFinalFrameDataOffsets = be32(header + 44);
    if ((h->mVersion != 0x10000 && h->mVersion != 0x11000) ||
        !h->mNumFrames || h->mNumFrames > 1000000 ||
        !isfinite(h->mFrameRate) || h->mFrameRate < 1 || h->mFrameRate > 60 ||
        !h->mBufferSize || h->mBufferSize > 4 * 1024 * 1024 ||
        h->mMovieDataOffsets > fileBytes || h->mMovieDataSize > fileBytes - h->mMovieDataOffsets ||
        !read_range(components, sizeof(components), h->mCompInfoDataOffsets)) return 0;
    unsigned count = be32(components);
    if (!count || count > 16) return 0;
    SimpleControl.componentInfo.mNumComponents = count;
    memcpy(SimpleControl.componentInfo.mFrameComp, components + 4, 16);
    uint32_t offset = h->mCompInfoDataOffsets + sizeof(components);
    int video = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned type = components[4 + i];
        unsigned bytes = type == 0 ? (h->mVersion == 0x11000 ? 12 : 8) : 12;
        if (type > 1 || !read_range(info, bytes, offset)) return 0;
        if (type == 0) {
            SimpleControl.videoInfo.width = be32(info);
            SimpleControl.videoInfo.height = be32(info + 4);
            video++;
        } else {
            SimpleControl.hasAudio = 1;
            memcpy(SimpleControl.audioInfoData, info, 12);
        }
        offset += bytes;
    }
    unsigned w = SimpleControl.videoInfo.width, height = SimpleControl.videoInfo.height;
    if (video != 1 || !w || !height || w > 1024 || height > 1024 || (w & 15) || (height & 15))
        return 0;
    SimpleControl.movieOpen = 1;
    SimpleControl.videoBuffers[0].frameIndex = -1;
    return 1;
}

s32 THPSimpleRead(DVDFileInfo *file, void *target, s32 length, s32 offset)
{
    return length >= 0 && offset >= 0 && mp6_dvd_read(file, target, length, offset) ? length : -1;
}

s32 THPSimpleCalcNeedMemory(void)
{
    if (!SimpleControl.movieOpen) return 0;
    size_t pixels = (size_t)SimpleControl.videoInfo.width * SimpleControl.videoInfo.height;
    size_t encoded = (SimpleControl.header.mBufferSize + 31u) & ~(size_t)31;
    return (s32)(encoded * 3u + pixels * 6u + 256u);
}

s32 THPSimpleSetBuffer(void *buffer)
{
    if (!buffer || !SimpleControl.movieOpen) return 0;
    uintptr_t p = ((uintptr_t)buffer + 31) & ~(uintptr_t)31;
    size_t pixels = (size_t)SimpleControl.videoInfo.width * SimpleControl.videoInfo.height;
    size_t encoded = (SimpleControl.header.mBufferSize + 31u) & ~(size_t)31;
    compressed = (uint8_t *)p;
    jpeg = compressed + encoded;
    jpegCapacity = encoded * 2;
    p += encoded * 3;
    for (unsigned i = 0; i < 2; ++i) {
        SimpleControl.videoBuffers[i].yPlane = (void *)p; p += pixels;
        SimpleControl.videoBuffers[i].cbPlane = (void *)p; p += pixels / 4;
        SimpleControl.videoBuffers[i].crPlane = (void *)p; p += pixels / 4;
    }
    rgbFrame = (uint8_t *)p;
    return 1;
}

static int decode_next(void)
{
    THPHeader *h = &SimpleControl.header;
    unsigned count = SimpleControl.componentInfo.mNumComponents;
    uint32_t size = SimpleControl.nextFrameSize;
    uint32_t offset = SimpleControl.nextFrameOffset;
    if (!compressed || size > h->mBufferSize || size < 8 + count * 4 ||
        !read_range(compressed, size, offset)) return 1;
    uint32_t next = be32(compressed);
    size_t start = 8 + count * 4;
    int slot = 1 - bufIdx;
    for (unsigned i = 0; i < count; ++i) {
        size_t bytes = be32(compressed + 8 + i * 4);
        if (bytes > size - start) return 1;
        if (SimpleControl.componentInfo.mFrameComp[i] == 0 &&
            !mp6_thp_decode_video_rgb(compressed + start, bytes, jpeg, jpegCapacity,
                SimpleControl.videoBuffers[slot].yPlane,
                SimpleControl.videoBuffers[slot].cbPlane,
                SimpleControl.videoBuffers[slot].crPlane,
                SimpleControl.videoInfo.width, SimpleControl.videoInfo.height, rgbFrame)) return 1;
        start += bytes;
    }
    bufIdx = slot;
    SimpleControl.videoBuffers[0].frameIndex = SimpleControl.nextFrameNumber;
    ++generation;
    GXInvalidateTexAll();
    ++SimpleControl.nextFrameNumber;
    if ((uint32_t)SimpleControl.nextFrameNumber >= h->mNumFrames) {
        if (!SimpleControl.loopMovie) { SimpleControl.loadingActive = 0; return 0; }
        SimpleControl.nextFrameNumber = 0;
        SimpleControl.nextFrameOffset = h->mMovieDataOffsets;
        SimpleControl.nextFrameSize = h->mFirstFrameSize;
    } else {
        if (offset > UINT32_MAX - size || next < 8 + count * 4 || next > h->mBufferSize) return 1;
        SimpleControl.nextFrameOffset = offset + size;
        SimpleControl.nextFrameSize = next;
    }
    return 0;
}

s32 THPSimplePreLoad(s32 loop)
{
    if (!SimpleControl.movieOpen || !compressed) return 0;
    SimpleControl.loopMovie = loop == 1;
    SimpleControl.nextFrameNumber = 0;
    SimpleControl.nextFrameOffset = SimpleControl.header.mMovieDataOffsets;
    SimpleControl.nextFrameSize = SimpleControl.header.mFirstFrameSize;
    SimpleControl.loadingActive = 1;
    SimpleControl.videoBuffers[0].frameIndex = -1;
    frameClock = 0;
    lastTick = mp6_tick_count;
    return decode_next() == 0;
}

s32 THPSimpleDecode(void)
{
    if (!SimpleControl.loadingActive) return 0;
    long elapsed = mp6_tick_count - lastTick;
    lastTick = mp6_tick_count;
    if (elapsed < 0 || elapsed > 60) elapsed = 1;
    frameClock += elapsed * (double)SimpleControl.header.mFrameRate / 60.0;
    if (frameClock < 1.0) return 0;
    frameClock -= 1.0;
    return decode_next();
}

s32 THPSimpleLoadStop(void)
{
    SimpleControl.loadingActive = 0;
    SimpleControl.videoBuffers[0].frameIndex = -1;
    return 1;
}

/* These menu previews explicitly request volume zero. Audio chunks are skipped
 * while streaming video; native movie-audio mixing remains a separate adapter. */
void THPSimpleAudioStart(void) { SimpleControl.audioActive = 1; }
void THPSimpleAudioStop(void) { SimpleControl.audioActive = 0; }
s32 THPSimpleSetVolume(s32 volume, s32 duration)
{
    (void)duration;
    SimpleControl.currentVolume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    return SimpleControl.movieOpen;
}
s32 THPSimpleClose(void)
{
    SimpleControl.movieOpen = SimpleControl.loadingActive = SimpleControl.audioActive = 0;
    SimpleControl.videoBuffers[0].frameIndex = -1;
    compressed = jpeg = NULL;
    rgbFrame = NULL;
    return 1;
}

int mp6_thp_copy_frame_rgba8(void *destination, unsigned width, unsigned height)
{
    if (!destination || !width || !height || width > 1024 || height > 1024 ||
        (width & 3) || (height & 3)) return 0;
    /* The destination remains the last displayed frame during close/open. It
     * never receives an unrelated EFB copy while a new movie is loading. */
    if (!rgbFrame || SimpleControl.videoBuffers[0].frameIndex < 0) return 1;
    if (copiedDestination == destination && copiedGeneration == generation &&
        copiedWidth == width && copiedHeight == height) return 1;
    unsigned sw = SimpleControl.videoInfo.width, sh = SimpleControl.videoInfo.height;
    uint8_t *out = destination;
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned x0 = x * sw / width, x1 = (x + 1) * sw / width;
        unsigned y0 = y * sh / height, y1 = (y + 1) * sh / height;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        unsigned r = 0, g = 0, b = 0, samples = (x1 - x0) * (y1 - y0);
        for (unsigned sy = y0; sy < y1; ++sy) for (unsigned sx = x0; sx < x1; ++sx) {
            const uint8_t *pixel = rgbFrame + ((size_t)sy * sw + sx) * 3;
            r += pixel[0]; g += pixel[1]; b += pixel[2];
        }
        size_t at = ((size_t)(y / 4) * (width / 4) + x / 4) * 64 +
                    ((y % 4) * 4 + x % 4) * 2;
        out[at] = 255; out[at + 1] = (r + samples / 2) / samples;
        out[at + 32] = (g + samples / 2) / samples;
        out[at + 33] = (b + samples / 2) / samples;
    }
    copiedDestination = destination;
    copiedGeneration = generation;
    copiedWidth = width; copiedHeight = height;
    DCFlushRange(destination, width * height * 4);
    GXInvalidateTexAll();
    return 1;
}
s32 THPSimpleGetVideoInfo(UnkThpStruct04 *info)
{
    if (!SimpleControl.movieOpen || !info) return 0;
    *info = SimpleControl.videoInfo;
    return 1;
}
s32 THPSimpleGetTotalFrame(void) { return SimpleControl.header.mNumFrames; }
float THPSimpleGetFrameRate(void) { return SimpleControl.header.mFrameRate; }
s32 THPSimpleDrawCurrentFrame(GXRenderModeObj *mode, GXColor *color, Mtx matrix,
                              s32 x, s32 y, s32 width, s32 height)
{
    if (SimpleControl.videoBuffers[0].frameIndex < 0) return -1;
    THPGXYuv2RgbSetup(mode, color, matrix);
    THPGXYuv2RgbDraw(SimpleControl.videoBuffers[bufIdx].yPlane,
        SimpleControl.videoBuffers[bufIdx].cbPlane, SimpleControl.videoBuffers[bufIdx].crPlane,
        x, y, SimpleControl.videoInfo.width, SimpleControl.videoInfo.height, width, height);
    THPGXRestore();
    return SimpleControl.videoBuffers[0].frameIndex;
}
