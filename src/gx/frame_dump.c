/* MP6 native port -- debug lever: MP6_FRAME_DUMP. See include/
 * mp6_frame_dump.h for the full contract, what the captured pixels are,
 * and why an in-engine per-present capture is the only instrument that can
 * settle a flicker question at all.
 *
 * Lives in src/gx/ next to framescope.c and shadow_dump.c -- the same
 * category of thing: env-gated verification tooling hanging off the frame
 * boundary. Like shadow_dump.c it is one TU compiled into BOTH build modes
 * (tools/build.py's PLATFORM_SOURCES_COMMON) and split internally by
 * #ifdef, because its trigger entry point is called from src/os/
 * mp6_events.c, which exists in both.
 *
 * HOST STATICS (carved): every static below -- the env latch, the armed
 * state, the frame counter, the readback scratch buffer's heap pointer --
 * describes the RUNNING process's debug session, never game state. A
 * savestate that restored them would reinstate a capturing process's heap
 * pointer and its capture position (tools/build.py's
 * HOST_STATE_SECTION_SOURCES lists this file for exactly that reason,
 * same as framescope.c and shadow_dump.c).
 */
#include "mp6_frame_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_host_section.h"

#if defined(MP6_HEADLESS_BUILD) || defined(__ANDROID__)

/* Standing no-ops in two builds:
 *  - headless: no renderer, no GPU, no present source to read back.
 *  - Android: the Android aurora archive does not carry aurora-patches/
 *    0025's aurora_gx_debug_capture_present_frame, so referencing it here
 *    would be an undefined symbol at the Android link. Same rule
 *    shadow_dump.c documents for aurora-patches/0016: keep desktop-debug
 *    levers that call an aurora debug-patch symbol out of the Android TU. */
void mp6_frame_dump_present(int replayFrame) { (void)replayFrame; }
void mp6_frame_dump_trigger(const char *text) { (void)text; }
int mp6_frame_dump_active(void) { return 0; }

#else

#include "host.h"           /* mp6_host_mkdir, mp6_host_monotonic_ns */
#include "mp6_parse.h"      /* mp6_parse_i32_strict */
#include "mp6_path.h"       /* mp6_path_copy_checked */
#include "mp6_utf8_file.h"  /* mp6_fopen_utf8 */
#include "mp6_console.h"    /* the MP6_FRAME_DUMP runtime lever */

extern long mp6_tick_count;

/* Aurora extension entry point (external_refs/repos/aurora,
 * aurora-patches/0025): declared locally instead of including an aurora
 * header, the same C-linkage seam shadow_dump.c uses for 0016's
 * aurora_gx_debug_dump_copy_png (this TU compiles with the decomp's own
 * dolphin headers, not aurora's).
 *
 * Reads back aurora's present source -- the exact texture end_frame()
 * blits to the swapchain -- into `out` as tightly packed w*4-byte rows.
 * Pass out=NULL to query geometry only. The rect (0,0,0,0 = whole frame)
 * is applied IN THE GPU COPY, which is why MP6_FRAME_DUMP_RECT is the
 * lever that decides whether a burst can coexist with the Unlocked-FPS
 * idle window rather than starving it. Returns the captured image size in
 * bytes (w*h*4), with data written only when out != NULL and outCapacity
 * was at least that; 0 on any failure. Blocking: it stalls this thread for
 * a whole GPU readback. `outFormat` is aurora-patches/0025's own stable
 * code, not a wgpu enum value: 1=RGBA8Unorm 2=RGBA8UnormSrgb
 * 3=BGRA8Unorm 4=BGRA8UnormSrgb. */
extern unsigned int aurora_gx_debug_capture_present_frame(void *out, unsigned int outCapacity,
                                                          unsigned int rectX, unsigned int rectY,
                                                          unsigned int rectW, unsigned int rectH,
                                                          unsigned int *outSrcWidth,
                                                          unsigned int *outSrcHeight,
                                                          unsigned int *outWidth,
                                                          unsigned int *outHeight,
                                                          unsigned int *outFormat);

