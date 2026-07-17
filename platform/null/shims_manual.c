/* MP6 native port -- hand-written SDK shims that need REAL behavior, not a
 * logging no-op. See tools/gen_shims.py's MANUAL_SYMBOLS set for why each
 * of these is here instead of in the auto-generated
 * platform/null/shims_generated.c.
 */
#include "dolphin.h"
#include "dolphin/mic.h" /* MICProbeEx/MICMount, see their own comment below */
#include "mp6_shim_log.h"
#include "mp6_boot.h"
#include "host.h" /* mp6_host_monotonic_ns/mp6_host_wallclock (the OSGetTime
                   * timebase below) + mp6_host_rss_bytes (RSS watchdog +
                   * [LEAKHUNT]) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "zlib.h"
#ifdef MP6_LEAKHUNT_DEBUG
#include "game/memory.h"
#endif

/* -------------------------------------------------------------------
 * OSReport -- the game's OWN diagnostic channel, not hardware.
 * Deliberately NOT rate-limited (unlike every other shim): a readable
 * boot trace matters here, and OSReport's actual formatted text (DLL link
 * steps, memory sizes, ...) carries far more signal than
 * "[SDK] OSReport(...) seen once" ever could -- this is the explicit,
 * deliberate exception to the blanket log-once policy.
 *
 * OSPanic has NO definition here at all -- src/game/fault.c provides a
 * real one (draws an on-screen panic message via its own bitmap-font
 * framebuffer writer, then calls PPCHalt()); see PPCHalt below for where
 * the actual process-exit happens instead.
 * ------------------------------------------------------------------- */
void OSReport(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vfprintf(stdout, msg, ap);
    va_end(ap);
    fflush(stdout);
}

/* Real hardware never returns from this. It's the actual termination step
 * at the end of src/game/fault.c's own OSPanic (see above) -- a clean,
 * distinguishable non-zero exit is more useful than either silently
 * continuing (masking the problem) or crashing unceremoniously. */
void PPCHalt(void)
{
    fprintf(stdout, "[PANIC] PPCHalt() -- game-side OSPanic (src/game/fault.c) halted the system\n");
    fflush(stdout);
    exit(1);
}

/* -------------------------------------------------------------------
 * OSGetTick / OSGetTime -- real elapsed wall-clock time, scaled to the
 * GameCube's documented timer rate, so game-side millisecond-based waits
 * (OSTicksToMilliseconds(OSGetTick() - start) < N) resolve like they
 * would on real hardware instead of never (a naive "+1 per call" counter
 * would take ~1.6M calls to reach 1000ms worth of ticks) or immediately.
 * Also backs __OSBusClock/__OSCoreClock with real GameCube constants
 * (libogc-documented: 162MHz bus / 486MHz core) so OS_TIMER_CLOCK
 * (OS_BUS_CLOCK/4, used by the OSTicksToMilliseconds macro) isn't 0 --
 * every dolphin/os.h AT_ADDRESS(...) global defaults to zero-initialized
 * otherwise, which would make that macro a divide-by-zero.
 * ------------------------------------------------------------------- */
void mp6_os_globals_init(void)
{
    /* Real GameCube constants (documented widely, e.g. libogc): 162MHz
     * bus / 486MHz core. Without this, __OSBusClock/__OSCoreClock default
     * to zero-initialized (dolphin/os.h's AT_ADDRESS(...) globals, empty
     * under non-MWERKS -> plain tentative definitions), making
     * OS_TIMER_CLOCK (OS_BUS_CLOCK/4) zero and every OSTicksToMilliseconds
     * call a divide-by-zero. */
    __OSBusClock = 162000000u;
    __OSCoreClock = 486000000u;
}

static int g_timebaseInit; /* the raw counter comes from
                            * mp6_host_monotonic_ns (fixed 1e9/s timebase) */
static s64 g_rtcBaseTicks; /* ticks from 2000-01-01 00:00:00 (local) to process start */

/* Days from civil date to days-since-1970-01-01 (Howard Hinnant's
 * public-domain civil-days algorithm) -- used both to seed the RTC base
 * below and, inverted, by OSTicksToCalendarTime. */
