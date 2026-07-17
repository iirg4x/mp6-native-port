/* MP6 native port -- REL "loader" bridge for bootDll/selmenuDll/fileseldll,
 * PLUS the DVD entry points every OTHER path (real game data: fonts,
 * message tables, HSF models, sprites, ...) goes through.
 *
 * On real hardware, game/objdll.c's omDLLLink() reads a .rel FILE from DVD
 * (HuDvdDataReadDirect -> DVDOpen/DVDReadAsyncPrio) into memory, then calls
 * OSLink() to runtime-relocate it against the already-running DOL and
 * patch its OSModuleHeader.prolog/epilog fields from section-relative
 * offsets into absolute addresses, before finally calling
 * ((OMDLLPROLOG)module->prolog)().
 *
 * This port links the decompiled overlays' C sources DIRECTLY into the
 * game image instead of building real .rel binaries -- there is nothing
 * left to relocate, the linker already resolved everything. So this file
 * fakes just enough of that dance for game/objdll.c (unmodified decomp
 * code) to keep working as-is:
 *
 *   - DVDOpen() recognizes the known "dll/xxx.rel" paths (see
 *     include/ovl_table.h) and hands back a tiny synthetic file whose
 *     "length" is sizeof(OSModuleHeader) -- nothing else, since OSLink
 *     will be a no-op relocation (no sections to walk).
 *   - DVDReadAsyncPrio()/DVDReadPrio(), when the target file is one of
 *     those, fabricate a zeroed OSModuleHeader tagged with a sentinel ID
 *     identifying which DLL it is, instead of reading real disc bytes.
 *   - OSLink() recognizes the sentinel ID and writes the REAL, statically-
 *     linked (renamed -- see tools/build.py's per-TU -D_prolog=.../
 *     -D_epilog=... flags, which dodge the multiple-definition clash of
 *     every DLL otherwise defining `_prolog`/`_epilog`) function pointers
 *     into the header's .prolog/.epilog fields, exactly mimicking what a
 *     real relocation would have produced -- then returns TRUE.
 *
 * Every OTHER path resolves for real, over the extracted disc tree
 * (GP6E01/{sys/fst.bin,files/}) -- see platform/dvd/dvd_files.c, which
 * owns the actual FST parsing/host-file I/O. This file checks the
 * synthetic paths FIRST, unconditionally, in every DVD entry point below;
 * only once a path/entrynum genuinely isn't one of those does it fall
 * through to mp6_dvd_*(). A path that's neither a synthetic DLL nor a real
 * FST entry still honestly returns "not found" -- game/dvd.c's
 * HuDvdDataReadWait callers hit their own OSPanic("File Open Error") when
 * that happens, which our hand-written OSPanic (platform/null/
 * shims_manual.c) prints clearly and exits on, rather than hanging or
 * crashing obscurely.
 */
#include "dolphin.h"
#include "game/process.h" /* HuPrcChildCreate/HuPrcCurrentGet/HuPrcVSleep, see mp6_minigame_stub_idle_child */
#include "mp6_shim_log.h"
#include "mp6_dvd_files.h"
#include "mp6_boot.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

typedef void (*VoidFunc)(void);

/* The decompiled DLLs' renamed prolog/epilog (see tools/build.py) and
 * empty ctors/dtors sentinel arrays (on real hardware these are linker-
 * synthesized per-module static-initializer tables; every DLL source file
 * in this decomp is plain C with nothing to construct, so an empty,
 * zero-terminated array reproduces that exactly -- `for (ctor = _ctors;
 * *ctor != 0; ctor++)` runs zero times). */
extern int bootDll_prolog(void);
extern void bootDll_epilog(void);
const VoidFunc bootDll_ctors[] = { 0 };
const VoidFunc bootDll_dtors[] = { 0 };

extern int selmenuDll_prolog(void);
extern void selmenuDll_epilog(void);
const VoidFunc selmenuDll_ctors[] = { 0 };
const VoidFunc selmenuDll_dtors[] = { 0 };