#define FD_COUNT_DEFAULT 240
#define FD_COUNT_MAX     100000
#define FD_HEADER_BYTES  96
#define FD_TRIGGER_MAX   96

/* -2 = env not parsed yet, -1 = disabled, 1 = enabled. */
static int   s_fdEnabled = -2;
static char  s_fdDir[900];
static char  s_fdTrigger[FD_TRIGGER_MAX];
static int   s_fdWantCount = FD_COUNT_DEFAULT;
static int   s_fdDelay = 0;
static int   s_fdStride = 1;
static int   s_fdDownscale = 1;
static int   s_fdRectX = 0, s_fdRectY = 0, s_fdRectW = 0, s_fdRectH = 0; /* w/h 0 = full frame */

static int   s_fdArmed = 0;        /* trigger fired (or none required) */
static int   s_fdDone = 0;         /* burst finished; never re-arms in one run */
static int   s_fdWritten = 0;      /* frames written so far */
static int   s_fdSkipLeft = 0;     /* MP6_FRAME_DUMP_DELAY countdown */
static int   s_fdStrideCounter = 0;
static unsigned int s_fdPresentIndex = 0; /* global present counter, counts always */

static unsigned char *s_fdBuf = NULL;      /* readback scratch (source-size) */
static unsigned int   s_fdBufBytes = 0;
static unsigned char *s_fdOut = NULL;      /* post-crop/downscale staging */
static unsigned int   s_fdOutBytes = 0;
static FILE          *s_fdIndex = NULL;

static int fd_env_int(const char *name, int fallback, int minValue, int maxValue)
{
    const char *value = getenv(name);
    int parsed;
    if (value == NULL || value[0] == '\0') return fallback;
    if (!mp6_parse_i32_strict(value, minValue, maxValue, &parsed)) {
        printf("[FRAMEDUMP] %s='%s' is invalid (expected %d..%d) -- using %d\n",
               name, value, minValue, maxValue, fallback);
        return fallback;
    }
    return parsed;
}

/* "x,y,w,h", all non-negative. Anything malformed leaves the crop unset
 * (full frame) with a loud line -- a typo must not silently capture a
 * different region than the one the investigation is about. */
static void fd_parse_rect(void)
{
    const char *value = getenv("MP6_FRAME_DUMP_RECT");
    int v[4];
    int i;
    const char *p;
    if (value == NULL || value[0] == '\0') return;
    p = value;
    for (i = 0; i < 4; i++) {
        char field[16];
        size_t n = 0;
        while (*p != '\0' && *p != ',' && n + 1 < sizeof(field)) field[n++] = *p++;
        field[n] = '\0';
        if (!mp6_parse_i32_strict(field, 0, 1 << 20, &v[i])) {
            printf("[FRAMEDUMP] MP6_FRAME_DUMP_RECT='%s' is invalid (expected x,y,w,h) -- capturing the full frame\n",
                   value);
            return;
        }
        if (i < 3) {
            if (*p != ',') {
                printf("[FRAMEDUMP] MP6_FRAME_DUMP_RECT='%s' is invalid (expected x,y,w,h) -- capturing the full frame\n",
                       value);
                return;
            }
            p++;
        }
    }
    if (v[2] == 0 || v[3] == 0) {
        printf("[FRAMEDUMP] MP6_FRAME_DUMP_RECT='%s' has a zero extent -- capturing the full frame\n", value);
        return;
    }
    s_fdRectX = v[0]; s_fdRectY = v[1]; s_fdRectW = v[2]; s_fdRectH = v[3];
}