static s64 mp6_days_from_civil(int y, int m, int d)
{
    s64 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                      /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;     /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static s64 mp6_os_tick64(void)
{
    if (!g_timebaseInit) {
        /* Seed the timebase from the host's real-time clock, exactly like
         * the GC boot ROM seeds the PPC timebase from the EXI RTC (and like
         * Dolphin does from the host clock) -- OSGetTime() is "ticks since
         * 2000-01-01 00:00:00" on every platform this game ever ran on, and
         * game code treats it that way: saveload.c stamps GwCommon.time
         * with it and fileseldll renders it as the save file's calendar
         * date. An uptime-based value (~0 at boot) would make every
         * natively-created save carry a 01/01/2000 date. Local time, not
         * UTC: the GC RTC counts the user's wall-clock time (it has no
         * zone), and Dolphin's default RTC source is the host's local
         * clock too -- the user's own Dolphin save decodes to the local
         * evening they actually played. Tick DELTAS (all the
         * OSTicksToMilliseconds(now - start) waits in game code) are
         * unaffected by the constant base.
         * Overflow: 2^63 / 40.5e6 ticks/s covers ~7,200 years of seconds --
         * (secs2000 * OS_TIMER_CLOCK) is nowhere near saturating s64.
         * The raw primitives live behind the host seam
         * (mp6_host_wallclock for the local calendar read,
         * mp6_host_monotonic_ns for the fixed 1e9/s elapsed-time counter). */
        Mp6DateTime lt;
        s64 secs2000;
        mp6_host_wallclock(&lt);
        secs2000 = (mp6_days_from_civil(lt.year, lt.mon, lt.day) - 10957) * 86400
                 + lt.hour * 3600 + lt.min * 60 + lt.sec; /* 10957 = days 1970-01-01 .. 2000-01-01 */
        g_rtcBaseTicks = secs2000 * 40500000
                       + (s64)((double)lt.msec * 40500.0)
                       - (s64)((double)mp6_host_monotonic_ns() / 1000000000.0 * 40500000.0);
        g_timebaseInit = 1;
    }
    /* OS_TIMER_CLOCK = OS_BUS_CLOCK / 4 = 40,500,000 Hz on real hardware. */
    return g_rtcBaseTicks + (s64)((double)mp6_host_monotonic_ns() / 1000000000.0 * 40500000.0);
}

OSTime OSGetTime(void)
{
    MP6_LOG_ONCE("OS", "OSGetTime");
    return (OSTime)mp6_os_tick64();
}

OSTick OSGetTick(void)
{
    MP6_LOG_ONCE("OS", "OSGetTick");
    return (OSTick)mp6_os_tick64();
}

/* -------------------------------------------------------------------
 * OSTicksToCalendarTime -- REAL tick -> calendar decomposition. The
 * auto-generated weak stub writes NOTHING to *td, so the file-select save
 * tags would render mon+1/mday/year from an all-zero struct -- a
 * "01/00/0000" date on every save, no matter how correct GwCommon.time
 * was. This strong definition overrides the weak one at link.
 *
 * Semantics matched to the GC SDK: input is ticks since 2000-01-01
 * 00:00:00 at OS_TIMER_CLOCK (40.5MHz); outputs per dolphin/os.h's own
 * field comments (mon 0-11, year AD, wday 0=Sunday, yday 0-based).
 * Floor semantics for pre-2000 (negative) inputs so a garbage time from
 * a corrupt save decodes to SOME valid calendar date instead of UB.
 * ------------------------------------------------------------------- */
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime *td)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    s64 secs, rem, days, dsec, z, era, doe, yoe, doy, mp;
    int leap, m;

    MP6_LOG_ONCE("OS", "OSTicksToCalendarTime");
    /* split ticks -> whole seconds + sub-second remainder (floor) */
    secs = ticks / 40500000;
    rem = ticks % 40500000;
    if (rem < 0) {
        rem += 40500000;
        secs -= 1;
    }
    td->msec = (int)(rem / 40500);                     /* 40500 ticks per ms  */
    td->usec = (int)((rem % 40500) * 1000 / 40500);    /* then us within the ms */
    /* split seconds -> days + second-of-day (floor) */
    days = secs / 86400;
    dsec = secs % 86400;
    if (dsec < 0) {
        dsec += 86400;
        days -= 1;
    }
    td->hour = (int)(dsec / 3600);
    td->min = (int)((dsec % 3600) / 60);
    td->sec = (int)(dsec % 60);
    td->wday = (int)((days % 7 + 7 + 6) % 7); /* 2000-01-01 was a Saturday (6) */
    /* civil date from days since 2000-01-01 (Hinnant civil_from_days,
     * epoch-shifted: +10957 days to 1970, +719468 to era base) */
    z = days + 10957 + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                        /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* [0, 399]    */
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                 /* [0, 365]    */
    mp = (5 * doy + 2) / 153;                                      /* [0, 11]     */
    td->mday = (int)(doy - (153 * mp + 2) / 5 + 1);                /* [1, 31]     */
    m = (int)(mp + (mp < 10 ? 3 : -9));                            /* [1, 12]     */
    td->year = (int)(yoe + era * 400) + (m <= 2);
    td->mon = m - 1;                                               /* [0, 11]     */
    /* day-of-year from the civil date */
    leap = (td->year % 4 == 0 && td->year % 100 != 0) || td->year % 400 == 0;
    td->yday = td->mday - 1;
    for (m = 0; m < td->mon; m++) {
        td->yday += mdays[m];
    }
    if (td->mon > 1 && leap) {
        td->yday += 1; /* past February in a leap year */
    }
}

/* -------------------------------------------------------------------
 * VI frame-pacing / tick-budget state. Shared by BOTH build modes (see
 * tools/build.py's --headless flag / MP6_HEADLESS_BUILD):
 *   - --headless: this file's own VIWaitForRetrace (below) calls
 *     mp6_tick_and_maybe_exit(), which exits(0) directly -- no Aurora
 *     device/frame state to worry about tearing down.
 *   - default (aurora) build: platform/gx/aurora_bridge.c provides the
 *     REAL VIWaitForRetrace (ends/begins aurora frames, pumps SDL events,
 *     handles window-close) and calls mp6_tick_advance() instead (see
 *     mp6_boot.h's own comment for why a PLAIN exit(0) here -- skipping
 *     aurora_shutdown() -- made Aurora's own teardown treat the device
 *     loss as fatal and abort() instead of exiting cleanly).
 * mp6_max_ticks/mp6_tick_count/mp6_tick_advance() therefore stay
 * unconditional (not `#ifdef MP6_HEADLESS_BUILD`) -- they are the shared
 * state/logic both VIWaitForRetrace implementations build on, not
 * duplicated between them.
 * ------------------------------------------------------------------- */
