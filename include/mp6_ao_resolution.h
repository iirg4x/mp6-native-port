#ifndef MP6_AO_RESOLUTION_H
#define MP6_AO_RESOLUTION_H
#include <stdint.h>
#include <stdlib.h>

/* Shading-only budget. Decal coverage, R32 scene depth, foliage color and the
 * final bilateral composite always keep the full framebuffer resolution. */
static inline void mp6_ao_working_size_with_limit(unsigned width, unsigned height,
    unsigned limit, unsigned *aoWidth, unsigned *aoHeight)
{
    unsigned longest=width>height ? width : height;
    if (longest>limit) {
        *aoWidth=(unsigned)(((uint64_t)width*limit+longest-1)/longest);
        *aoHeight=(unsigned)(((uint64_t)height*limit+longest-1)/longest);
    } else {
        *aoWidth=width; *aoHeight=height;
    }
    if (!*aoWidth) *aoWidth=1;
    if (!*aoHeight) *aoHeight=1;
}
static inline void mp6_ao_working_size(unsigned width, unsigned height,
                                      unsigned *aoWidth, unsigned *aoHeight)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    /* 1200. 720 and 960 left stair-steps on characters at 3x phone scale.
     * On the S22+ (2026-10-22) 1200 cost the same as 960 (~102 FPS vs ~119
     * off), while 1440 fell off a cliff (~66 FPS). Android's interpolated
     * frames reuse the tick's AO, so the size costs only tick frames. */
    static int override=-1;
    if (override<0) { /* MP6_AO_CAP_PX: comparison runs */
        const char *env=getenv("MP6_AO_CAP_PX");
        const unsigned long v=env ? strtoul(env,0,10) : 0;
        override=v>=64 && v<=16384 ? (int)v : 0;
    }
    unsigned limit=override ? (unsigned)override : 1200;
#if defined(__EMSCRIPTEN__)
    /* iPhone: half the screen's long side (1398 on a 2796-px Pro Max),
     * within 1200..1440, so AO pixels stay at most 2x2 screen pixels. */
    if (!override) {
        const unsigned half=(width>height ? width : height)/2;
        limit=half<1200 ? 1200 : half>1440 ? 1440 : half;
    }
#endif
#else
    const unsigned limit=1920;
#endif
    mp6_ao_working_size_with_limit(width,height,limit,aoWidth,aoHeight);
}

/* Mobile GPUs (Android, iPhone Safari): the prepared R32 depth that GTAO, the
 * denoiser and the reconstruction read is also stored at the shading size,
 * not the full framebuffer. An iPhone A/B (2796x1056, 2026-10-19) showed the
 * apply step owning nearly all of AO's GPU time while the 960 cap or the blur
 * alone changed nothing: the full-resolution preparation pass and 30-odd
 * scattered taps per pixel into a 3-megapixel depth texture stayed. Desktop
 * keeps full-resolution depth. Decal coverage and foliage stay full size. */
static inline int mp6_ao_reduced_depth(void)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    /* MP6_AO_FULL_DEPTH=1 keeps full-resolution depth (comparison runs). */
    static int reduced = -1;
    if (reduced < 0) {
        const char *env = getenv("MP6_AO_FULL_DEPTH");
        reduced = !(env && env[0] == '1');
    }
    return reduced;
#else
    return 0;
#endif
}
#endif