extern int fileselDll_prolog(void);
extern void fileselDll_epilog(void);
const VoidFunc fileselDll_ctors[] = { 0 };
const VoidFunc fileselDll_dtors[] = { 0 };

/* mdseldll -- mode select, the overlay file-select unconditionally
 * proceeds to (omOvlCallEx(0x5d, ...) = ovl_table.h index 93). Compiled
 * from the decomp's src/REL/mdseldll/mdsel.c and bound for real, exactly
 * like the other overlays; same -D rename scheme (tools/build.py
 * REL_SOURCES) for the shared _prolog/_epilog/_ctors/_dtors/ObjectSetup
 * names. */
extern int mdselDll_prolog(void);
extern void mdselDll_epilog(void);
const VoidFunc mdselDll_ctors[] = { 0 };
const VoidFunc mdselDll_dtors[] = { 0 };

/* mdpartydll -- party-mode setup, the overlay mode select's own "Party
 * Mode" confirm proceeds to (mdsel.c: omOvlCall(DLL_mdpartydll, 0, 0) =
 * ovl_table.h index 91). Compiled from the decomp's
 * src/REL/mdpartydll/{mdparty.c,stage.c} and bound for real, exactly like
 * the other overlays; same -D rename scheme (tools/build.py REL_SOURCES)
 * for the shared _prolog/_epilog/_ctors/_dtors/ObjectSetup names. */
extern int mdpartyDll_prolog(void);
extern void mdpartyDll_epilog(void);
const VoidFunc mdpartyDll_ctors[] = { 0 };
const VoidFunc mdpartyDll_dtors[] = { 0 };

enum {
    MP6_DLL_NONE = 0,
    MP6_DLL_BOOT,
    MP6_DLL_SELMENU,
    MP6_DLL_FILESEL,
    MP6_DLL_MDSEL,
    MP6_DLL_MDPARTY,
    MP6_DLL_MINIGAME_STUB,
};

/* Every not-yet-decompiled minigame DLL on the real disc. Matched by
 * FILENAME SHAPE ("m" + digits + "dll.rel", case-insensitive), not a
 * hardcoded list -- the real disc tree contains BOTH the contiguous
 * m601-m681/m699 range include/ovl_table.h's compiled-in table lists AND
 * 7 extra numeric IDs that exist as real disc files but are absent from
 * that table entirely (m433/m535/m541/m559/m562/m571/m580 -- presumably
 * loaded by some board's own direct filename construction rather than an
 * ovl_table.h lookup) -- a shape-based match handles either calling
 * convention identically, with no dependency on which one a given board
 * actually uses. Deliberately does NOT match "md*dll"/"mgm*dll"/"mic*dll"
 * (letters, not digits, right after the leading "m") -- those are
 * menu/quiz-select/etc. DLLs, a different family entirely, out of this
 * stub's scope (OSLink's catch-all below covers them instead). */
static BOOL is_minigame_stub_path(const char *path, char *nameOut, size_t nameOutSize)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t i;
    size_t digitEnd;
    size_t len = strlen(base);

    if (len < 2 || (base[0] != 'm' && base[0] != 'M')) {
        return FALSE;
    }
    i = 1;
    while (i < len && isdigit((unsigned char)base[i])) {
        i++;
    }
    digitEnd = i;
    if (digitEnd == 1) {
        return FALSE; /* no digits at all right after 'm' -- not this family */
    }
    /* Remainder must be "dll.rel" / "Dll.rel" / "DLL.rel" (case-insensitive). */
    if (len - digitEnd != 7) {
        return FALSE;
    }
    if (tolower((unsigned char)base[digitEnd + 0]) != 'd' || tolower((unsigned char)base[digitEnd + 1]) != 'l' ||
        tolower((unsigned char)base[digitEnd + 2]) != 'l' || base[digitEnd + 3] != '.' ||
        tolower((unsigned char)base[digitEnd + 4]) != 'r' || tolower((unsigned char)base[digitEnd + 5]) != 'e' ||
        tolower((unsigned char)base[digitEnd + 6]) != 'l') {
        return FALSE;
    }
    if (nameOut && nameOutSize > 0) {
        size_t copyLen = len < nameOutSize - 1 ? len : nameOutSize - 1;
        memcpy(nameOut, base, copyLen);
        nameOut[copyLen] = '\0';
    }
    return TRUE;
}