int mp6_max_ticks = 60;
long mp6_tick_count = 0;
int mp6_ticks_unlimited = 0; /* aurora build's no-arg (double-click) default -- see mp6_boot.h */

/* -------------------------------------------------------------------
 * RSS watchdog: the exe samples its own RSS and aborts loudly past a
 * configurable cap (MP6_RSS_CAP_MB, default 4096) -- a silent
 * multi-gigabyte machine-eater becomes a loud, small, diagnosable
 * failure instead. Lives here rather than in
 * platform/gx/aurora_bridge.c for the exact same reason mp6_tick_advance
 * itself does (see this section's own header comment above): this file
 * is shared, unconditionally compiled into BOTH build modes, and
 * mp6_tick_advance() is the one hook both VIWaitForRetrace
 * implementations already call every single tick -- piggybacking the
 * watchdog here guarantees it actually runs in both builds from exactly
 * one call site, with nothing new to wire up per build mode.
 *
 * Deliberately does NOT try to route through Aurora's own clean-shutdown
 * dance (mp6_clean_shutdown_exit, platform/gx/aurora_bridge.c) even in
 * the windowed build: this file also links into the headless build,
 * which has no Aurora device to shut down at all, and a hard RSS cap is
 * exactly the "stop right now" case, not a graceful teardown -- printing
 * the reason loudly (both streams, flushed) BEFORE exiting means the
 * actual diagnosis survives on-screen/in-log even if Aurora's own static
 * destructors later treat the abrupt exit as a device loss. */
#define MP6_RSS_WATCHDOG_INTERVAL_TICKS 600
#define MP6_RSS_CAP_MB_DEFAULT 4096.0

static double mp6_rss_cap_mb(void)
{
    static double cap = -1.0;
    if (cap < 0.0) {
        const char *env = getenv("MP6_RSS_CAP_MB");
        double parsed;
        char *end = NULL;
        cap = MP6_RSS_CAP_MB_DEFAULT;
        if (env && env[0]) {
            parsed = strtod(env, &end);
            if (end != env && parsed > 0.0) {
                cap = parsed;
            }
        }
    }
    return cap;
}

static void mp6_rss_watchdog_check(void)
{
    size_t rssBytes;
    double rssMb, capMb;

    if (mp6_tick_count == 0 || (mp6_tick_count % MP6_RSS_WATCHDOG_INTERVAL_TICKS) != 0) {
        return;
    }
    rssBytes = mp6_host_rss_bytes(); /* behind the host seam, not a direct OS call */
    if (rssBytes == 0) {
        return; /* can't sample -- fail open, don't kill the process over a diagnostics failure */
    }
    rssMb = (double)rssBytes / (1024.0 * 1024.0);
    capMb = mp6_rss_cap_mb();
    if (rssMb > capMb) {
        fprintf(stderr,
                "[RSS-WATCHDOG] FATAL: working set %.1f MB exceeded the %.1f MB cap (tick=%ld) "
                "-- aborting loudly instead of silently eating machine memory. Override with the "
                "MP6_RSS_CAP_MB env var if this is expected.\n",
                rssMb, capMb, mp6_tick_count);
        fflush(stderr);
        printf("[RSS-WATCHDOG] FATAL: working set %.1f MB exceeded the %.1f MB cap (tick=%ld) "
               "-- exiting 1\n", rssMb, capMb, mp6_tick_count);
        fflush(stdout);
        exit(1);
    }
}

int mp6_tick_advance(void)
{
    mp6_tick_count++;
    mp6_rss_watchdog_check();
    mp6_alloc_census_tick_check(); /* leak hunt -- see mp6_boot.h/malloc_direct.c */
    /* Checked once, on this very first tick -- late enough that arena/heap
     * init has already run in both build modes (see mp6_boot.h's own
     * comment on this function). No-op unless MP6_TEST_LOAD_DLL is set. */
    mp6_dll_bridge_selftest_check_env();
    if (mp6_ticks_unlimited) return 0;
    return mp6_tick_count >= mp6_max_ticks;
}

void mp6_tick_and_maybe_exit(void)
{
    if (mp6_tick_advance()) {
        printf("[BOOT] reached %ld VIWaitForRetrace ticks (limit %d) -- exiting 0\n",
               mp6_tick_count, mp6_max_ticks);
        fflush(stdout);
        exit(0);
    }
}

#ifdef MP6_HEADLESS_BUILD
/* -------------------------------------------------------------------
 * --headless only. In the default (aurora) build these same symbol
 * names are provided instead by platform/gx/aurora_bridge.c (VI*) and by
 * Aurora's own libaurora_gx.a (GXInit, a real, self-contained CPU-side
 * FIFO/shadow-register reset with no GPU/window dependency, safe to link
 * unconditionally). Excluding them here (rather than relying on the
 * weak-symbol trick gen_shims.py's generated shims use) keeps the
 * --headless path an exact match of this file's own hand-written
 * behavior, with zero risk of ever silently picking up an Aurora symbol
 * by accident.
 * ------------------------------------------------------------------- */
