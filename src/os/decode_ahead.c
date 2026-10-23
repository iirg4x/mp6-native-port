/* MP6 native port -- decompress archive files ahead of the game; see
 * include/mp6_decode_ahead.h for the contract.
 *
 * An archive is stored once by content and decoded once; every place the
 * game holds those bytes (the disc read buffer, the ARAM cache, a copy
 * back from ARAM, one file copied out of an ARAM archive) is a "view" of
 * it. HuDecodeData finds the view containing its source pointer and copies
 * the finished file, after checking that the bytes the decoder depends on
 * still match.
 *
 * The decoders below are bounded re-implementations of the game's
 * (src/game/decode.c in the decomp): same output for every well-formed
 * file, and "failed" for anything the game's version would resolve from
 * memory outside the file (reads past the stream, a back-reference before
 * the output start, an output overrun, a short zlib stream). Failed files
 * are simply decoded by the game as before. */
#include "mp6_decode_ahead.h"
#include "mp6_hsf_preflight.h"
#include "mp6_dvd_files.h" /* mp6_dvd_read_entry_now: cooperative registration */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"

#include "mp6_host_section.h" /* worker pool: state of the running process */

enum {
    AHEAD_SKIP,    /* stored files, or nothing worth doing */
    AHEAD_QUEUED,  /* waiting for a worker */
    AHEAD_RUNNING, /* a worker is decoding it */
    AHEAD_DONE,    /* out holds the decoded file */
    AHEAD_FAILED,  /* the game decodes it itself */
    AHEAD_CLAIMED, /* the game asked before a worker started; queued again on the next visit */
};

enum { HU_TYPE_NONE = 0, HU_TYPE_LZ = 1, HU_TYPE_SLIDE = 2, HU_TYPE_FSLIDE_ALT = 3,
       HU_TYPE_FSLIDE = 4, HU_TYPE_RLE = 5, HU_TYPE_ZLIB = 7 };

#define AHEAD_MAX_WORKERS 4
#define AHEAD_QUEUE_CAP 16384
#define AHEAD_MAX_FILES 4096
#define AHEAD_MAX_VIEWS 256
/* Decoded bytes kept, least recently used archives dropped first. */
#define AHEAD_DEFAULT_BUDGET_MB 160

typedef struct AheadArchive AheadArchive;

typedef struct AheadEntry {
    AheadArchive *archive;
    uint32_t offset;   /* the compressed stream, within the archive */
    uint32_t span;     /* bytes from offset to the next file or the end */
    uint32_t size;     /* decoded size */
    int32_t type;
    uint32_t consumed; /* stream bytes the decode depends on */
    uint8_t *out;
    int state;
    uint32_t fileIndex; /* position in the archive's directory */
    int hsfChecked;     /* a model the worker already ran the loader's check on */
    Mp6HsfPreflight hsf;
} AheadEntry;

/* Which files of an archive the game has ever asked for, remembered across
 * sessions (mp6_decode_ahead.idx in the host's pref dir): a known archive
 * only decodes those, so the cache holds what scenes use (about a third of
 * what whole archives decode to) instead of every file. */
typedef struct {
    uint64_t hash;
    uint32_t length, count;
    uint8_t *bits; /* (count + 7) / 8 bytes, bit i = directory file i */
} AheadUsage;

struct AheadArchive {
    AheadArchive *next;   /* most recently used first */
    AheadUsage *usage;    /* never NULL once queued */
    uint64_t hash;
    uint32_t length;
    int32_t disc;         /* disc file it was read from, or -1: matched without hashing */
    uint8_t *bytes;       /* our copy of the archive */
    AheadEntry *entries;  /* sorted by offset */
    uint32_t count;
    uint32_t queued;      /* queue slots still pointing into entries */
    uint64_t decoded;     /* bytes held by DONE entries' outputs */
};

/* Game memory holding (part of) an archive: game[i] is archive byte
 * archiveOffset + i. */
typedef struct {
    const uint8_t *game;
    uint32_t length;
    uint32_t archiveOffset;
    AheadArchive *archive;
    /* Data the game keeps for later (ARAM): only an overlapping write
     * retires it, never the ring of newer views wrapping around. */
    int stable;
} AheadView;

static Mp6Mutex g_lock;
static Mp6Sem g_work;
static int g_enabled = -1;
static int g_verify;
static int g_debug;
static int g_workers;
/* No worker threads (the web): queued work runs on the game thread in the
 * idle time before each tick's deadline (mp6_decode_ahead_idle), and what
 * the game reads is registered there too, never during its own load. */
static int g_coop;
#define AHEAD_COOP_BUDGET_MB 64
#define AHEAD_PENDING_CAP 64
typedef struct {
    int32_t disc;          /* a disc file to read whole, or -1 */
    const uint8_t *stable; /* memory the game keeps an archive in (ARAM) */
    uint32_t length;
    unsigned tries;
} AheadPending;
static AheadPending g_pending[AHEAD_PENDING_CAP];
static unsigned g_pendingHead, g_pendingTail;
/* Where the game holds an archive it loaded before idle time registered it,
 * and the files it decoded from there meanwhile: registration learns those
 * as used, so the next visit has them decoded ahead. */
#define AHEAD_LAZY_CAP 16
typedef struct {
    const uint8_t *game;
    uint32_t length;
    int32_t disc;
    int stable;
    uint32_t *offsets; /* archive offsets of the files taken */
    uint32_t count, cap;
} AheadLazy;
static AheadLazy g_lazy[AHEAD_LAZY_CAP];
static unsigned g_lazyNext;
static int g_inIdle;
static double g_decodeNsPerKB = 5000.0; /* learned (EMA); 200 MB/s to start */
static double g_copyNsPerKB = 500.0;    /* reading + registering, learned too */
static uint64_t g_budget;
static AheadArchive *g_archives;
static AheadView g_views[AHEAD_MAX_VIEWS];
static unsigned g_viewNext;
static AheadEntry *g_queue[AHEAD_QUEUE_CAP];
static unsigned g_queueHead, g_queueTail;
/* Files the game asked for before a worker reached them on an earlier visit;
 * workers take these before anything in g_queue. */
#define AHEAD_URGENT_CAP 1024
static AheadEntry *g_urgent[AHEAD_URGENT_CAP];
static unsigned g_urgentHead, g_urgentTail;
static uint64_t g_bytes; /* outputs allocated, decoded or not */
/* Cooperative mode: archive copies count against the budget too (the web
 * heap never shrinks), and queued files of an old archive are dropped with
 * it rather than keeping it alive. */
