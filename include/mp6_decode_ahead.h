/* MP6 native port -- decompress archive files ahead of the game.
 *
 * The game reads a whole data/NAME.bin archive (from the disc, or out of its
 * ARAM cache, where boards and characters wait between scenes) and then
 * decompresses file after file on the game thread (HuDecodeData), often
 * hundreds in one tick at a board start. Here an archive is decompressed
 * once by a few worker threads as soon as the game first has it -- for
 * ARAM archives, long before they are needed -- and kept in host memory
 * (least recently used archives dropped past MP6_DECODE_AHEAD_MB, default
 * 160). HuDecodeData then copies the finished file instead of
 * decompressing, and only waits for files the workers have not reached.
 * Which files of each archive the game actually asks for is remembered in
 * mp6_decode_ahead.idx (host pref dir); a known archive decodes only those,
 * which keeps the cache at what scenes use (~26 MB instead of ~69 MB to a
 * board).
 *
 * Exactness: a result is used only when the bytes the decoder consumed
 * still equal the game's source bytes at that address, and only for files
 * the worker decoded cleanly within their bounds (anything unusual, such as
 * a back-reference before the output start or a short stream, falls back to
 * the game's own decoder). The output is a pure function of those bytes, so
 * game memory ends up identical. The one difference: a zlib file no longer
 * borrows and frees its scratch block from the model heap, which leaves the
 * heap's block layout unchanged.
 *
 * MP6_DECODE_AHEAD=0 turns it off; =N sets the worker count (default from
 * the CPU count, at most 4). MP6_DECODE_AHEAD_VERIFY=1 still runs the game's
 * decoder and reports any result that differs. The web build has no worker
 * threads, so there it does nothing.
 */
#ifndef MP6_DECODE_AHEAD_H
#define MP6_DECODE_AHEAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An archive's bytes, just read in full from the disc into game memory at
 * `base`. Anything that does not parse as an archive is ignored. */
void mp6_decode_ahead_archive(const void *base, uint32_t length);

/* The same for an archive read whole from disc file `disc` (an FST entry):
 * once that file has been seen, its archive is found without hashing. */
void mp6_decode_ahead_disc_read(const void *base, uint32_t length, int32_t disc);

/* Part of a known disc archive, [offset, offset + length), read into game
 * memory at `base` (the game reading one file out of it): files decoded
 * from there come from decode-ahead too. */
void mp6_decode_ahead_disc_part(const void *base, uint32_t length, int32_t disc, uint32_t offset);

/* An archive the game is about to read (the disc prefetch read disc file
 * `disc` ahead, src/dvd/dvd_files.c): decode its files now, so they are
 * ready when the game's own read of that file arrives. Any thread; `bytes`
 * may be freed on return. */
void mp6_decode_ahead_preload(const void *bytes, uint32_t length, int32_t disc);

/* Where there are no worker threads (the web), decode-ahead is cooperative:
 * archives the game reads or prefetches are noted (want_*), and the game
 * thread registers and decodes them in the idle time before a tick's
 * deadline (idle), one predicted-to-fit job at a time. idle returns 1 when
 * it did some work, 0 when there was nothing that fits (or threads exist). */
void mp6_decode_ahead_want_disc(int32_t disc, uint32_t length);
void mp6_decode_ahead_want_stable(const void *base, uint32_t length);
int mp6_decode_ahead_idle(int64_t deadlineNs);

/* The game copied `length` bytes from src to dst (ARAM transfers). When the
 * source holds a known archive, or a file inside one, dst becomes another
 * place to find it; otherwise dst is tried as a whole archive. `stable` marks
 * memory the game keeps data in for later (ARAM): its view survives until
 * something overwrites it. */
void mp6_decode_ahead_copy(void *dst, const void *src, uint32_t length, int stable);

/* From HuDecodeData: 1 when dst now holds the decoded file, else 0 and the
 * caller decodes as usual (then calls mp6_decode_ahead_verify). */
int mp6_decode_ahead_take(const void *src, void *dst, uint32_t size, int32_t type);
void mp6_decode_ahead_verify(const void *dst, uint32_t size);

/* MP6_DECODE_GUARD=1: log every decompression (type, destination range,
 * bytes written vs expected, leftover size counter, checksum) to catch heap
 * stomps and run-to-run differences; no output and one cached getenv when
 * unset. `wrote` is how far the decoder advanced (zlib does not). */
void mp6_decode_guard(const void *dst, uint32_t size, int32_t type, long long wrote, uint32_t left);

/* Once per game tick (diagnostic output only). */
void mp6_decode_ahead_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_DECODE_AHEAD_H */