void VIInit(void)
{
    MP6_LOG_ONCE("VI", "VIInit");
}

void VIWaitForRetrace(void)
{
    MP6_LOG_ONCE("VI", "VIWaitForRetrace");
#ifdef MP6_LEAKHUNT_DEBUG
    if (mp6_tick_count > 0 && mp6_tick_count % 300000 == 0) {
        /* mp6_host_rss_bytes returns 0 on a failed sample, so this prints
         * a plain 0.00 rather than propagating an error. */
        fprintf(stderr, "[LEAKHUNT] tick=%ld rss=%.2fMB HEAP_HEAP=%d HEAP_SOUND=%d HEAP_MODEL=%d HEAP_DVD=%d\n",
                mp6_tick_count, (double)mp6_host_rss_bytes() / (1024.0*1024.0),
                HuMemUsedMallocSizeGet(HEAP_HEAP), HuMemUsedMallocSizeGet(HEAP_SOUND),
                HuMemUsedMallocSizeGet(HEAP_MODEL), HuMemUsedMallocSizeGet(HEAP_DVD));
        fflush(stderr);
    }
#endif
    mp6_tick_and_maybe_exit();
}

u32 VIGetRetraceCount(void)
{
    MP6_LOG_ONCE("VI", "VIGetRetraceCount");
    return (u32)mp6_tick_count;
}

u32 VIGetNextField(void)
{
    MP6_LOG_ONCE("VI", "VIGetNextField");
    return (u32)(mp6_tick_count & 1);
}

/* GXInit -- needs to hand back a plausible non-null GXFifoObj* (game/
 * init.c stores it and passes it straight to other now-no-op GX calls;
 * nothing dereferences its innards meaningfully in headless mode). */
static GXFifoObj g_mp6FakeFifo;

GXFifoObj *GXInit(void *base, u32 size)
{
    MP6_LOG_ONCE("GX", "GXInit");
    (void)base;
    (void)size;
    memset(&g_mp6FakeFifo, 0, sizeof(g_mp6FakeFifo));
    return &g_mp6FakeFifo;
}
#endif /* MP6_HEADLESS_BUILD */

/* -------------------------------------------------------------------
 * THPInit -- THP full-motion-video playback is out of scope for this
 * port: this game's THP videos are per-minigame intros under
 * movie/*.thp, never on the boot-to-menu path this port covers.
 * game/THPSimple.c's THPSimpleInit() calls this FIRST, unconditionally,
 * before touching DVD or parsing anything:
 * `if (THPInit() == FALSE) { return 0; }`. Returning FALSE here is a
 * complete, self-contained "no THP codec available" gate -- Initialized
 * (THPSimple.c's own static) never becomes 1, so THPSimpleOpen()'s own
 * very first check (`if (Initialized == 0) return 0;`) then makes EVERY
 * subsequent THPSimpleOpen/THPSimpleRead call fail immediately too, with
 * no DVD access and no big-endian THP-header parsing ever attempted.
 *
 * Traced directly (not guessed): src/REL/bootDll/boot.c's entire
 * BootExec/BootWarningExec/BootTitleExec state machine -- the whole
 * warning -> title -> DLL_fileseldll flow this port covers -- never
 * calls HuTHPSprCreate/HuTHP3DCreate (or anything else THP-named) at all,
 * so this gate is defense-in-depth for a code path this port doesn't
 * reach, not something the current flow exercises.
 *
 * Known residual risk for whoever reaches THP next: game/thpmain.c's
 * THPTestProc (real decomp code, a HuPrc process HuTHPSprCreate/
 * HuTHP3DCreate spawn) retries `while (THPSimpleOpen(...) == 0) { ...
 * HuPrcVSleep(); }` with NO give-up path at all -- THPInit()==FALSE makes
 * each individual retry cheap (no I/O), but the loop itself still spins
 * (and OSReports "THPSimpleOpen fail" every tick) forever if anything
 * ever calls those entry points while THP stays stubbed. Fixing that
 * would mean either patching thpmain.c's retry loop (a real, narrow,
 * justifiable decomp patch) or making THPInit()/THPSimpleOpen() succeed
 * against a fabricated zero-frame/zero-component THP header so the loop
 * exits via success on its first try -- deliberately not done here since
 * it's provably unreachable on this port's own boot-to-menu scope.
 * ------------------------------------------------------------------- */
BOOL THPInit(void)
{
    MP6_LOG_ONCE("THP", "THPInit");
    return FALSE;
}

/* -------------------------------------------------------------------
 * AR/ARQ -- ARAM as a synchronous DMA: the callback fires immediately
 * (unblocking every `while (HuARDMACheck());`-style busy wait in
 * game/armem.c) rather than after a real async transfer. ARInit/ARQInit/
 * ARCheckInit/ARGetSize below are pure bookkeeping no-ops; the actual
 * byte transfer happens in ARQPostRequest (further below), which gives
 * ARAM a real backing buffer and memcpy's into/out of it synchronously
 * before firing the callback.
 * ------------------------------------------------------------------- */