static void fd_parse_env(void)
{
    const char *dir = getenv("MP6_FRAME_DUMP");
    if (dir == NULL || dir[0] == '\0') {
        s_fdEnabled = -1;
        return;
    }
    if (mp6_path_copy_checked(s_fdDir, sizeof(s_fdDir), dir) != 0) {
        printf("[FRAMEDUMP] MP6_FRAME_DUMP path is too long -- capture disabled\n");
        s_fdEnabled = -1;
        return;
    }
    if (mp6_host_mkdir(s_fdDir) != 0) {
        printf("[FRAMEDUMP] cannot create/open output directory '%s' -- capture disabled\n", s_fdDir);
        s_fdEnabled = -1;
        return;
    }
    s_fdWantCount = fd_env_int("MP6_FRAME_DUMP_COUNT", FD_COUNT_DEFAULT, 1, FD_COUNT_MAX);
    s_fdDelay     = fd_env_int("MP6_FRAME_DUMP_DELAY", 0, 0, 1000000);
    s_fdStride    = fd_env_int("MP6_FRAME_DUMP_STRIDE", 1, 1, 1000);
    s_fdDownscale = fd_env_int("MP6_FRAME_DUMP_DOWNSCALE", 1, 1, 64);
    fd_parse_rect();
    s_fdTrigger[0] = '\0';
    {
        const char *trig = getenv("MP6_FRAME_DUMP_TRIGGER");
        if (trig != NULL && trig[0] != '\0') {
            if (mp6_path_copy_checked(s_fdTrigger, sizeof(s_fdTrigger), trig) != 0) {
                printf("[FRAMEDUMP] MP6_FRAME_DUMP_TRIGGER is too long (max %d) -- arming immediately instead\n",
                       FD_TRIGGER_MAX - 1);
                s_fdTrigger[0] = '\0';
            }
        }
    }
    s_fdSkipLeft = s_fdDelay;
    s_fdArmed = (s_fdTrigger[0] == '\0');
    s_fdEnabled = 1;
    printf("[FRAMEDUMP] enabled: dir='%s' count=%d stride=%d delay=%d downscale=%d rect=%d,%d,%d,%d trigger='%s'\n",
           s_fdDir, s_fdWantCount, s_fdStride, s_fdDelay, s_fdDownscale,
           s_fdRectX, s_fdRectY, s_fdRectW, s_fdRectH,
           s_fdTrigger[0] != '\0' ? s_fdTrigger : "(none: arms at first present)");
    fflush(stdout);
}

static int fd_enabled(void)
{
    if (s_fdEnabled == -2) fd_parse_env();
    /* env decides the output directory and the burst's shape, so the console
     * cannot ARM a dump that MP6_FRAME_DUMP never configured -- it can only
     * suppress one that is configured (`set framedump 0`). That asymmetry is
     * deliberate: this lever costs ~9.8 ms/frame of GPU readback and eats the
     * whole idle window (include/mp6_frame_dump.h), so the useful runtime
     * control is the OFF switch, and turning it ON needs a destination. */
    return mp6_console_cvar_get(MP6_CVAR_FRAME_DUMP, s_fdEnabled == 1) == 1 &&
           s_fdEnabled == 1;
}

int mp6_frame_dump_active(void)
{
    return fd_enabled() && s_fdArmed && !s_fdDone;
}

void mp6_frame_dump_trigger(const char *text)
{
    if (!fd_enabled() || s_fdArmed || s_fdDone || text == NULL) return;
    if (s_fdTrigger[0] == '\0') return;
    if (strstr(text, s_fdTrigger) == NULL) return;
    s_fdArmed = 1;
    printf("[FRAMEDUMP] ARMED by '%s' (matched trigger '%s') at tick %ld, present %u -- capturing %d frames\n",
           text, s_fdTrigger, mp6_tick_count, s_fdPresentIndex, s_fdWantCount);
    fflush(stdout);
}

static void fd_put_u32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void fd_put_u64(unsigned char *p, unsigned long long v)
{
    fd_put_u32(p, (unsigned int)(v & 0xffffffffull));
    fd_put_u32(p + 4, (unsigned int)((v >> 32) & 0xffffffffull));
}

static void fd_finish(const char *why)
{
    if (s_fdDone) return;
    s_fdDone = 1;
    if (s_fdIndex != NULL) { fclose(s_fdIndex); s_fdIndex = NULL; }
    free(s_fdBuf); s_fdBuf = NULL; s_fdBufBytes = 0;
    free(s_fdOut); s_fdOut = NULL; s_fdOutBytes = 0;
    printf("[FRAMEDUMP] burst finished (%s): %d frames in '%s'\n", why, s_fdWritten, s_fdDir);
    fflush(stdout);
}

