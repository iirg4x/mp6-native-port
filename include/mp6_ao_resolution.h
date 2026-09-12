#ifndef MP6_AO_RESOLUTION_H
#define MP6_AO_RESOLUTION_H
#include <stdint.h>

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
#ifdef __ANDROID__
    const unsigned limit=960;
#else
    const unsigned limit=1920;
#endif
    mp6_ao_working_size_with_limit(width,height,limit,aoWidth,aoHeight);
}
#endif