static uint64_t g_archiveBytes;
static uint8_t *g_verifyOut; /* MP6_DECODE_AHEAD_VERIFY: the result to compare */
static uint64_t g_hits, g_misses, g_verified, g_mismatches;
static uint64_t g_waitNs, g_waits; /* game thread waiting for a worker */
/* MP6_DECODE_AHEAD_DEBUG per tick: game-thread time in hits (compare and
 * copy), and what misses left the game to decode itself. */
static uint64_t g_tickHitNs, g_tickHits, g_tickMissBytes, g_tickMisses;
static uint64_t g_tickMatchNs; /* game thread identifying archives it read */
static uint64_t g_tickIdleNs, g_tickIdleJobs, g_tickIdleCalls; /* cooperative work done in idle time */
/* Models take() just delivered with a checked result, for the loader. Game
 * thread only; valid for the tick they were delivered in. */
#define AHEAD_HSF_RECENT 8
static struct {
    const void *dst;
    uint32_t size;
    long tick;
    Mp6HsfPreflight hsf;
} g_hsfRecent[AHEAD_HSF_RECENT];
static unsigned g_hsfRecentNext;
static long g_aheadTick;
static AheadUsage **g_usage;
static uint32_t g_usageCount, g_usageCap;
static int g_usageDirty;
static void usage_load(void);
static char g_usagePath[512];

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- bounded decoders ------------------------------------------------ */

typedef struct {
    const uint8_t *src, *srcEnd, *srcStart;
    uint8_t *dst, *dstEnd, *dstStart;
} Bounded;

#define NEED_IN(b, n) do { if ((size_t)((b)->srcEnd - (b)->src) < (size_t)(n)) return 0; } while (0)

static int decode_lz(Bounded *b)
{
    uint8_t ring[1024];
    unsigned control = 0, windowPos = 958;
    memset(ring, 0, sizeof(ring));
    while (b->dst < b->dstEnd) {
        control >>= 1;
        if (!(control & 0x100)) {
            NEED_IN(b, 1);
            control = *b->src++ | 0xFF00;
        }
        if (control & 1) {
            NEED_IN(b, 1);
            ring[windowPos++] = *b->dst++ = *b->src++;
            windowPos &= 0x3FF;
        } else {
            unsigned readPos, len, i;
            NEED_IN(b, 2);
            readPos = *b->src++;
            len = *b->src++;
            readPos |= (len & ~0x3Fu) << 2;
            len = (len & 0x3F) + 3;
            if (len > (size_t)(b->dstEnd - b->dst)) return 0;
            for (i = 0; i < len; i++) {
                ring[windowPos++] = *b->dst++ = ring[(readPos + i) & 0x3FF];
                windowPos &= 0x3FF;
            }
        }
    }
    return 1;
}

/* SLIDE fills references before the output start with zeroes; FSLIDE reads
 * whatever precedes the destination, which only the game can see. */
static int decode_slide(Bounded *b, int zeroBeforeStart)
{
    uint32_t bits = 0, control = 0;
    NEED_IN(b, 4); /* declared size, unused */
    b->src += 4;
    while (b->dst < b->dstEnd) {
        if (bits == 0) {
            NEED_IN(b, 4);
            control = be32(b->src);
            b->src += 4;
            bits = 32;
        }
        if (control >> 31) {
            NEED_IN(b, 1);
            *b->dst++ = *b->src++;
        } else {
            uint32_t distance, run;
            const uint8_t *from;
            NEED_IN(b, 2);
            distance = ((uint32_t)b->src[0] << 8) | b->src[1];
            b->src += 2;
            run = (distance >> 12) & 0xF;
            distance &= 0xFFF;
            if (run == 0) {
                NEED_IN(b, 1);
                run = *b->src++ + 18u;
            } else {
                run += 2;
            }
            if (run > (size_t)(b->dstEnd - b->dst)) return 0;
            from = b->dst - distance;
            for (; run; run--, from++) {
                if (from - 1 < b->dstStart) {
                    if (!zeroBeforeStart) return 0;
                    *b->dst++ = 0;
                } else {
                    *b->dst++ = from[-1];
                }
            }
        }
        control <<= 1;
        bits--;
    }
    return 1;
}

static int decode_rle(Bounded *b)
{
    while (b->dst < b->dstEnd) {
        unsigned run;
        NEED_IN(b, 1);
        run = *b->src++;
        if (run < 0x80) {
            uint8_t value;
            NEED_IN(b, 1);
            value = *b->src++;
            if (run > (size_t)(b->dstEnd - b->dst)) return 0;
            memset(b->dst, value, run);
            b->dst += run;
        } else {
            run -= 0x80;
            NEED_IN(b, run);
            if (run > (size_t)(b->dstEnd - b->dst)) return 0;
            memcpy(b->dst, b->src, run);
            b->dst += run;
            b->src += run;
        }
    }
    return 1;
}

static voidpf zlib_alloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    return calloc(items, size);
}

static void zlib_free(voidpf opaque, voidpf address)
{
    (void)opaque;
    free(address);
}

static int decode_zlib(Bounded *b)
{
    z_stream stream;
    uint32_t streamLength;
    int ok;
    NEED_IN(b, 8);
    streamLength = be32(b->src + 4);
    NEED_IN(b, 8u + (uint64_t)streamLength);
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)(b->src + 8);
    stream.avail_in = streamLength;
    stream.next_out = b->dst;
    stream.avail_out = (uInt)(b->dstEnd - b->dst);
    stream.zalloc = zlib_alloc;
    stream.zfree = zlib_free;
    if (inflateInit(&stream) != Z_OK) return 0;
    ok = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.avail_out == 0;
    inflateEnd(&stream);
    /* The game's decoder may look at any byte of the declared stream. */
    b->src += 8u + streamLength;
    b->dst = b->dstEnd;
    return ok;
}

static int decode_entry(AheadEntry *e)
{
    Bounded b;
    int ok;
    b.srcStart = b.src = e->archive->bytes + e->offset;
    b.srcEnd = b.src + e->span;
    b.dstStart = b.dst = e->out;
    b.dstEnd = e->out + e->size;
    switch (e->type) {
    case HU_TYPE_LZ: ok = decode_lz(&b); break;
    case HU_TYPE_SLIDE: ok = decode_slide(&b, 1); break;
    case HU_TYPE_FSLIDE_ALT:
    case HU_TYPE_FSLIDE: ok = decode_slide(&b, 0); break;
    case HU_TYPE_RLE: ok = decode_rle(&b); break;
    case HU_TYPE_ZLIB: ok = decode_zlib(&b); break;
    default: ok = 0; break;
    }
    e->consumed = (uint32_t)(b.src - b.srcStart);
    return ok;
}