/* Set by synth_dll_for_path the instant a minigame-stub path is
 * recognized; read by mp6_minigame_stub_prolog (called later, with no
 * arguments of its own -- OMDLLPROLOG is BOOL(*)(void), matching every
 * other DLL's real prolog signature) so its banner can name the actual
 * requested file, not just "a minigame". Single-slot: this port loads at
 * most one DLL at a time in every reachable path so far (matching
 * MP6_DLL_BOOT/SELMENU/FILESEL's own one-at-a-time synthetic scheme). */
static char g_minigameStubName[64] = "(unknown)";

/* A trivial, permanently-idle child process, created by the stub prolog
 * below purely so `game/objmain.c`'s omWatchOverlayProc has something to
 * watch. Every REAL DLL's own ObjectSetup always creates at least one
 * real child process (e.g. fileseldll's `HuPrcChildCreate(FileselMain,
 * ...)`) before returning, and `game/process.c`'s HuPrcChildWatch is
 * written assuming that's always true: `if(curr->child) {
 * ...gclongjmp...(back to the scheduler) }` -- with NO child at all (this
 * stub's honest situation -- there is no real logic to run), that whole
 * body is skipped, so HuPrcChildWatch returns immediately without ever
 * yielding back to the top-level scheduler that calls VIWaitForRetrace.
 * omWatchOverlayProc's `while(1) { ... HuPrcChildWatch(); }` then spins
 * on that immediate return forever -- a genuine, silent 100%-CPU hang
 * (Aurora's event pump lives inside VIWaitForRetrace, so the window stops
 * pumping SDL events entirely and Windows reports it "Not Responding",
 * even though the process never crashes). A single always-sleeping child
 * fixes this exactly the way every real DLL's own child does --
 * `curr->child` is non-NULL, so HuPrcChildWatch's real gcsetjmp/gclongjmp
 * dance runs and control genuinely returns to the scheduler every tick,
 * same as any other loaded overlay. */
static void mp6_minigame_stub_idle_child(void)
{
    for (;;) {
        HuPrcVSleep();
    }
}

static BOOL mp6_minigame_stub_prolog(void)
{
    printf("[STUB] minigame '%s' not yet decompiled -- rendering a black screen placeholder "
           "instead of crashing or running garbage code.\n",
           g_minigameStubName);
    fflush(stdout);
    mp6_dll_stub_black_screen_active = 1;
    HuPrcChildCreate(mp6_minigame_stub_idle_child, 0x100, 0x1000, 0, HuPrcCurrentGet());
    return TRUE;
}

static void mp6_minigame_stub_epilog(void)
{
    /* Real minigame epilogs restore whatever the board screen's own state
     * was; this stub never changed anything BUT the black-screen flag, so
     * un-setting it is the entire teardown story. Not observed to be
     * reachable yet (nothing in this port's own boot flow requests a
     * minigame unload -- boards aren't decompiled), but safe and correct
     * by construction if it ever is. */
    printf("[STUB] minigame '%s' epilog -- clearing black-screen placeholder\n", g_minigameStubName);
    fflush(stdout);
    mp6_dll_stub_black_screen_active = 0;
}

/* Side table: DVDOpen tags a DVDFileInfo* with which synthetic DLL it is;
 * DVDReadAsyncPrio/DVDReadPrio/DVDClose consult it by pointer identity.
 * (DVDFileInfo has no spare field we can safely repurpose without risking
 * a real field game/dvd.c also reads, so a small side table is cleaner.) */
#define MP6_MAX_OPEN_SYNTH 8
static struct { DVDFileInfo *key; int dll; } g_openSynth[MP6_MAX_OPEN_SYNTH];