#define MP6_FAKE_ARAM_SIZE (16u * 1024u * 1024u)

BOOL ARCheckInit(void)
{
    MP6_LOG_ONCE("AR", "ARCheckInit");
    return TRUE; /* always "already initialized" -- ARInit/ARQInit below are no-ops anyway */
}

u32 ARInit(u32 *stack_index_addr, u32 num_entries)
{
    MP6_LOG_ONCE("AR", "ARInit");
    (void)stack_index_addr;
    (void)num_entries;
    return 0;
}

u32 ARGetSize(void)
{
    MP6_LOG_ONCE("AR", "ARGetSize");
    return MP6_FAKE_ARAM_SIZE;
}

void ARQInit(void)
{
    MP6_LOG_ONCE("AR", "ARQInit");
}

/* ARAM's real backing storage -- see this function's own comment below
 * for why a pure no-op is not an option here: real DVD data flows
 * through game/armem.c's ARAM-cache path, and dropping it silently
 * corrupts the game's own sprite/font reads. */
static u8 *g_fakeAram;

static u8 *mp6_aram_buf(void)
{
    if (!g_fakeAram) {
        g_fakeAram = (u8 *)calloc(1, MP6_FAKE_ARAM_SIZE);
    }
    return g_fakeAram;
}

/* Real hardware's ARAM is a second RAM bank reachable only via DMA
 * (ARQPostRequest/ARStartDMA) -- game/armem.c's HuAR_DVDtoARAM/
 * HuAR_ARAMtoMRAM route every window/font/icon/cursor ANM sprite (and
 * more) through a store-to-ARAM-then-load-back-from-ARAM round trip as a
 * caching optimization. A no-op stub here (fire the callback, touch zero
 * bytes) is not an option once real DVD data flows through this path: it
 * would silently discard every byte crossing it -- HuAR_ARAMtoMRAM's
 * destination buffer would come back as whatever was already sitting in
 * freshly-allocated heap memory, not real file content, corrupting
 * game/window.c's HuWinInit() font/icon/cursor ANM reads (this is not
 * theoretical: it manifested as a zig runtime pointer-overflow panic
 * inside HuSprAnimRead, traced back to a wildly wrong patNum that turned
 * out to be uninitialized memory, not a decode or endianness bug).
 *
 * Fixed by giving ARAM real backing storage (a lazily-allocated
 * MP6_FAKE_ARAM_SIZE-byte host buffer) and actually memcpy'ing between it
 * and real MRAM, dispatched on `type` exactly like real hardware's own DMA
 * engine would: AMEM_PTR (game/armem.h) is always a plain byte OFFSET into
 * ARAM starting at HU_AMEM_BASE (0x808000), never a real CPU-
 * dereferenceable address even on real hardware, so treating it as a
 * direct index into this buffer is the same shape of translation the real
 * DMA hardware performs (source/dest whichever side is "MRAM" is always a
 * real pointer; whichever side is "ARAM" is always ARAM-relative). */
void ARQPostRequest(ARQRequest *request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length, ARQCallback callback)
{
    MP6_LOG_ONCE("AR", "ARQPostRequest");
    (void)owner;
    (void)priority;
    {
        u8 *aram = mp6_aram_buf();
        if (!aram) {
            fprintf(stderr, "[WARN] ARQPostRequest: couldn't allocate the %u-byte fake ARAM buffer\n",
                    (unsigned)MP6_FAKE_ARAM_SIZE);
        } else if (type == ARQ_TYPE_MRAM_TO_ARAM
                   && (uint64_t)dest + (uint64_t)length <= (uint64_t)MP6_FAKE_ARAM_SIZE) {
            memcpy(aram + dest, (const void *)(uintptr_t)source, length);
        } else if (type == ARQ_TYPE_ARAM_TO_MRAM
                   && (uint64_t)source + (uint64_t)length <= (uint64_t)MP6_FAKE_ARAM_SIZE) {
            memcpy((void *)(uintptr_t)dest, aram + source, length);
        } else {
            fprintf(stderr,
                    "[WARN] ARQPostRequest: type=%u source=%#x dest=%#x length=%u out of the fake "
                    "%u-byte ARAM's bounds -- dropping this transfer\n",
                    type, source, dest, length, (unsigned)MP6_FAKE_ARAM_SIZE);
        }
    }
    if (callback) callback((u32)(uintptr_t)request);
}

/* -------------------------------------------------------------------
 * OS-internal globals the headers declare `extern` unconditionally
 * (include/dolphin/os/OSThread.h, OSContext.h) -- on real hardware these
 * live at fixed low-memory addresses the SDK's own internals write; here
 * they just need SOME storage since nothing in this slice's call graph
 * reaches deep enough into thread/context internals to need them to be
 * meaningfully populated.
 * ------------------------------------------------------------------- */
OSThread *__OSCurrentThread;
OSThreadQueue __OSActiveThreadQueue;
volatile OSContext *__OSFPUContext;

/* game/mic.c (`__OSFpscrEnableBits = __OSFpscrEnableBits & 0xffffffef;`)
 * reads/writes this directly with no header declaration anywhere in the
 * tree at all (implicitly declared, tolerated by -Wno-error=implicit-
 * function-declaration... except this one's a plain global, not a call,
 * so it just needs storage to exist for the linker). Real hardware: an
 * FPU status/control-register shadow the OS's own internals maintain. */