/* ---- archives and views (g_lock held) --------------------------------- */

static uint64_t content_hash(const uint8_t *p, uint32_t length)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ length;
    uint32_t i = 0;
    for (; i + 8 <= length; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < length; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static void touch_archive(AheadArchive *a)
{
    AheadArchive **link = &g_archives;
    while (*link && *link != a) link = &(*link)->next;
    if (*link == NULL || link == &g_archives) return;
    *link = a->next;
    a->next = g_archives;
    g_archives = a;
}

static int archive_busy(const AheadArchive *a)
{
    uint32_t i;
    if (a->queued && !g_coop) return 1;
    for (i = 0; i < a->count; i++) {
        if (a->entries[i].state == AHEAD_RUNNING) return 1;
    }
    return 0;
}

/* Queue slots pointing into `a` (cooperative mode: nothing is running). */
static void unqueue_archive(AheadArchive *a)
{
    unsigned i, keep;
    for (i = keep = g_queueHead; i != g_queueTail; i++) {
        AheadEntry *e = g_queue[i % AHEAD_QUEUE_CAP];
        if (e->archive != a) g_queue[keep++ % AHEAD_QUEUE_CAP] = e;
    }
    g_queueTail = keep;
    for (i = keep = g_urgentHead; i != g_urgentTail; i++) {
        AheadEntry *e = g_urgent[i % AHEAD_URGENT_CAP];
        if (e->archive != a) g_urgent[keep++ % AHEAD_URGENT_CAP] = e;
    }
    g_urgentTail = keep;
    a->queued = 0;
}

static void drop_archive(AheadArchive *a)
{
    AheadArchive **link = &g_archives;
    uint32_t i;
    if (a->queued) unqueue_archive(a);
    while (*link && *link != a) link = &(*link)->next;
    if (*link) *link = a->next;
    for (i = 0; i < AHEAD_MAX_VIEWS; i++) {
        if (g_views[i].archive == a) g_views[i].archive = NULL;
    }
    for (i = 0; i < a->count; i++) {
        AheadEntry *e = &a->entries[i];
        if (e->out) {
            free(e->out);
            g_bytes -= e->size;
        }
    }
    free(a->entries);
    free(a->bytes);
    g_archiveBytes -= a->length;
    free(a);
}

static uint64_t held_bytes(void)
{
    return g_bytes + (g_coop ? g_archiveBytes : 0);
}

/* Least recently used archives go first; one with work in flight stays. */
static void enforce_budget(uint64_t incoming)
{
    while (held_bytes() + incoming > g_budget) {
        AheadArchive *a, *victim = NULL;
        for (a = g_archives; a; a = a->next) {
            if (!archive_busy(a)) victim = a;
        }
        if (victim == NULL) return;
        drop_archive(victim);
    }
}

static int file_used(const AheadEntry *e)
{
    const AheadUsage *u = e->archive->usage;
    return u && (u->bits[e->fileIndex >> 3] & (1u << (e->fileIndex & 7)));
}

/* A known archive loaded again: queue the files scenes use that are not
 * decoded yet -- ones the game claimed last time, and ones learned since
 * the archive was first seen. Returns how many were queued. */
static unsigned requeue_used(AheadArchive *a)
{
    unsigned queued = 0;
    uint32_t i;
    int pass;
    /* Claimed files go first: last visit the game wanted them before a
     * worker reached them, so they are needed earliest. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < a->count; i++) {
            AheadEntry *e = &a->entries[i];
            if (e->size == 0 || e->type == HU_TYPE_NONE || !file_used(e)) continue;
            if (e->state != (pass == 0 ? AHEAD_CLAIMED : AHEAD_SKIP)) continue;
            if (held_bytes() + e->size > g_budget || g_queueTail - g_queueHead >= AHEAD_QUEUE_CAP) return queued;
            if (pass == 0 && g_urgentTail - g_urgentHead >= AHEAD_URGENT_CAP) continue;
            e->out = (uint8_t *)malloc(e->size);
            if (!e->out) return queued;
            g_bytes += e->size;
            a->queued++;
            if (pass == 0) g_urgent[g_urgentTail++ % AHEAD_URGENT_CAP] = e;
            else g_queue[g_queueTail++ % AHEAD_QUEUE_CAP] = e;
            e->state = AHEAD_QUEUED;
            queued++;
        }
    }
    return queued;
}

/* Whatever the game had at these addresses before is gone. */
static void retire_views(const uint8_t *game, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < AHEAD_MAX_VIEWS; i++) {
        AheadView *v = &g_views[i];
        if (v->archive && v->game < game + length && game < v->game + v->length) v->archive = NULL;
    }
}

static void add_view(const uint8_t *game, uint32_t length, uint32_t archiveOffset, AheadArchive *a,
                     int stable)
{
    uint32_t i, slot = g_viewNext % AHEAD_MAX_VIEWS;
    retire_views(game, length);
    /* Reuse the oldest slot that is free or holds a transient view. Each
     * address is in at most one view, so lookups do not depend on order. */
    for (i = 0; i < AHEAD_MAX_VIEWS; i++) {
        uint32_t k = (g_viewNext + i) % AHEAD_MAX_VIEWS;
        if (!g_views[k].archive || !g_views[k].stable) {
            slot = k;
            break;
        }
    }
    g_views[slot] = (AheadView){ game, length, archiveOffset, a, stable };
    g_viewNext = slot + 1;
}

/* The newest view holding [p, p + length). */
static AheadView *find_view(const uint8_t *p, uint32_t length)
{
    unsigned n;
    for (n = 1; n <= AHEAD_MAX_VIEWS; n++) {
        AheadView *v = &g_views[(g_viewNext - n) % AHEAD_MAX_VIEWS];
        if (v->archive && p >= v->game && (uint64_t)(p - v->game) + length <= v->length) return v;
    }
    return NULL;
}

static AheadEntry *find_entry(AheadArchive *a, uint32_t offset)
{
    uint32_t lo = 0, hi = a->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (a->entries[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    return lo < a->count && a->entries[lo].offset == offset ? &a->entries[lo] : NULL;
}

/* ---- workers ----------------------------------------------------------- */

/* The next queued file (g_lock held), marked running; NULL when none. With
 * maxNs (cooperative mode), the next one predicted to finish within it:
 * larger ones go to the back of their queue, left to the game's own load. */
static AheadEntry *pop_job(double maxNs)
{
    unsigned skips = 0;
    while (g_urgentHead != g_urgentTail || g_queueHead != g_queueTail) {
        int urgent = g_urgentHead != g_urgentTail;
        AheadEntry *next = urgent ? g_urgent[g_urgentHead % AHEAD_URGENT_CAP]
                                  : g_queue[g_queueHead % AHEAD_QUEUE_CAP];
        if (urgent) g_urgentHead++;
        else g_queueHead++;
        if (next->state == AHEAD_QUEUED && maxNs > 0 && next->size / 1024.0 * g_decodeNsPerKB > maxNs) {
            if (urgent) g_urgent[g_urgentTail++ % AHEAD_URGENT_CAP] = next;
            else g_queue[g_queueTail++ % AHEAD_QUEUE_CAP] = next;
            if (++skips >= (g_urgentTail - g_urgentHead) + (g_queueTail - g_queueHead)) return NULL;
            continue;
        }
        next->archive->queued--;
        if (next->state == AHEAD_QUEUED) {
            next->state = AHEAD_RUNNING;
            return next;
        }
    }
    return NULL;
}

/* Decode one popped file, outside g_lock. */
static void run_job(AheadEntry *e)
{
    int ok = decode_entry(e);
        /* A model: run the loader's input check here too, off the game thread. */
    if (ok && e->size >= 8 && memcmp(e->out, "HSF", 3) == 0)
        e->hsfChecked = mp6_hsf_preflight_bytes(e->out, e->size, &e->hsf);
    mp6_host_mutex_lock(&g_lock);
    if (ok) {
        e->state = AHEAD_DONE;
    } else {
        e->state = AHEAD_FAILED;
        free(e->out);
        e->out = NULL;
        g_bytes -= e->size;
    }
    mp6_host_mutex_unlock(&g_lock);
}

static void worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        AheadEntry *e;
        mp6_host_sem_wait(&g_work);
        mp6_host_mutex_lock(&g_lock);
        e = pop_job(0);
        mp6_host_mutex_unlock(&g_lock);
        if (e != NULL) run_job(e);
    }
}

static int ahead_enabled(void)
{
    if (g_enabled < 0) {
        const char *env = getenv("MP6_DECODE_AHEAD");
        const char *verify = getenv("MP6_DECODE_AHEAD_VERIFY");
        const char *budget = getenv("MP6_DECODE_AHEAD_MB");
        int workers = mp6_host_cpu_count() / 2, i;
        if (workers > AHEAD_MAX_WORKERS) workers = AHEAD_MAX_WORKERS;
        if (workers < 1) workers = 1;
        if (env && env[0]) workers = atoi(env);
        if (workers > AHEAD_MAX_WORKERS) workers = AHEAD_MAX_WORKERS;
        g_verify = verify && verify[0] == '1';
        g_debug = getenv("MP6_DECODE_AHEAD_DEBUG") != NULL;
        g_enabled = 0;
        mp6_host_mutex_init(&g_lock);
        if (workers > 0 && mp6_host_sem_init(&g_work)) {
            for (i = 0; i < workers; i++) {
                if (mp6_host_thread_start(worker_main, NULL) == 0) g_workers++;
            }
        }
        /* MP6_DECODE_AHEAD=0 turns it off everywhere; without threads it
         * runs cooperatively. */
        g_coop = g_workers == 0 && !(env && env[0] == '0');
        g_enabled = g_workers > 0 || g_coop;
        g_budget = (uint64_t)(budget && atoi(budget) > 0 ? atoi(budget)
                              : g_coop ? AHEAD_COOP_BUDGET_MB : AHEAD_DEFAULT_BUDGET_MB) << 20;
        if (g_enabled) usage_load();
        if (g_enabled)
            printf("[MP6-DECODE-AHEAD] %s, %llu MB%s\n", g_coop ? "cooperative (idle time)" : "workers",
                   (unsigned long long)(g_budget >> 20), g_verify ? ", verify" : "");
    }
    return g_enabled;
}

/* ---- learned file usage ------------------------------------------------ */

#define AHEAD_USAGE_MAGIC "MP6DAIX1"

/* Records are allocated one by one: archives keep pointers to them. */
static AheadUsage *usage_add(uint64_t hash, uint32_t length, uint32_t count)
{
    AheadUsage *u;
    if (g_usageCount == g_usageCap) {
        uint32_t cap = g_usageCap ? g_usageCap * 2 : 64;
        AheadUsage **grown = (AheadUsage **)realloc(g_usage, cap * sizeof(AheadUsage *));
        if (!grown) return NULL;
        g_usage = grown;
        g_usageCap = cap;
    }
    u = (AheadUsage *)calloc(1, sizeof(AheadUsage));
    if (!u) return NULL;
    u->bits = (uint8_t *)calloc((count + 7) / 8, 1);
    if (!u->bits) {
        free(u);
        return NULL;
    }
    g_usage[g_usageCount] = u;
    u->hash = hash;
    u->length = length;
    u->count = count;
    g_usageCount++;
    return u;
}

static AheadUsage *usage_find(uint64_t hash, uint32_t length, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < g_usageCount; i++) {
        if (g_usage[i]->hash == hash && g_usage[i]->length == length && g_usage[i]->count == count)
            return g_usage[i];
    }
    return NULL;
}