static int synth_dll_for_path(const char *path)
{
    if (strcmp(path, "dll/bootdll.rel") == 0) return MP6_DLL_BOOT;
    if (strcmp(path, "dll/selmenuDLL.rel") == 0) return MP6_DLL_SELMENU;
    if (strcmp(path, "dll/fileseldll.rel") == 0) return MP6_DLL_FILESEL;
    /* Exact path string per game/ovllist.c's own `DLL(name)` macro
     * expansion, `{ "dll/" #name ".rel", 0 }` of ovl_table.h's
     * DLL(mdseldll) entry -- all lowercase, unlike selmenuDLL above. */
    if (strcmp(path, "dll/mdseldll.rel") == 0) return MP6_DLL_MDSEL;
    /* Same DLL(name) macro expansion, ovl_table.h line 92 =
     * DLL(mdpartydll) = index 91 -- all lowercase again. */
    if (strcmp(path, "dll/mdpartydll.rel") == 0) return MP6_DLL_MDPARTY;
    if (is_minigame_stub_path(path, g_minigameStubName, sizeof(g_minigameStubName))) {
        return MP6_DLL_MINIGAME_STUB;
    }
    return MP6_DLL_NONE;
}

static void synth_tag(DVDFileInfo *f, int dll)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == NULL || g_openSynth[i].key == f) {
            g_openSynth[i].key = f;
            g_openSynth[i].dll = dll;
            return;
        }
    }
    fprintf(stderr, "[WARN] dll_bridge: synth-file table full, dropping tag for %p\n", (void *)f);
}

static int synth_lookup(DVDFileInfo *f)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == f) return g_openSynth[i].dll;
    }
    return MP6_DLL_NONE;
}

static void synth_untag(DVDFileInfo *f)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_SYNTH; i++) {
        if (g_openSynth[i].key == f) {
            g_openSynth[i].key = NULL;
            g_openSynth[i].dll = MP6_DLL_NONE;
            return;
        }
    }
}

BOOL DVDOpen(char *fileName, DVDFileInfo *fileInfo)
{
    MP6_LOG_ONCE("DVD", "DVDOpen");
    memset(fileInfo, 0, sizeof(*fileInfo));
    {
        int dll = synth_dll_for_path(fileName);
        if (dll != MP6_DLL_NONE) {
            fileInfo->length = sizeof(OSModuleHeader);
            synth_tag(fileInfo, dll);
            printf("[BOOT] DVDOpen: synthetic REL bridge for \"%s\"\n", fileName);
            return TRUE;
        }
    }
    /* Real disc file, served over the extracted GP6E01/files/ tree -- see
     * platform/dvd/dvd_files.c. Honestly returns FALSE ("not found") if
     * fileName isn't a real FST entry, or is one but the extracted file
     * is missing. */
    if (mp6_dvd_open_by_path(fileName, fileInfo)) {
        return TRUE;
    }
    printf("[BOOT] DVDOpen: \"%s\" not found (not a synthetic REL path, not a real FST entry)\n", fileName);
    return FALSE;
}

static void fill_fake_module_header(void *addr, s32 length, int dll)
{
    OSModuleHeader *hdr = (OSModuleHeader *)addr;
    if ((size_t)length < sizeof(OSModuleHeader)) {
        memset(addr, 0, (size_t)length);
        return;
    }
    memset(hdr, 0, sizeof(*hdr));
    hdr->info.id = (OSModuleID)(0xD11C0000u + (unsigned)dll);
    /* Everything else (numSections, bssSize, prolog/epilog offsets, ...)
     * stays 0 -- OSLink() below never walks sections, it just recognizes
     * info.id and writes the real function pointers directly. */
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    MP6_LOG_ONCE("DVD", "DVDReadAsyncPrio");
    (void)prio;
    {
        int dll = synth_lookup(fileInfo);
        if (dll != MP6_DLL_NONE) {
            (void)offset;
            fill_fake_module_header(addr, length, dll);
            if (callback) callback(length, fileInfo);
            return TRUE;
        }
    }
    /* Real file, completes synchronously (see platform/dvd/dvd_files.c's
     * header comment for why every reachable caller in this slice
     * tolerates that) -- the callback has already fired and `addr`
     * already holds the bytes by the time this returns. */
    if (mp6_dvd_read(fileInfo, addr, length, offset)) {
        if (callback) callback(length, fileInfo);
        return TRUE;
    }
    if (callback) callback(DVD_RESULT_FATAL_ERROR, fileInfo);
    return FALSE;
}