u32 __OSFpscrEnableBits;

/* -------------------------------------------------------------------
 * game/mic.c also calls 2 things with NO declaration ANYWHERE in the
 * decomp tree (not even an unguarded one -- genuinely missing headers):
 *   - __frsqrte: the PPC "reciprocal square root ESTIMATE" instruction,
 *     called directly (not via a header macro) from a HAND-INLINED
 *     Newton-Raphson sqrtf in src/REL/fileseldll/filename.c (its own
 *     comment explains it's written this way to match the original
 *     binary's float-constant pooling -- decomp-fidelity, not a bug).
 *     A real reciprocal sqrt makes the following Newton-Raphson
 *     refinement steps redundant-but-harmless rather than needing PPC
 *     hardware; only the ESTIMATE quality would differ from real
 *     hardware, and that level of float-precision fidelity does not
 *     matter here.
 *   - gsapi_EngineGetParam: a 3rd-party voice-recognition/speech engine
 *     call (mic-driven minigames), not part of the Dolphin SDK at all --
 *     no header, anywhere, declares it. Single call site; a no-op stub
 *     matching its K&R-implicit-declaration call shape is enough to link.
 * ------------------------------------------------------------------- */
double __frsqrte(double x)
{
    MP6_LOG_ONCE("CACHE", "__frsqrte");
    return (x > 0.0) ? (1.0 / sqrt(x)) : 0.0;
}

/* Same story as __frsqrte, but for PPC's "absolute value" instruction --
 * src/game/gamemes.c has its own hand-inlined `extern inline double
 * fabs(double x) { return __fabs(x); }` (again for original-binary
 * float-pooling fidelity, per that file's own comment) with no clang
 * equivalent for the intrinsic. A real fabs is just a sign-bit clear. */
double __fabs(double x)
{
    MP6_LOG_ONCE("CACHE", "__fabs");
    return fabs(x);
}

/* src/game/mapspace.c explicitly `#undef HuSetVecF` right after its own
 * includes (line 7) -- humath.h's HuSetVecF is a multi-statement,
 * no-braces macro (classic "if (x) HuSetVecF(...);" footgun if the
 * caller ever omits braces), so this file opts out and calls it as a
 * plain (implicitly declared, tolerated by -Wno-error=implicit-function-
 * declaration) function instead. Matches the macro's own semantics
 * exactly, just as a real callable function instead of inline text
 * substitution; HuVecF is `typedef Vec HuVecF` (humath.h), so Vec* here
 * is the identical type without needing humath.h's own include chain. */
void HuSetVecF(Vec *vec, float x, float y, float z)
{
    vec->x = x;
    vec->y = y;
    vec->z = z;
}

int gsapi_EngineGetParam(void *engine, int param, void *out)
{
    MP6_LOG_ONCE("OS", "gsapi_EngineGetParam");
    (void)engine;
    (void)param;
    (void)out;
    return 0;
}

/* -------------------------------------------------------------------
 * zlib -- src/game/decode.c's HuDecodeZlib calls real zlib inflate() to
 * decompress HU_DECODE_TYPE_ZLIB asset data. A faked no-op inflate() is
 * not an option once real disc data flows through this path: game/window.c's
 * HuWinInit() font-sprite read (data/win.bin, real disc data) would come
 * back as uninitialized memory instead of decompressed content.
 *
 * NOT hand-written here -- tools/build.py links the real zlib-ng build
 * Aurora's own CMake already fetches+builds
 * (external_refs/repos/aurora/build/_deps/zlib-build/libzlib.dll.a +
 * libzlib1.dll) into BOTH build modes: zlib-ng is plain C with zero
 * Aurora/C++ entanglement of its own, so reusing this already-built
 * artifact for --headless too avoids hand-rolling zlib-ng's own
 * SIMD/CPU-dispatch build machinery from source just for this port.
 * Confirmed ABI-compatible with decomp's OWN include/zlib.h (the header
 * game/decode.c actually compiles against): z_stream's 15 fields match
 * byte-for-byte in type/order between the two headers, and zlib-ng's
 * "zlib-compat" build mode exists specifically to guarantee this. Real
 * inflateInit_/inflate/inflateEnd now simply resolve to that library
 * (nothing here defines them, so there's nothing to conflict with it).
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * GXRenderModeObj data objects -- NOT SDK function calls (excluded from
 * sdk_surface.json's whitelist on purpose, see its methodology notes),
 * but game/init.c and REL/bootDll/boot.c take their address directly
 * (&GXNtsc480IntDf, &GXNtsc480Prog, ...), so real storage has to live
 * somewhere. All 30 are declared in one header (dolphin/gx/GXFrameBuffer.h)
 * so they're all defined here for completeness; only a handful are
 * actually referenced by this slice. Plausible standard-definition values
 * (640x480) for the ones this slice reads fields of; zeroed for the rest.
 *
 * Aurora's OWN lib/dolphin/gx/GXFrameBuffer.cpp provides REAL storage
 * (with real field values, not just plausible placeholders) for exactly
 * 3 of these 30 -- GXNtsc480IntDf, GXPal528IntDf, GXMpal480IntDf (verified
 * via grep: these are the only 3 with a real `= {...}` initializer over
 * there; every other one of the 30 is `extern`-only in Aurora's header,
 * matching decomp's, with no storage behind it at all). Those 3 are
 * `#ifdef MP6_HEADLESS_BUILD`-guarded below so Aurora's real definitions
 * win in the default build (confirmed empirically: this is a genuine
 * "duplicate symbol" link error otherwise, not something -fcommon's
 * tentative-definition merging papers over -- unlike the OTHER 26, which
 * link fine either way since nothing else ever defines them). The
 * remaining 27 stay unconditional, needed as real (if placeholder)
 * storage in BOTH build modes since Aurora allocates none of them. */