static void usage_load(void)
{
    char dir[400];
    FILE *f;
    char magic[8];
    if (mp6_host_pref_dir(dir, sizeof(dir)) != 0) return;
    snprintf(g_usagePath, sizeof(g_usagePath), "%s/mp6_decode_ahead.idx", dir);
    f = fopen(g_usagePath, "rb");
    if (!f) return;
    if (fread(magic, 1, 8, f) == 8 && memcmp(magic, AHEAD_USAGE_MAGIC, 8) == 0) {
        for (;;) {
            uint64_t hash;
            uint32_t meta[2];
            AheadUsage *u;
            if (fread(&hash, sizeof(hash), 1, f) != 1 || fread(meta, sizeof(meta), 1, f) != 1) break;
            if (meta[1] == 0 || meta[1] > AHEAD_MAX_FILES) break;
            u = usage_add(hash, meta[0], meta[1]);
            if (!u || fread(u->bits, 1, (meta[1] + 7) / 8, f) != (meta[1] + 7) / 8) {
                if (u) { /* a torn tail: keep what was whole */
                    g_usageCount--;
                    free(u->bits);
                    free(u);
                }
                break;
            }
        }
    }
    fclose(f);
}

/* Written whole to a temporary name and renamed, so a crash never leaves
 * a half file. */