BOOL DVDReadPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, s32 prio)
{
    MP6_LOG_ONCE("DVD", "DVDReadPrio");
    (void)prio;
    {
        int dll = synth_lookup(fileInfo);
        if (dll != MP6_DLL_NONE) {
            (void)offset;
            fill_fake_module_header(addr, length, dll);
            return TRUE;
        }
    }
    return mp6_dvd_read(fileInfo, addr, length, offset);
}

BOOL DVDClose(DVDFileInfo *f)
{
    MP6_LOG_ONCE("DVD", "DVDClose");
    synth_untag(f);
    mp6_dvd_close(f); /* no-op if f isn't one of ITS open handles */
    return TRUE;
}

s32 DVDGetDriveStatus(void)
{
    MP6_LOG_ONCE("DVD", "DVDGetDriveStatus");
    /* A constant, never-changing "fine" status so game/dvd.c's
     * HuDvdErrorWatch() (which only acts on a CHANGE in status) never
     * fires any of its error branches. See this file's header comment. */
    return 0;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo)
{
    MP6_LOG_ONCE("DVD", "DVDFastOpen");
    memset(fileInfo, 0, sizeof(*fileInfo));
    /* Entrynum-only open (no path string) -- there's no synthetic-REL
     * path here at all (the fake DLLs are only ever opened by literal
     * path string, see DVDOpen above), so this is entirely real-FST
     * territory. entrynum comes from a prior real
     * DVDConvertPathToEntrynum() call the caller cached (e.g. game/data.c's
     * HuDataInit() populates DataDirStat[].entryNum this way). */
    return mp6_dvd_open_by_entrynum(entrynum, fileInfo);
}

s32 DVDCancel(volatile DVDCommandBlock *block)
{
    MP6_LOG_ONCE("DVD", "DVDCancel");
    (void)block;
    return 0;
}

BOOL DVDCancelAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    MP6_LOG_ONCE("DVD", "DVDCancelAsync");
    (void)block;
    (void)callback;
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *block)
{
    MP6_LOG_ONCE("DVD", "DVDGetCommandBlockStatus");
    (void)block;
    return 0;
}

s32 DVDConvertPathToEntrynum(char *pathPtr)
{
    /* game/data.c's HuDataInit() calls this once per entry in a compiled-in
     * table of every data/*.bin path the game knows about (248 of them),
     * purely to sanity-check "does this path exist on the disc" up front:
     * on any -1 it OSPanics immediately, before main()'s loop ever reaches
     * omMasterInit or the first HuPrcCall tick. Resolution is real, against
     * the disc's own FST (platform/dvd/dvd_files.c); every one of those 248
     * DATADIR() paths resolves against this disc's fst.bin, so HuDataInit's
     * panic-on-(-1) never fires on a complete extraction. A path that's
     * genuinely NOT on the disc still honestly returns -1, exactly like
     * real hardware would. */
    MP6_LOG_ONCE("DVD", "DVDConvertPathToEntrynum");
    return mp6_dvd_path_to_entrynum(pathPtr);
}