/* Integer point-decimate an already-cropped cw*ch image into s_fdOut (the
 * crop itself happened GPU-side in the copy, see the extern above).
 * Returns 0 and sets *outW/*outH on success. Point sampling (not
 * averaging) is deliberate: averaging would smear exactly the
 * single-pixel/single-frame differences this capture exists to find. */
static int fd_downscale(const unsigned char *src, unsigned int cw, unsigned int ch,
                        unsigned int *outW, unsigned int *outH)
{
    unsigned int step = (unsigned int)s_fdDownscale;
    unsigned int w, h, y, x;
    unsigned long long bytes;

    if (cw == 0 || ch == 0) return -1;
    if (step == 1) {
        /* No decimation: the readback buffer is already the exact image. */
        *outW = cw;
        *outH = ch;
        return 0;
    }
    w = (cw + step - 1) / step;
    h = (ch + step - 1) / step;
    if (w == 0 || h == 0) return -1;

    bytes = (unsigned long long)w * h * 4ull;
    if (bytes > 0x40000000ull) return -1; /* 1 GiB sanity ceiling */
    if (s_fdOutBytes < (unsigned int)bytes) {
        unsigned char *grown = (unsigned char *)realloc(s_fdOut, (size_t)bytes);
        if (grown == NULL) return -1;
        s_fdOut = grown;
        s_fdOutBytes = (unsigned int)bytes;
    }
    for (y = 0; y < h; y++) {
        const unsigned char *srcRow = src + (unsigned long long)(y * step) * cw * 4ull;
        unsigned char *dstRow = s_fdOut + (unsigned long long)y * w * 4ull;
        for (x = 0; x < w; x++) {
            memcpy(dstRow + (size_t)x * 4u, srcRow + (size_t)(x * step) * 4u, 4u);
        }
    }
    *outW = w;
    *outH = h;
    return 0;
}