static void usage_save(void)
{
    char tmp[540];
    FILE *f;
    uint32_t i;
    if (!g_usagePath[0]) return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_usagePath);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(AHEAD_USAGE_MAGIC, 1, 8, f);
    for (i = 0; i < g_usageCount; i++) {
        const AheadUsage *u = g_usage[i];
        uint32_t meta[2] = { u->length, u->count }, k;
        /* Nothing learned yet is not "uses nothing": a later run decodes
         * all of it again rather than none. */
        for (k = 0; k < (u->count + 7) / 8 && u->bits[k] == 0; k++) {}
        if (k == (u->count + 7) / 8) continue;
        fwrite(&u->hash, sizeof(u->hash), 1, f);
        fwrite(meta, sizeof(meta), 1, f);
        fwrite(u->bits, 1, (u->count + 7) / 8, f);
    }
    if (fclose(f) == 0) {
        remove(g_usagePath);
        rename(tmp, g_usagePath);
    }
}

static void usage_mark(AheadEntry *e)
{
    AheadUsage *u = e->archive->usage;
    uint8_t bit = (uint8_t)(1u << (e->fileIndex & 7));
    if (u && !(u->bits[e->fileIndex >> 3] & bit)) {
        u->bits[e->fileIndex >> 3] |= bit;
        g_usageDirty = 1;
    }
}

/* ---- game thread ------------------------------------------------------- */

static int compare_offsets(const void *a, const void *b)
{
    uint32_t x = ((const AheadEntry *)a)->offset, y = ((const AheadEntry *)b)->offset;
    return x < y ? -1 : x > y;
}

/* Parse and queue a new archive (g_lock not held). NULL when the bytes are
 * not an archive or there is no room. */