#define MP6_STD_MODE_INIT { 0, 640, 480, 480, 0, 0, 640, 480, 0, 0, 0, {{0}}, {0} }

GXRenderModeObj GXNtsc240Ds;
GXRenderModeObj GXNtsc240DsAa;
GXRenderModeObj GXNtsc240Int;
GXRenderModeObj GXNtsc240IntAa;
#ifdef MP6_HEADLESS_BUILD
GXRenderModeObj GXNtsc480IntDf = MP6_STD_MODE_INIT;
#endif
GXRenderModeObj GXNtsc480Int;
GXRenderModeObj GXNtsc480IntAa;
GXRenderModeObj GXNtsc480Prog = MP6_STD_MODE_INIT;
GXRenderModeObj GXNtsc480ProgAa;
GXRenderModeObj GXMpal240Ds;
GXRenderModeObj GXMpal240DsAa;
GXRenderModeObj GXMpal240Int;
GXRenderModeObj GXMpal240IntAa;
#ifdef MP6_HEADLESS_BUILD
GXRenderModeObj GXMpal480IntDf = MP6_STD_MODE_INIT;
#endif
GXRenderModeObj GXMpal480Int;
GXRenderModeObj GXMpal480IntAa;
GXRenderModeObj GXPal264Ds;
GXRenderModeObj GXPal264DsAa;
GXRenderModeObj GXPal264Int;
GXRenderModeObj GXPal264IntAa;
#ifdef MP6_HEADLESS_BUILD
GXRenderModeObj GXPal528IntDf = MP6_STD_MODE_INIT;
#endif
GXRenderModeObj GXPal528Int;
GXRenderModeObj GXPal524IntAa;
GXRenderModeObj GXEurgb60Hz240Ds;
GXRenderModeObj GXEurgb60Hz240DsAa;
GXRenderModeObj GXEurgb60Hz240Int;
GXRenderModeObj GXEurgb60Hz240IntAa;
GXRenderModeObj GXEurgb60Hz480IntDf;
GXRenderModeObj GXEurgb60Hz480Int;
GXRenderModeObj GXEurgb60Hz480IntAa;

/* -------------------------------------------------------------------
 * The save-data/CARD gate, hand-written for a REAL result instead of the
 * auto-generated `return 0` (see tools/gen_shims.py's MANUAL_SYMBOLS
 * entry for these 4 names).
 *
 * The auto-generated stub claimed CARD_RESULT_READY (0) unconditionally,
 * for EVERY channel, with none of the OUT parameters (memSize/sectorSize/
 * CARDStat*) ever written -- i.e. "a memory card is present and was
 * successfully probed/mounted", a claim this port cannot honor (there is
 * no real memory-card image backing any of this). Traced the real
 * consequence end to end, not guessed:
 *   game/card.c's HuCardSlotCheck/HuCardMount/HuCardSectorSizeGet declare
 *   their local `s32 sectorSize;`/`u32 sectorSize;` UNINITIALIZED and pass
 *   `&sectorSize` straight to CARDProbeEx/CARDGetSectorSize, trusting a
 *   real CARD_RESULT_READY to mean it was actually filled in -- with the
 *   old stub, it never was, so HuCardSectorSizeGet returns whatever
 *   garbage happened to be on the stack.
 *   REL/fileseldll/saveload.c's FileCardMount takes that garbage
 *   "sector size", compares it against the real GC constant (0x2000), and
 *   -- depending on the garbage's value on any given run -- unpredictably
 *   lands on `FileMessOut(8)`/`return -2` (a CARD_RESULT_WRONGDEVICE-
 *   shaped "this card can't be used with Mario Party 6" warning,
 *   confirmed directly: this exact message rendered correctly, in-game)
 *   instead of
 *   the plain, standard, extremely common "no memory card in this slot"
 *   message real hardware shows for a genuinely empty slot.
 *
 * Fixed by being honest instead: CARD_RESULT_NOCARD (-3) for both
 * CARDProbeEx and CARDCheck/CARDMount (in case some other, not-yet-
 * exercised call site invokes either directly rather than through
 * CARDProbeEx first), and 0 for CARDGetSectorSize's *size purely as
 * defense-in-depth (every real caller already returns before reading it,
 * on the negative result -- matching game/card.c's own `if (result < 0)
 * return result;` gate -- so this is never actually consumed, just cheap
 * insurance against a future caller that doesn't check first). This is
 * not a guess at "safe" values: CARD_RESULT_NOCARD is real hardware's own
 * well-tested, well-defined response to an empty slot, and every caller
 * in game/card.c already has correct, working handling for it (this
 * doesn't newly exercise unproven decomp code paths -- "no memory card"
 * is the single most common real-hardware scenario there is). No CARD*
 * function past this point (Open/Read/Write/Create/...) is reachable once
 * the mount gate honestly reports NOCARD, so this alone is the fix -- no
 * "no save data" behavior had to be invented, only reported truthfully.
 *
 * CARD-WIRING UPDATE (user request "setup a memory card on slot A"):
 * these four honest no-card gates are now HEADLESS-ONLY. The aurora build
 * links aurora's complete CARD backend instead (planning/
 * aurora_surface.json gained the CARD family, so shims_generated_aurora.c
 * no longer stubs any CARD symbol), configured by platform/os/
 * card_native.c's mp6_CARDInit interposer (GCI-folder storage under
 * ./saves). Keeping these here unguarded would be a duplicate-strong-
 * symbol link error against aurora's real implementations.
 * ------------------------------------------------------------------- */