void mp6_frame_dump_present(int replayFrame)
{
    unsigned int srcW = 0, srcH = 0, cropW = 0, cropH = 0, fmt = 0, need;
    unsigned int outW = 0, outH = 0;
    unsigned long long wallNs;
    unsigned char header[FD_HEADER_BYTES];
    char path[1024];
    FILE *f;

    if (!fd_enabled()) return;
    s_fdPresentIndex++;
    if (!s_fdArmed || s_fdDone) return;
    if (s_fdSkipLeft > 0) { s_fdSkipLeft--; return; }
    if (s_fdStride > 1) {
        if (s_fdStrideCounter++ % s_fdStride != 0) return;
    }

    /* Query geometry first (out=NULL costs no readback), then size the
     * scratch buffer. The framebuffer can be resized mid-burst by a window
     * drag or a live SSAA change, so this re-checks every frame instead of
     * latching once. */
    need = aurora_gx_debug_capture_present_frame(NULL, 0,
                                                 (unsigned int)s_fdRectX, (unsigned int)s_fdRectY,
                                                 (unsigned int)s_fdRectW, (unsigned int)s_fdRectH,
                                                 &srcW, &srcH, &cropW, &cropH, &fmt);
    if (need == 0 || cropW == 0 || cropH == 0) {
        /* No present source yet (pre-first-frame) -- not an error, just not
         * capturable. Do not consume a slot. */
        return;
    }
    if (s_fdBufBytes < need) {
        unsigned char *grown = (unsigned char *)realloc(s_fdBuf, (size_t)need);
        if (grown == NULL) {
            fd_finish("out of memory sizing the readback buffer");
            return;
        }
        s_fdBuf = grown;
        s_fdBufBytes = need;
    }
    if (aurora_gx_debug_capture_present_frame(s_fdBuf, s_fdBufBytes,
                                              (unsigned int)s_fdRectX, (unsigned int)s_fdRectY,
                                              (unsigned int)s_fdRectW, (unsigned int)s_fdRectH,
                                              &srcW, &srcH, &cropW, &cropH, &fmt) == 0) {
        printf("[FRAMEDUMP] readback failed at present %u -- stopping the burst\n", s_fdPresentIndex);
        fd_finish("readback failure");
        return;
    }
    if (fd_downscale(s_fdBuf, cropW, cropH, &outW, &outH) != 0) {
        printf("[FRAMEDUMP] downscale of %ux%u by %d failed -- stopping the burst\n",
               cropW, cropH, s_fdDownscale);
        fd_finish("downscale failure");
        return;
    }

    wallNs = (unsigned long long)mp6_host_monotonic_ns();

    if (s_fdIndex == NULL) {
        char indexPath[1024];
        if (mp6_path_join_checked(indexPath, sizeof(indexPath), s_fdDir, "index.csv") == 0) {
            s_fdIndex = mp6_fopen_utf8(indexPath, "w");
        }
        if (s_fdIndex != NULL) {
            fprintf(s_fdIndex, "seq,present_index,tick,wall_ns,replay,width,height,"
                               "src_width,src_height,crop_x,crop_y,crop_w,crop_h,downscale,format\n");
        }
    }

    {
        char name[32];
        snprintf(name, sizeof(name), "f%06d.mfd", s_fdWritten);
        if (mp6_path_join_checked(path, sizeof(path), s_fdDir, name) != 0) {
            fd_finish("output path too long");
            return;
        }
    }

    memset(header, 0, sizeof(header));
    memcpy(header, "MP6FDUMP", 8);
    fd_put_u32(header + 8, 1u);                          /* version */
    fd_put_u32(header + 12, outW);
    fd_put_u32(header + 16, outH);
    fd_put_u32(header + 20, 4u);                         /* bytes per pixel */
    fd_put_u32(header + 24, fmt);                        /* aurora-patches/0025 format code */
    fd_put_u32(header + 28, (unsigned int)s_fdWritten);  /* seq */
    fd_put_u32(header + 32, s_fdPresentIndex);
    fd_put_u32(header + 36, (unsigned int)(replayFrame ? 1 : 0));
    fd_put_u64(header + 40, (unsigned long long)mp6_tick_count);
    fd_put_u64(header + 48, wallNs);
    fd_put_u32(header + 56, srcW);
    fd_put_u32(header + 60, srcH);
    fd_put_u32(header + 64, (unsigned int)s_fdRectX);
    fd_put_u32(header + 68, (unsigned int)s_fdRectY);
    fd_put_u32(header + 72, cropW);
    fd_put_u32(header + 76, cropH);
    fd_put_u32(header + 80, (unsigned int)s_fdDownscale);
    /* 84..95 reserved, left zero by the memset above. */

    f = mp6_fopen_utf8(path, "wb");
    if (f == NULL) {
        printf("[FRAMEDUMP] cannot open '%s' for writing -- stopping the burst\n", path);
        fd_finish("output open failure");
        return;
    }
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(s_fdDownscale == 1 ? s_fdBuf : s_fdOut, 1, (size_t)outW * outH * 4u, f)
            != (size_t)outW * outH * 4u) {
        fclose(f);
        printf("[FRAMEDUMP] short write on '%s' (disk full?) -- stopping the burst\n", path);
        fd_finish("short write");
        return;
    }
    fclose(f);

    if (s_fdIndex != NULL) {
        fprintf(s_fdIndex, "%d,%u,%ld,%llu,%d,%u,%u,%u,%u,%d,%d,%u,%u,%d,%u\n",
                s_fdWritten, s_fdPresentIndex, mp6_tick_count, wallNs,
                replayFrame ? 1 : 0, outW, outH, srcW, srcH,
                s_fdRectX, s_fdRectY, cropW, cropH, s_fdDownscale, fmt);
    }

    s_fdWritten++;
    if (s_fdWritten == 1) {
        printf("[FRAMEDUMP] first frame captured: %ux%u (crop %ux%u at %d,%d of %ux%u, /%d) fmt=%u -> %s\n",
               outW, outH, cropW, cropH, s_fdRectX, s_fdRectY, srcW, srcH, s_fdDownscale, fmt, path);
        fflush(stdout);
    }
    if (s_fdWritten >= s_fdWantCount) {
        fd_finish("count reached");
    }
}

#endif /* headless / android */