static AheadArchive *new_archive(const uint8_t *game, uint32_t length, uint64_t hash)
{
    AheadArchive *a;
    uint32_t count, i, queued = 0;
    uint64_t decoded = 0;
    int learning = 0; /* usage record made here, for an archive not seen before */

    count = be32(game);
    if (count == 0 || count > AHEAD_MAX_FILES || 4u + (uint64_t)count * 4u > length) return NULL;
    for (i = 0; i < count; i++) {
        uint32_t header = be32(game + 4 + i * 4);
        if (header < 4u + count * 4u || header > length - 8 || be32(game + header + 4) > HU_TYPE_ZLIB)
            return NULL;
    }
    a = (AheadArchive *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->disc = -1;
    a->entries = (AheadEntry *)calloc(count, sizeof(AheadEntry));
    a->bytes = (uint8_t *)malloc(length);
    if (!a->entries || !a->bytes) {
        free(a->entries);
        free(a->bytes);
        free(a);
        return NULL;
    }
    memcpy(a->bytes, game, length);
    a->hash = hash;
    a->length = length;
    a->count = count;
    for (i = 0; i < count; i++) {
        uint32_t header = be32(a->bytes + 4 + i * 4);
        AheadEntry *e = &a->entries[i];
        e->archive = a;
        e->fileIndex = i;
        e->offset = header + 8; /* past the size and type words */
        e->size = be32(a->bytes + header);
        e->type = (int32_t)be32(a->bytes + header + 4);
        e->state = e->size != 0 && e->type != HU_TYPE_NONE ? AHEAD_QUEUED : AHEAD_SKIP;
    }
    {
        /* A known archive decodes only the files scenes have asked for; a new
         * one decodes everything and starts learning. */
        AheadUsage *u = usage_find(hash, length, count);
        if (u) {
            for (i = 0; i < count; i++) {
                AheadEntry *e = &a->entries[i];
                if (e->state == AHEAD_QUEUED && !(u->bits[e->fileIndex >> 3] & (1u << (e->fileIndex & 7))))
                    e->state = AHEAD_SKIP;
            }
        } else {
            u = usage_add(hash, length, count);
            learning = 1;
            if (!u) {
                free(a->entries);
                free(a->bytes);
                free(a);
                return NULL;
            }
        }
        a->usage = u;
    }
    qsort(a->entries, count, sizeof(AheadEntry), compare_offsets);
    for (i = 0; i < count; i++) {
        AheadEntry *e = &a->entries[i];
        uint32_t end = length, j;
        for (j = i + 1; j < count; j++) {
            if (a->entries[j].offset > e->offset) {
                end = a->entries[j].offset - 8;
                break;
            }
        }
        e->span = end > e->offset ? end - e->offset : 0;
        if (e->state == AHEAD_QUEUED) decoded += e->size;
    }

    mp6_host_mutex_lock(&g_lock);
    enforce_budget(decoded + (g_coop ? length : 0));
    if (held_bytes() + decoded + (g_coop ? length : 0) > g_budget ||
        g_queueTail - g_queueHead + count > AHEAD_QUEUE_CAP) {
        if (learning && g_usageCount && g_usage[g_usageCount - 1] == a->usage) {
            /* Nothing learned: an empty record would mean "uses no files". */
            g_usageCount--;
            free(a->usage->bits);
            free(a->usage);
        }
        mp6_host_mutex_unlock(&g_lock);
        free(a->entries);
        free(a->bytes);
        free(a);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        AheadEntry *e = &a->entries[i];
        if (e->state != AHEAD_QUEUED) continue;
        e->out = (uint8_t *)malloc(e->size);
        if (!e->out) {
            e->state = AHEAD_SKIP;
            continue;
        }
        g_bytes += e->size;
        a->queued++;
        g_queue[g_queueTail++ % AHEAD_QUEUE_CAP] = e;
        queued++;
    }
    a->next = g_archives;
    g_archives = a;
    g_archiveBytes += length;
    mp6_host_mutex_unlock(&g_lock);
    if (g_debug)
        printf("[MP6-DECODE-AHEAD] archive %u bytes, %u files, %llu KB to decode\n", (unsigned)length,
               (unsigned)count, (unsigned long long)(decoded >> 10));
    for (i = 0; i < queued; i++) mp6_host_sem_post(&g_work);
    return a;
}

/* ---- cooperative mode: archives the game holds, not registered yet (g_lock held) */

static void lazy_clear(AheadLazy *l)
{
    free(l->offsets);
    memset(l, 0, sizeof(*l));
}

static void lazy_add(const uint8_t *game, uint32_t length, int32_t disc, int stable)
{
    unsigned i;
    for (i = 0; i < AHEAD_LAZY_CAP; i++) {
        AheadLazy *l = &g_lazy[i];
        if (l->game && l->game < game + length && game < l->game + l->length) lazy_clear(l);
    }
    retire_views(game, length); /* not what was there before */
    {
        AheadLazy *l = &g_lazy[g_lazyNext++ % AHEAD_LAZY_CAP];
        lazy_clear(l);
        l->game = game;
        l->length = length;
        l->disc = disc;
        l->stable = stable;
    }
}

/* take() found no view: note the file if it is in a deferred archive. */
static void lazy_note(const uint8_t *p)
{
    unsigned i;
    for (i = 0; i < AHEAD_LAZY_CAP; i++) {
        AheadLazy *l = &g_lazy[i];
        if (!l->game || p < l->game || p >= l->game + l->length) continue;
        if (l->count == l->cap) {
            uint32_t cap = l->cap ? l->cap * 2 : 32;
            uint32_t *grown = (uint32_t *)realloc(l->offsets, cap * sizeof(uint32_t));
            if (!grown) return;
            l->offsets = grown;
            l->cap = cap;
        }
        l->offsets[l->count++] = (uint32_t)(p - l->game);
        return;
    }
}

static AheadLazy *lazy_find(int32_t disc, const uint8_t *stable, uint32_t length)
{
    unsigned i;
    for (i = 0; i < AHEAD_LAZY_CAP; i++) {
        AheadLazy *l = &g_lazy[i];
        if (l->game && l->length == length && (disc >= 0 ? l->disc == disc : l->game == stable)) return l;
    }
    return NULL;
}

/* The archive in `bytes` is about to be registered: the files the game
 * took from its own copy meanwhile become learned usage, so registration
 * queues those (and only those, when nothing was learned before). */
static void lazy_learn(const AheadLazy *l, const uint8_t *bytes, uint32_t length, uint64_t hash)
{
    uint32_t count = be32(bytes), i, k;
    AheadUsage *u;
    if (l->count == 0 || count == 0 || count > AHEAD_MAX_FILES || 4u + (uint64_t)count * 4u > length) return;
    u = usage_find(hash, length, count);
    if (!u) u = usage_add(hash, length, count);
    if (!u) return;
    for (i = 0; i < count; i++) {
        uint32_t offset = be32(bytes + 4 + i * 4) + 8; /* past the size and type words */
        for (k = 0; k < l->count; k++) {
            if (l->offsets[k] != offset) continue;
            if (!(u->bits[i >> 3] & (1u << (i & 7)))) {
                u->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
                g_usageDirty = 1;
            }
            break;
        }
    }
}

/* An archive at `base`: start decoding what it holds; `view` also records
 * base as a place the game keeps it. `disc` (or -1) is the disc file the
 * bytes were read from whole: a file seen before is matched by that alone,
 * skipping the hash and compare of the whole archive on the game thread. */
static void archive_view(const void *base, uint32_t length, int stable, int view, int32_t disc)
{
    const uint8_t *game = (const uint8_t *)base;
    uint64_t hash = 0;
    AheadArchive *a = NULL;
    unsigned queued, i;

    if (length < 8 || !ahead_enabled()) return;
    mp6_host_mutex_lock(&g_lock);
    if (disc >= 0) {
        /* A disc file is identified by itself; its archive is not hashed. */
        for (a = g_archives; a; a = a->next) {
            if (a->disc == disc && a->length == length) break;
        }
    }
    if (!a && g_coop && view && !g_inIdle) {
        /* The game's own load, of an archive not known yet: note it and
         * register it in idle time, not on top of the load. */
        if (disc >= 0 || stable) lazy_add(game, length, disc, stable);
        mp6_host_mutex_unlock(&g_lock);
        if (disc >= 0) mp6_decode_ahead_want_disc(disc, length);
        else if (stable) mp6_decode_ahead_want_stable(base, length);
        return;
    }
    if (disc < 0) {
        mp6_host_mutex_unlock(&g_lock);
        hash = content_hash(game, length);
        mp6_host_mutex_lock(&g_lock);
        for (a = g_archives; a; a = a->next) {
            if (a->hash == hash && a->length == length && memcmp(a->bytes, game, length) == 0) break;
        }
    }
    if (!a) {
        mp6_host_mutex_unlock(&g_lock);
        /* A disc file's learned usage is kept under its file, not its bytes. */
        if (disc >= 0) hash = 0xD15C000000000000ull ^ (uint64_t)(uint32_t)disc;
        a = new_archive(game, length, hash);
        if (!a) return;
        mp6_host_mutex_lock(&g_lock);
    }
    if (disc >= 0) a->disc = disc;
    touch_archive(a);
    if (view) add_view(game, length, 0, a, stable);
    queued = requeue_used(a);
    mp6_host_mutex_unlock(&g_lock);
    for (i = 0; i < queued; i++) mp6_host_sem_post(&g_work);
}

/* ---- cooperative mode: work in the game thread's idle time -------------- */

static void want(int32_t disc, const void *stable, uint32_t length)
{
    unsigned i;
    if (!g_coop || length < 8) return;
    for (i = g_pendingHead; i != g_pendingTail; i++) {
        AheadPending *q = &g_pending[i % AHEAD_PENDING_CAP];
        if (q->disc == disc && q->stable == stable && q->length == length) return;
    }
    if (g_pendingTail - g_pendingHead >= AHEAD_PENDING_CAP) g_pendingHead++; /* drop the oldest */
    g_pending[g_pendingTail++ % AHEAD_PENDING_CAP] = (AheadPending){ disc, (const uint8_t *)stable, length, 0 };
}

void mp6_decode_ahead_want_disc(int32_t disc, uint32_t length)
{
    if (disc >= 0 && ahead_enabled()) want(disc, NULL, length);
}

void mp6_decode_ahead_want_stable(const void *base, uint32_t length)
{
    if (base != NULL && ahead_enabled()) want(-1, base, length);
}

/* One pending archive into decode-ahead: 1 done (or dropped), 0 not yet. */
static int register_pending(AheadPending *q)
{
    int done = 1;
    AheadLazy *l;
    g_inIdle = 1;
    if (q->disc >= 0) {
        uint8_t *bytes = (uint8_t *)malloc(q->length);
        int64_t got = bytes ? mp6_dvd_read_entry_now(q->disc, bytes, q->length) : -1;
        if (got == (int64_t)q->length) {
            mp6_host_mutex_lock(&g_lock);
            l = lazy_find(q->disc, NULL, q->length);
            if (l) lazy_learn(l, bytes, q->length, 0xD15C000000000000ull ^ (uint64_t)(uint32_t)q->disc);
            mp6_host_mutex_unlock(&g_lock);
            archive_view(bytes, q->length, 0, 0, q->disc);
            mp6_host_mutex_lock(&g_lock);
            l = lazy_find(q->disc, NULL, q->length);
            if (l) {
                /* The game may still be decoding from its copy this visit;
                 * take() checks the bytes before trusting it. */
                AheadArchive *a;
                for (a = g_archives; a; a = a->next) {
                    if (a->disc == q->disc && a->length == q->length) break;
                }
                if (a) add_view(l->game, l->length, 0, a, l->stable);
                lazy_clear(l);
            }
            mp6_host_mutex_unlock(&g_lock);
        } else if (got < 0 && ++q->tries < 600) {
            done = 0; /* the page has not fetched it yet */
        }
        free(bytes);
    } else {
        uint64_t hash = content_hash(q->stable, q->length);
        mp6_host_mutex_lock(&g_lock);
        l = lazy_find(-1, q->stable, q->length);
        if (l) {
            lazy_learn(l, q->stable, q->length, hash);
            lazy_clear(l);
        }
        mp6_host_mutex_unlock(&g_lock);
        archive_view(q->stable, q->length, 1, 1, -1);
    }
    g_inIdle = 0;
    return done;
}

int mp6_decode_ahead_idle(int64_t deadlineNs)
{
    const int64_t margin = 1000000; /* leave the tick its deadline */
    int did = 0;
    unsigned waiting = 0; /* pending archives the page has not fetched, this call */
    int64_t start;
    if (!g_coop || g_enabled != 1) return 0;
    start = (int64_t)mp6_host_monotonic_ns();
    g_tickIdleCalls++;
    for (;;) {
        int64_t now = (int64_t)mp6_host_monotonic_ns();
        double left = (double)(deadlineNs - margin - now);
        if (left <= 0) break;
        if (g_pendingHead != g_pendingTail && waiting < g_pendingTail - g_pendingHead) {
            AheadPending *q = &g_pending[g_pendingHead % AHEAD_PENDING_CAP];
            if (q->length / 1024.0 * g_copyNsPerKB > left) {
                AheadPending later = *q; /* not in this slice: a later, emptier one */
                g_pendingHead++;
                g_pending[g_pendingTail++ % AHEAD_PENDING_CAP] = later;
                waiting++;
                continue;
            }
            if (register_pending(q)) {
                g_pendingHead++;
                g_copyNsPerKB = 0.8 * g_copyNsPerKB +
                    0.2 * ((double)((int64_t)mp6_host_monotonic_ns() - now) / (q->length / 1024.0 + 1.0));
            } else {
                /* Not fetched yet: try the next one, this one again later. */
                AheadPending later = *q;
                g_pendingHead++;
                g_pending[g_pendingTail++ % AHEAD_PENDING_CAP] = later;
                waiting++;
            }
            did = 1;
            continue;
        }
        {
            AheadEntry *e;
            mp6_host_mutex_lock(&g_lock);
            e = pop_job(left);
            mp6_host_mutex_unlock(&g_lock);
            if (e == NULL) break;
            run_job(e);
            g_tickIdleJobs++;
            g_decodeNsPerKB = 0.8 * g_decodeNsPerKB +
                0.2 * ((double)((int64_t)mp6_host_monotonic_ns() - now) / (e->size / 1024.0 + 1.0));
            did = 1;
        }
    }
    g_tickIdleNs += (uint64_t)((int64_t)mp6_host_monotonic_ns() - start);
    return did;
}

void mp6_decode_ahead_preload(const void *bytes, uint32_t length, int32_t disc)
{
    /* Another thread: only once the game thread has set the pool up. */
    if (g_enabled != 1) return;
    archive_view(bytes, length, 0, 0, disc);
}

void mp6_decode_ahead_disc_part(const void *base, uint32_t length, int32_t disc, uint32_t offset)
{
    AheadArchive *a;
    unsigned queued = 0, i;
    if (length < 8 || disc < 0 || g_enabled != 1) return;
    mp6_host_mutex_lock(&g_lock);
    for (a = g_archives; a; a = a->next) {
        if (a->disc == disc && (uint64_t)offset + length <= a->length) break;
    }
    if (a) {
        touch_archive(a);
        add_view((const uint8_t *)base, length, offset, a, 0);
        queued = requeue_used(a);
    }
    mp6_host_mutex_unlock(&g_lock);
    for (i = 0; i < queued; i++) mp6_host_sem_post(&g_work);
}

void mp6_decode_ahead_disc_read(const void *base, uint32_t length, int32_t disc)
{
    uint64_t t0 = ahead_enabled() && g_debug ? mp6_host_monotonic_ns() : 0;
    archive_view(base, length, 0, 1, disc);
    if (g_debug) g_tickMatchNs += mp6_host_monotonic_ns() - t0;
}

void mp6_decode_ahead_archive(const void *base, uint32_t length)
{
    archive_view(base, length, 0, 1, -1);
}

void mp6_decode_ahead_copy(void *dst, const void *src, uint32_t length, int stable)
{
    AheadView *v;
    if (length < 8 || !ahead_enabled()) return;
    mp6_host_mutex_lock(&g_lock);
    v = find_view((const uint8_t *)src, length);
    if (v) {
        add_view((const uint8_t *)dst, length, v->archiveOffset + (uint32_t)((const uint8_t *)src - v->game),
                 v->archive, stable);
        mp6_host_mutex_unlock(&g_lock);
        return;
    }
    mp6_host_mutex_unlock(&g_lock);
    archive_view(dst, length, stable, 1, -1);
}

int mp6_decode_ahead_take(const void *src, void *dst, uint32_t size, int32_t type)
{
    const uint8_t *p = (const uint8_t *)src;
    AheadView *v;
    AheadEntry *e = NULL;
    uint32_t viewOffset = 0;
    uint64_t t0 = 0;
    if (g_enabled != 1) return 0;
    if (g_debug) t0 = mp6_host_monotonic_ns();
    mp6_host_mutex_lock(&g_lock);
    v = find_view(p, 1);
    if (v) {
        viewOffset = (uint32_t)(p - v->game);
        e = find_entry(v->archive, v->archiveOffset + viewOffset);
    }
    if (e && (e->size != size || e->type != type)) e = NULL;
    if (e) usage_mark(e);
    if (!v && g_coop) lazy_note(p);
    if (e && e->state == AHEAD_RUNNING) {
        uint64_t t0 = mp6_host_monotonic_ns();
        while (e->state == AHEAD_RUNNING) {
            mp6_host_mutex_unlock(&g_lock);
            mp6_host_sleep_ns(0);
            mp6_host_mutex_lock(&g_lock);
        }
        g_waitNs += mp6_host_monotonic_ns() - t0;
        g_waits++;
    }
    /* The game's copy must hold everything the decode read, unchanged. */
    if (e && e->state == AHEAD_DONE && (uint64_t)viewOffset + e->consumed <= v->length &&
        memcmp(p, e->archive->bytes + e->offset, e->consumed) == 0) {
        if (g_verify) {
            g_verifyOut = (uint8_t *)malloc(size);
            if (g_verifyOut) memcpy(g_verifyOut, e->out, size);
        } else {
            memcpy(dst, e->out, size);
            if (e->hsfChecked) {
                unsigned k = g_hsfRecentNext++ % AHEAD_HSF_RECENT;
                g_hsfRecent[k].dst = dst;
                g_hsfRecent[k].size = size;
                g_hsfRecent[k].tick = g_aheadTick;
                g_hsfRecent[k].hsf = e->hsf;
            }
        }
        touch_archive(e->archive);
        g_hits++;
        if (g_debug) {
            g_tickHitNs += mp6_host_monotonic_ns() - t0;
            g_tickHits++;
        }
        mp6_host_mutex_unlock(&g_lock);
        return !g_verify;
    }
    if (g_debug)
        printf("[MP6-DECODE-AHEAD] miss size=%u type=%d: %s\n", (unsigned)size, (int)type,
               !v ? "no view" : !e ? "no file" : e->state == AHEAD_QUEUED ? "queued"
               : e->state == AHEAD_FAILED ? "failed" : "changed");
    if (e && e->state == AHEAD_QUEUED && !g_verify) {
        /* The game decodes it now; a worker reaching it later would only
         * spend time the files still ahead of the game need. */
        e->state = AHEAD_CLAIMED;
        free(e->out);
        e->out = NULL;
        g_bytes -= e->size;
    }
    g_misses++;
    g_tickMisses++;
    g_tickMissBytes += size;
    mp6_host_mutex_unlock(&g_lock);
    return 0;
}

void mp6_decode_ahead_verify(const void *dst, uint32_t size)
{
    if (!g_verifyOut) return;
    g_verified++;
    if (memcmp(dst, g_verifyOut, size) != 0) {
        g_mismatches++;
        printf("[MP6-DECODE-AHEAD] MISMATCH size=%u (%llu of %llu)\n", (unsigned)size,
               (unsigned long long)g_mismatches, (unsigned long long)g_verified);
    }
    free(g_verifyOut);
    g_verifyOut = NULL;
}

int mp6_decode_ahead_hsf_preflight(const void *data, size_t capacity, Mp6HsfPreflight *out)
{
    unsigned k;
    if (g_enabled != 1) return 0;
    for (k = 0; k < AHEAD_HSF_RECENT; k++) {
        if (g_hsfRecent[k].dst == data && g_hsfRecent[k].tick == g_aheadTick && capacity >= g_hsfRecent[k].size) {
            *out = g_hsfRecent[k].hsf;
            g_hsfRecent[k].dst = NULL;
            return 1;
        }
    }
    return 0;
}

void mp6_decode_ahead_tick(void)
{
    static long tick;
    if (g_enabled != 1) return;
    tick++;
    g_aheadTick = tick;
    mp6_host_mutex_lock(&g_lock);
    if (g_debug && g_coop && g_tickIdleCalls && tick % 60 == 0) {
        printf("[MP6-DECODE-AHEAD] idle calls=%llu jobs=%llu %.2fms pending=%u (head %uKB tries %u) queued=%u "
               "held=%lluKB rate=%.0f/%.0fns/KB\n",
               (unsigned long long)g_tickIdleCalls, (unsigned long long)g_tickIdleJobs, g_tickIdleNs / 1e6,
               g_pendingTail - g_pendingHead,
               g_pendingHead != g_pendingTail ? g_pending[g_pendingHead % AHEAD_PENDING_CAP].length >> 10 : 0,
               g_pendingHead != g_pendingTail ? g_pending[g_pendingHead % AHEAD_PENDING_CAP].tries : 0,
               (g_urgentTail - g_urgentHead) + (g_queueTail - g_queueHead),
               (unsigned long long)(held_bytes() >> 10), g_decodeNsPerKB, g_copyNsPerKB);
        g_tickIdleCalls = g_tickIdleJobs = g_tickIdleNs = 0;
    }
    if (g_debug && (g_waits || g_tickHits || g_tickMisses || g_tickMatchNs)) {
        printf("[MP6-DECODE-AHEAD] tick waits=%llu %.2fms hits=%llu %.2fms misses=%llu %lluKB match=%.2fms "
               "total hits=%llu misses=%llu held=%lluKB\n",
               (unsigned long long)g_waits, g_waitNs / 1e6, (unsigned long long)g_tickHits,
               g_tickHitNs / 1e6, (unsigned long long)g_tickMisses,
               (unsigned long long)(g_tickMissBytes >> 10), g_tickMatchNs / 1e6, (unsigned long long)g_hits,
               (unsigned long long)g_misses, (unsigned long long)(g_bytes >> 10));
        g_waits = g_waitNs = 0;
        g_tickHits = g_tickHitNs = g_tickMisses = g_tickMissBytes = g_tickMatchNs = 0;
    }
    if (g_usageDirty && tick % 600 == 0) {
        g_usageDirty = 0;
        usage_save();
    }
    if (g_verify && tick % 600 == 0 && g_verified)
        printf("[MP6-DECODE-AHEAD] verified=%llu mismatches=%llu misses=%llu\n",
               (unsigned long long)g_verified, (unsigned long long)g_mismatches,
               (unsigned long long)g_misses);
    mp6_host_mutex_unlock(&g_lock);
}

void mp6_decode_guard(const void *dst, uint32_t size, int32_t type, long long wrote, uint32_t left)
{
    static int on = -1;
    const unsigned char *d0 = (const unsigned char *)dst;
    unsigned sum = 5381u;
    uint32_t k;
    if (on < 0) {
        const char *e = getenv("MP6_DECODE_GUARD");
        on = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (!on) return;
    for (k = 0; k < size; k++) sum = sum * 33u + d0[k];
    printf("[DECODE] type=%d dst=%p..%p size=%u wrote=%lld left=%u sum=%08x%s\n", (int)type, dst,
           (const void *)(d0 + size), (unsigned)size, wrote, (unsigned)left, sum,
           (type != HU_TYPE_ZLIB && wrote != (long long)size) ? "  <-- LENGTH MISMATCH (overrun/underrun)" : "");
    fflush(stdout);
}