BOOL OSLink(OSModuleInfo *newModule, void *bss)
{
    MP6_LOG_ONCE("OS", "OSLink");
    (void)bss;
    {
        OSModuleHeader *hdr = (OSModuleHeader *)newModule; /* info is the 1st member */
        unsigned dll = (unsigned)(hdr->info.id - 0xD11C0000u);
        switch (dll) {
            case MP6_DLL_BOOT:
                hdr->prolog = (u32)(uintptr_t)&bootDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&bootDll_epilog;
                printf("[BOOT] OSLink: bound bootDll prolog/epilog\n");
                return TRUE;
            case MP6_DLL_SELMENU:
                hdr->prolog = (u32)(uintptr_t)&selmenuDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&selmenuDll_epilog;
                printf("[BOOT] OSLink: bound selmenuDll prolog/epilog\n");
                return TRUE;
            case MP6_DLL_FILESEL:
                hdr->prolog = (u32)(uintptr_t)&fileselDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&fileselDll_epilog;
                printf("[BOOT] OSLink: bound fileselDll prolog/epilog\n");
                return TRUE;
            case MP6_DLL_MDSEL:
                /* Real mode select. The default: catch-all below still
                 * covers everything not decompiled (the mdXXXdll mode
                 * overlays this menu itself calls next, in particular). */
                hdr->prolog = (u32)(uintptr_t)&mdselDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&mdselDll_epilog;
                printf("[BOOT] OSLink: bound mdselDll prolog/epilog\n");
                return TRUE;
            case MP6_DLL_MDPARTY:
                /* Real party-mode setup. The default: catch-all below
                 * still covers everything not decompiled (the board DLLs
                 * this setup flow itself calls next, in particular). */
                hdr->prolog = (u32)(uintptr_t)&mdpartyDll_prolog;
                hdr->epilog = (u32)(uintptr_t)&mdpartyDll_epilog;
                printf("[BOOT] OSLink: bound mdpartyDll prolog/epilog\n");
                return TRUE;
            case MP6_DLL_MINIGAME_STUB:
                hdr->prolog = (u32)(uintptr_t)&mp6_minigame_stub_prolog;
                hdr->epilog = (u32)(uintptr_t)&mp6_minigame_stub_epilog;
                printf("[BOOT] OSLink: bound minigame-stub prolog/epilog for '%s'\n", g_minigameStubName);
                return TRUE;
            default:
                /* Catch-all for ANY module id this switch doesn't
                 * recognize: bind the same black-screen stub
                 * prolog/epilog as the minigame family. Without this, an
                 * unrecognized overlay's header would carry real,
                 * un-relocated disc bytes straight through as its
                 * prolog/epilog pointers -- a guaranteed wild jump the
                 * instant objdll.c calls
                 * `((OMDLLPROLOG)dll->module->prolog)()` (a raw,
                 * GC-address-shaped value used as an x86_64 function
                 * pointer). A generic catch-all covers every
                 * not-yet-decompiled DLL uniformly regardless of filename
                 * shape, instead of extending is_minigame_stub_path's
                 * name-pattern allowlist one name at a time. The adjacent,
                 * unmodified decomp OSReport (`objdll>Link DLL:%s`)
                 * already names the file in the log immediately before
                 * this line, so nothing is lost by this branch itself only
                 * logging the raw id. */
                fprintf(stderr, "[WARN] OSLink: unrecognized module id 0x%08x -- treating as a "
                        "not-yet-decompiled module, same graceful stub as the minigame family\n",
                        (unsigned)hdr->info.id);
                hdr->prolog = (u32)(uintptr_t)&mp6_minigame_stub_prolog;
                hdr->epilog = (u32)(uintptr_t)&mp6_minigame_stub_epilog;
                snprintf(g_minigameStubName, sizeof(g_minigameStubName), "id 0x%08x", (unsigned)hdr->info.id);
                return TRUE;
        }
    }
}

BOOL OSUnlink(OSModuleInfo *oldModule)
{
    MP6_LOG_ONCE("OS", "OSUnlink");
    (void)oldModule;
    return TRUE;
}

/* The storage behind mp6_boot.h's extern declaration -- read every frame
 * by platform/gx/aurora_bridge.c's VIWaitForRetrace (Aurora build) to
 * force a black clear. Starts 0 (no effect) so every normal scenario
 * (opening/title/file-select) is completely unaffected; only a real (or
 * test-forced) minigame-stub prolog ever sets it. */