#ifdef MP6_HEADLESS_BUILD
s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize)
{
    MP6_LOG_ONCE("CARD", "CARDProbeEx");
    (void)chan;
    if (memSize) *memSize = 0;
    if (sectorSize) *sectorSize = 0;
    return CARD_RESULT_NOCARD;
}

s32 CARDCheck(s32 chan)
{
    MP6_LOG_ONCE("CARD", "CARDCheck");
    (void)chan;
    return CARD_RESULT_NOCARD;
}

s32 CARDMount(s32 chan, void *workArea, CARDCallback detachCallback)
{
    MP6_LOG_ONCE("CARD", "CARDMount");
    (void)chan;
    (void)workArea;
    (void)detachCallback;
    return CARD_RESULT_NOCARD;
}

s32 CARDGetSectorSize(s32 chan, u32 *size)
{
    MP6_LOG_ONCE("CARD", "CARDGetSectorSize");
    (void)chan;
    if (size) *size = 0;
    return CARD_RESULT_NOCARD;
}
#endif /* MP6_HEADLESS_BUILD -- CARD no-card gates */

/* -------------------------------------------------------------------
 * MICProbeEx/MICMount -- the exact same "auto-stub dishonestly claims
 * success" shape as CARDProbeEx/CARDMount above (see tools/gen_shims.py's
 * MANUAL_SYMBOLS entry for the full call-chain trace: an auto-generated
 * `return 0` default would make game/mic.c's HuMCProbe report
 * MIC_RESULT_READY, which every one of its 3 real call sites treats as "a
 * mic really is plugged in", taking a full HuMCInit/HuMCMount path that
 * ends in a real, deterministic access violation inside HuMCInit's own
 * MCDVDRead -- confirmed via this project's own crash-handler
 * symbolication, reached from REL/fileseldll/filesel.c's fn_1_680, itself
 * called from FileselMain's own "no save flag" teardown branch, i.e.
 * squarely on the file-select -> mode-select path, not a contrived
 * route). Fixed the same honest way:
 * MIC_RESULT_NOCARD (-3) -- HuMCProbe's own retry loop
 * (`while(...) { result = MICProbeEx(...); if (result != -1) break; }`)
 * only continues past MIC_RESULT_BUSY (-1), so this also returns
 * immediately instead of paying its up-to-500ms busy-wait first. No mic/
 * bongos peripheral support is invented here, only reported truthfully --
 * matching every real call site's own existing, correct "no mic" branch. */
s32 MICProbeEx(s32 chan)
{
    MP6_LOG_ONCE("MIC", "MICProbeEx");
    (void)chan;
    return MIC_RESULT_NOCARD;
}

s32 MICMount(s32 chan, s16 *buffer, s32 size, MICCallback detachCallback)
{
    MP6_LOG_ONCE("MIC", "MICMount");
    (void)chan;
    (void)buffer;
    (void)size;
    (void)detachCallback;
    return MIC_RESULT_NOCARD;
}

/*  the ANDROID builds rename decomp's
 * PADControlMotor call sites to mp6_PADControlMotor (dolphin_compat.h,
 * #ifdef __ANDROID__ -- see the rationale there: motor commands must not
 * execute on HuPrc coroutine stacks because the Android path can reach a
 * JNI upcall). The windowed/aurora Android build implements it in
 * platform/gx/aurora_bridge.c as a defer-to-VIWaitForRetrace queue; THIS
 * definition is the HEADLESS android build's null backend, emitting the
 * byte-identical MP6_LOG_ONCE line the generated plain-name shim
 * (shims_generated.c PADControlMotor) emits on Windows -- the U-A1
 * 600-tick logdiff compares that exact "[SDK] PAD.PADControlMotor(...)"
 * line across platforms, so the name string must stay "PADControlMotor",
 * not the renamed symbol. Guarded so Windows builds (no rename, generated
 * shim serves) preprocess this file byte-identically. */
#if defined(__ANDROID__) && defined(MP6_HEADLESS_BUILD)
void mp6_PADControlMotor(s32 chan, u32 cmd)
{
    MP6_LOG_ONCE("PAD", "PADControlMotor");
    (void)chan;
    (void)cmd;
}
#endif
