#ifndef MP6_MSM_SAFE_H
#define MP6_MSM_SAFE_H

#include <stdint.h>

/* A malformed but file-contained bank must not turn a tiny header into a
 * multi-gigabyte decoded allocation.  32 Mi PCM frames is over ten minutes
 * at MP6's authored 32 kHz stream rate and comfortably exceeds the retail
 * bank census; the byte ceiling bounds peak stream decode workspace to
 * 256 MiB (8 bytes per PCM frame for stereo output + L/R temporaries). */
#define MP6_MSM_MAX_DECODED_PCM_FRAMES (32u * 1024u * 1024u)
#define MP6_MSM_MAX_DECODED_BYTES      (256u * 1024u * 1024u)

static inline int mp6_msm_decode_budget_valid(uint64_t pcmFrames,
                                               uint32_t peakBytesPerFrame)
{
    return pcmFrames > 0 && pcmFrames <= MP6_MSM_MAX_DECODED_PCM_FRAMES &&
           peakBytesPerFrame > 0 &&
           pcmFrames <= (uint64_t)MP6_MSM_MAX_DECODED_BYTES / peakBytesPerFrame;
}

/* Small, dependency-free helpers shared by the production parser/mixer and
 * its focused regression test.  All arithmetic is widened before addition so
 * malformed 32-bit file offsets cannot wrap back into an apparently-valid
 * span. */
static inline int mp6_msm_span_valid_u32(uint32_t total, uint32_t offset, uint32_t size)
{
    return (uint64_t)offset <= (uint64_t)total &&
           (uint64_t)size <= (uint64_t)total - (uint64_t)offset;
}

/* Validate an authored SAMPLE_HEADER loop.  MuSyX data in MP6 contains a
 * handful of loop lengths that include the terminal sample (end == total+1),
 * so accept that documented on-disc shape and clamp the exclusive native end
 * to total.  Larger overruns are malformed.  Returns 1 for a loop, 0 for a
 * one-shot, and -1 for invalid loop metadata. */
static inline int mp6_msm_loop_bounds(uint32_t total, uint32_t start,
                                      uint32_t length, uint32_t *end_out)
{
    uint64_t end;
    if (length == 0) {
        if (end_out) *end_out = 0;
        return 0;
    }
    if (total == 0 || start >= total) {
        return -1;
    }
    end = (uint64_t)start + (uint64_t)length;
    if (end > (uint64_t)total + 1u || end <= start) {
        return -1;
    }
    if (end > total) end = total;
    if (end_out) *end_out = (uint32_t)end;
    return 1;
}

/* Wrap a Q16.16 source position into [start,end).  Modulo handles a very
 * large resampling step without repeated subtraction or a second OOB read. */
static inline int mp6_msm_wrap_q16(uint64_t *position, uint32_t start, uint32_t end)
{
    uint64_t start_q, end_q, span_q;
    if (position == 0 || start >= end) return 0;
    start_q = (uint64_t)start << 16;
    end_q = (uint64_t)end << 16;
    if (*position < end_q) return 1;
    span_q = end_q - start_q;
    *position = start_q + ((*position - end_q) % span_q);
    return 1;
}

/* Mixer end-of-buffer policy shared by streams and SFX. A clear authored
 * loop flag is terminal (the caller deactivates the voice and reports DONE);
 * a set flag wraps to the validated loop interval. */
static inline int mp6_msm_resolve_end_q16(uint64_t *position, int loop,
                                          uint32_t start, uint32_t end)
{
    return loop ? mp6_msm_wrap_q16(position, start, end) : 0;
}

/* Convert a positive millisecond fade duration to output frames without a
 * narrowing wrap.  INT_MAX milliseconds at 48 kHz exceeds uint32_t; a
 * saturated duration remains monotonic and simply approaches a zero step. */
static inline uint32_t mp6_msm_fade_frames_from_ms(int32_t speedMs, uint32_t rateHz)
{
    uint64_t frames;
    if (speedMs <= 0 || rateHz == 0) return 0;
    frames = ((uint64_t)(uint32_t)speedMs * rateHz) / 1000u;
    if (frames == 0) frames = 1;
    if (frames > UINT32_MAX) frames = UINT32_MAX;
    return (uint32_t)frames;
}

#endif /* MP6_MSM_SAFE_H */
