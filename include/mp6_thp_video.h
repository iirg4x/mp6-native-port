#ifndef MP6_THP_VIDEO_H
#define MP6_THP_VIDEO_H
#include <stddef.h>
#include <stdint.h>

/* Decode a THP JPEG payload into GX I8 tiles. Scratch holds its byte-stuffed
 * JPEG representation; each plane has the movie's padded 8x4 tile dimensions. */
int mp6_thp_decode_video(const uint8_t *frame, size_t bytes,
                        uint8_t *scratch, size_t capacity,
                        uint8_t *y, uint8_t *u, uint8_t *v,
                        unsigned width, unsigned height);
int mp6_thp_decode_video_rgb(const uint8_t *frame, size_t bytes,
                            uint8_t *scratch, size_t capacity,
                            uint8_t *y, uint8_t *u, uint8_t *v,
                            unsigned width, unsigned height, uint8_t *rgb);
/* Native movie textures bypass EFB readback, so retained rendering consumes
 * the same decoded frame as a simulation frame. Rewrites only on frame change;
 * the caller reserves destination exclusively for movie pixels while bound. */
int mp6_thp_copy_frame_rgba8(void *destination, unsigned width, unsigned height);
#endif