volatile int mp6_dll_stub_black_screen_active = 0;

/* Test-only forced-load hook -- see mp6_boot.h's own comment for the full
 * rationale. Accepts MP6_TEST_LOAD_DLL as a bare name ("m601Dll"), a name
 * with extension ("m601Dll.rel"), or a full path ("dll/m601Dll.rel") --
 * normalizes to the one shape synth_dll_for_path/DVDOpen actually expect
 * (a "dll/"-prefixed, ".rel"-suffixed path, matching every real caller in
 * this codebase, e.g. "dll/fileseldll.rel" above) so this exercises the
 * EXACT SAME code path a real (future, not-yet-decompiled) board's own
 * omDLLLink call would, not a parallel/simplified test-only shortcut. */
void mp6_dll_bridge_selftest_check_env(void)
{
    static int checked = 0;
    const char *raw;
    char path[96];
    size_t len;
    DVDFileInfo fileInfo;
    OSModuleHeader header;
    BOOL prologResult;

    if (checked) {
        return; /* only ever run once, on the first tick */
    }
    checked = 1;

    raw = getenv("MP6_TEST_LOAD_DLL");
    if (!raw || !raw[0]) {
        return; /* no-op by default -- every normal run is unaffected */
    }

    len = strlen(raw);
    if (len >= 4 && strncmp(raw, "dll/", 4) == 0) {
        snprintf(path, sizeof(path), "%s", raw);
    } else if (len >= 4 && strcmp(raw + len - 4, ".rel") == 0) {
        snprintf(path, sizeof(path), "dll/%s", raw);
    } else {
        snprintf(path, sizeof(path), "dll/%s.rel", raw);
    }

    printf("[DLLSTUB-TEST] MP6_TEST_LOAD_DLL='%s' -> normalized path '%s' -- driving a real "
           "DVDOpen/DVDReadAsyncPrio/OSLink/prolog() round trip through this file's own real "
           "entry points (the exact same ones a real omDLLLink call would use).\n",
           raw, path);
    fflush(stdout);

    if (!DVDOpen(path, &fileInfo)) {
        printf("[DLLSTUB-TEST] FAILED: DVDOpen did not recognize '%s' as a minigame-stub "
               "path -- check is_minigame_stub_path's naming rule.\n", path);
        fflush(stdout);
        return;
    }
    if (!DVDReadAsyncPrio(&fileInfo, &header, (s32)sizeof(header), 0, NULL, 0)) {
        printf("[DLLSTUB-TEST] FAILED: DVDReadAsyncPrio did not produce a synthetic module "
               "header for '%s'.\n", path);
        fflush(stdout);
        DVDClose(&fileInfo);
        return;
    }
    if (!OSLink(&header.info, NULL)) {
        printf("[DLLSTUB-TEST] FAILED: OSLink did not bind stub prolog/epilog for '%s' "
               "(module id 0x%08x).\n", path, (unsigned)header.info.id);
        fflush(stdout);
        DVDClose(&fileInfo);
        return;
    }
    prologResult = ((BOOL(*)(void))(uintptr_t)header.prolog)();
    printf("[DLLSTUB-TEST] prolog() returned %d, mp6_dll_stub_black_screen_active=%d\n",
           (int)prologResult, mp6_dll_stub_black_screen_active);
    fflush(stdout);
    if (mp6_dll_stub_black_screen_active) {
        printf("[DLLSTUB-TEST] PASS: stub path confirmed end to end for '%s' -- black-screen "
               "flag is active, process is idling normally (no crash), see the [STUB] banner "
               "line above for the human-readable confirmation.\n", path);
    } else {
        printf("[DLLSTUB-TEST] FAIL: prolog ran but did not set the black-screen flag.\n");
    }
    fflush(stdout);
    if (!getenv("MP6_TEST_LOAD_DLL_HOLD")) {
        ((void(*)(void))(uintptr_t)header.epilog)();
        DVDClose(&fileInfo);
    }
}
