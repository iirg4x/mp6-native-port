/* MP6 native port -- real DVD file serving over the extracted GameCube disc
 * tree (GP6E01/{sys/fst.bin,files/}).
 *
 *   - The real disc's File String Table (sys/fst.bin -- the exact bytes
 *     __DVDFSInit() would have pointed BootInfo->FSTLocation at on real
 *     hardware) is loaded once, lazily, and kept as raw big-endian bytes
 *     -- never byte-swapped in place, read field-by-field via be.h's
 *     be32() at each entry, the same discipline as every other BE fix in
 *     patches/decomp/ -- then walked with the SAME directory-hierarchy
 *     algorithm real hardware's DVDConvertPathToEntrynum used (reference,
 *     read-only, NOT compiled into this port: the decomp repo's
 *     src/dolphin/dvd/dvdfs.c).
 *   - Every path in include/datadir_table.h resolves against this disc's
 *     fst.bin, so HuDataInit()'s up-front DVDConvertPathToEntrynum sanity
 *     check (which OSPanics on the first -1) passes on a complete
 *     extraction.
 *   - Once an entrynum/path is known, the actual bytes are read directly
 *     from the corresponding REAL file under GP6E01/files/ via plain
 *     host fopen/fseek/fread -- there is no raw ISO image to seek into
 *     here, just the already-extracted individual files, so this shim's
 *     own DVDFileInfo.startAddr is always 0 and every read is relative to
 *     that host file's own start, NOT a real absolute GC-disc byte
 *     address. Nothing outside this file and dll_bridge.c ever interprets
 *     startAddr as a real disc offset, so that's safe.
 *   - dll_bridge.c's synthetic "dll/*.rel" paths are checked FIRST,
 *     unconditionally, before any of this -- the statically-linked
 *     overlays keep loading exactly as before; this module only ever
 *     serves paths dll_bridge.c doesn't already recognize.
 *
 * Async reads complete SYNCHRONOUSLY, matching dll_bridge.c's
 * synthetic-path convention: by the time DVDReadAsyncPrio returns, the
 * callback has already fired and the bytes are already in the destination
 * buffer. Safe for every call site this slice reaches (game/dvd.c and
 * game/data.c's own async paths only ever poll a flag the callback itself
 * sets, or chain the next block's read from inside the callback -- both
 * unwind correctly, just faster than real hardware, if the whole transfer
 * is already done by the time the "kick it off" call returns).
 *
 * The bytes this file hands back are raw, unmodified, big-endian disc
 * bytes -- exactly what DVDRead always returned on real hardware.
 * Byte-swapping is each FORMAT's own concern (game/data.c's GetFileInfo,
 * game/decode.c's HuDecodeZlib, game/sprman.c's HuSprAnimRead, ... -- the
 * patches/decomp/ patch queue), never this DVD layer's. See
 * docs/ARCHITECTURE.md, "Data endianness".
 */
#include "dolphin.h"
#include "mp6_shim_log.h"
#include "mp6_dvd_files.h"
#include "be.h"
#include "host.h" /* mp6_host_disc_root -- see mp6_resolve_dvd_paths() below */
#include "mp6_savestate.h" /* W2: mp6_dvd_savestate_rehydrate() / open-handle count */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). Placing this AFTER this TU's own
 * includes is load-bearing, not stylistic: it is a #pragma clang section that
 * redirects every file-scope definition FOLLOWING it. As a -include (before all
 * headers) it also captured the decomp headers' C TENTATIVE definitions --
 * dolphin/os.h:27 `u32 __OSBusClock;` and friends -- turning those common
 * symbols into strong per-TU definitions and breaking the link with duplicate
 * symbol errors. Here, headers keep their normal linkage and only this file's
 * own statics move. tools/build.py asserts this line exists in every TU listed
 * in HOST_STATE_SECTION_SOURCES. */
#include "mp6_host_section.h"


#ifndef MP6_DVD_FILES_ROOT
#error "MP6_DVD_FILES_ROOT must be defined by tools/build.py (absolute path to orig/GP6E01/files)"
#endif
#ifndef MP6_DVD_FST_PATH
#error "MP6_DVD_FST_PATH must be defined by tools/build.py (absolute path to orig/GP6E01/sys/fst.bin)"
#endif

/* =======================================================================
 * Resolve the real disc tree relative to the RUNNING EXE'S OWN location,
 * not the process's current working directory and not solely the
 * build-time MP6_DVD_FILES_ROOT/MP6_DVD_FST_PATH -D's above. Those two
 * are absolute paths baked in at BUILD time (whatever the disc tree
 * resolved to on the machine that ran tools/build.py) -- correct as long
 * as the checkout never moves, but a plain double-click (any cwd) plus
 * any future move/rename of the enclosing workspace tree are both real
 * ways for that baked-in path to stop matching reality even though the
 * real files are still sitting right there, a few directories up from
 * the exe. The workspace layout is a FIXED relative offset from the
 * exe's own directory, so the host seam's exe-relative walk is robust to
 * both, whereas cwd is robust to neither. Falls back to the build-time
 * -D paths if that computed path doesn't exist on disk, so an
 * unusual/flattened deployment layout degrades to a still-working
 * behavior instead of a hard failure.
 * ======================================================================= */

static char g_resolvedFilesRoot[1024];
static char g_resolvedFstPath[1024];
static int g_pathsResolved;

static void mp6_resolve_dvd_paths(void)
{
    char discRoot[1024];

    if (g_pathsResolved) return;
    g_pathsResolved = 1;

    /* Always-valid fallback first -- every return path below either
     * overwrites this with a verified-existing runtime-relative path or
     * leaves it exactly as-is. */
    snprintf(g_resolvedFilesRoot, sizeof(g_resolvedFilesRoot), "%s", MP6_DVD_FILES_ROOT);
    snprintf(g_resolvedFstPath, sizeof(g_resolvedFstPath), "%s", MP6_DVD_FST_PATH);

    /* The exe-relative walk lives in mp6_host_disc_root (platform/host/),
     * WITH the fst.bin openability probe -- it returns 0 only for a
     * verified-existing disc root, so appending "/files" and
     * "/sys/fst.bin" here always yields usable paths. */
    if (mp6_host_disc_root(discRoot, sizeof(discRoot)) == 0) {
        snprintf(g_resolvedFilesRoot, sizeof(g_resolvedFilesRoot), "%s/files", discRoot);
        snprintf(g_resolvedFstPath, sizeof(g_resolvedFstPath), "%s/sys/fst.bin", discRoot);
        printf("[DVD] resolved the real disc tree relative to the running exe (not cwd): \"%s\"\n",
               g_resolvedFilesRoot);
        fflush(stdout);
    }
    /* else: no verified exe-relative tree on this layout -- silently keep
     * the build-time fallback already populated above; fst_load_once()'s
     * own "couldn't open FST" WARN still fires normally if that path is
     * also wrong. */
}

/* =======================================================================
 * Launcher-configured content root.
 *
 * Two tiny additive hooks for the windowed build's pre-boot launcher menu
 * (platform/gx/ui/launcher_core.cpp). Both are dead code in every other
 * flow -- the headless build never links the launcher, and the windowed
 * automation invocations (tick budget / --input-script /
 * MP6_AUTO_START_TICKS / MP6_LAUNCHER=0) never call them -- so the
 * resolve-order behavior above is untouched unless a user explicitly
 * configured a content root in the menu.
 * ======================================================================= */

/* Launcher menu probe: run (or reuse) the normal resolution above and
 * report which files-root it produced and whether its FST actually opens.
 * Returns 1 if the FST is openable, 0 if not. */
int mp6_dvd_probe_root(char *filesRootOut, size_t n)
{
    FILE *f;
    mp6_resolve_dvd_paths();
    if (filesRootOut != NULL && n > 0) {
        snprintf(filesRootOut, n, "%s", g_resolvedFilesRoot);
    }
    f = fopen(g_resolvedFstPath, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* Launcher menu override: replace both resolved paths outright (the
 * launcher validated fst.bin/files existence first). Must be called
 * before the first file open (the launcher calls it pre-GameMain); also
 * marks resolution done so a later mp6_resolve_dvd_paths() can't
 * overwrite it. */
void mp6_dvd_set_root_override(const char *filesRoot, const char *fstPath)
{
    g_pathsResolved = 1;
    snprintf(g_resolvedFilesRoot, sizeof(g_resolvedFilesRoot), "%s", filesRoot);
    snprintf(g_resolvedFstPath, sizeof(g_resolvedFstPath), "%s", fstPath);
    printf("[DVD] launcher-configured content root: \"%s\"\n", g_resolvedFilesRoot);
    fflush(stdout);
}

/* =======================================================================
 * FST (File String Table) -- loaded once, lazily, kept raw/big-endian.
 * Layout (12 bytes/entry, see external_refs/.../src/dolphin/dvd/dvdfs.c):
 *   [0..3]  isDirAndStringOff  (top byte: 0=file/nonzero=dir; low 24 bits:
 *                               byte offset of this entry's name in the
 *                               string table that starts right after the
 *                               last entry)
 *   [4..7]  parentOrPosition   (dir: parent entrynum. file: disc byte
 *                               position -- unused here, see header note
 *                               on startAddr)
 *   [8..11] nextEntryOrLength  (dir: entrynum just past this dir's whole
 *                               subtree. file: byte length. Entry 0 (the
 *                               root dir)'s own value here doubles as
 *                               MaxEntryNum, the total entry count.)
 * ======================================================================= */

static u8 *g_fstData;
static u32 g_fstMaxEntry;
static const char *g_fstStrings; /* = (char*)g_fstData + g_fstMaxEntry*12 */
static int g_fstLoadAttempted;
static int g_fstOk;
static char **g_entryPaths; /* g_entryPaths[i] = malloc'd disc-relative path for FILE entry i; NULL for dirs/root/unset */

#define FST_ENTRY_OFF(i) ((u32)(i) * 12u)

static u32 fst_raw_u32(u32 byteOff)
{
    return be32(g_fstData + byteOff);
}

static BOOL fst_is_dir(u32 i)
{
    return (fst_raw_u32(FST_ENTRY_OFF(i)) & 0xFF000000u) != 0;
}

static u32 fst_string_off(u32 i)
{
    return fst_raw_u32(FST_ENTRY_OFF(i)) & 0x00FFFFFFu;
}

static u32 fst_field1(u32 i) /* parentOrPosition */
{
    return fst_raw_u32(FST_ENTRY_OFF(i) + 4);
}

static u32 fst_field2(u32 i) /* nextEntryOrLength */
{
    return fst_raw_u32(FST_ENTRY_OFF(i) + 8);
}

static char *fst_dup(const char *s, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Builds g_entryPaths[] with ONE full depth-first walk of the real FST,
 * mirroring its own on-disc layout invariant (a directory's children are
 * laid out contiguously, depth-first, immediately after it; field2/
 * nextEntryOrLength marks one-past-the-end of that subtree).
 *
 * This exists because FILE entries carry NO parent pointer in this format
 * at all -- a file's own field1/parentOrPosition slot is repurposed to
 * hold a real disc byte position instead (see this file's top comment),
 * not a parent entrynum. The real SDK's own entryToPath()/
 * DVDConvertEntrynumToPath() (external_refs/.../dvdfs.c) gets away with a
 * naive parent-chain walk only because its one real call site
 * (DVDGetCurrentDir) ever passes a DIRECTORY entrynum, whose field1
 * genuinely IS its parent; DVDFastOpen's entrynum is routinely a FILE
 * entry, so that approach doesn't generalize here. A single top-down walk
 * building a full reverse-lookup table sidesteps the question entirely
 * (and costs a few KB for this disc's 945 files). */
static void fst_walk_build(u32 dirIdx, const char *prefix, size_t prefixLen)
{
    u32 i = dirIdx + 1;
    u32 end = fst_field2(dirIdx);
    while (i < end) {
        const char *name = g_fstStrings + fst_string_off(i);
        size_t nameLen = strlen(name);
        char full[1024];
        size_t fullLen;
        BOOL isDir = fst_is_dir(i);

        if (prefixLen > 0 && prefixLen + 1 + nameLen < sizeof(full)) {
            memcpy(full, prefix, prefixLen);
            full[prefixLen] = '/';
            memcpy(full + prefixLen + 1, name, nameLen + 1);
            fullLen = prefixLen + 1 + nameLen;
        } else if (prefixLen == 0 && nameLen < sizeof(full)) {
            memcpy(full, name, nameLen + 1);
            fullLen = nameLen;
        } else {
            /* Absurdly long path for this disc; skip rather than overflow. */
            i = isDir ? fst_field2(i) : (i + 1);
            continue;
        }

        if (isDir) {
            fst_walk_build(i, full, fullLen);
            i = fst_field2(i);
        } else {
            g_entryPaths[i] = fst_dup(full, fullLen);
            i++;
        }
    }
}

static void fst_load_once(void)
{
    FILE *f;
    long size;

    if (g_fstLoadAttempted) return;
    g_fstLoadAttempted = 1;

    mp6_resolve_dvd_paths();
    f = fopen(g_resolvedFstPath, "rb");
    if (!f) {
        fprintf(stderr,
                "[WARN] dvd_files: couldn't open FST at \"%s\" -- every real-file DVDOpen/"
                "DVDConvertPathToEntrynum will honestly report \"not found\"\n",
                g_resolvedFstPath);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 12) {
        fprintf(stderr, "[WARN] dvd_files: FST at \"%s\" is implausibly small (%ld bytes)\n",
                g_resolvedFstPath, size);
        fclose(f);
        return;
    }

    g_fstData = (u8 *)malloc((size_t)size);
    if (!g_fstData || fread(g_fstData, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "[WARN] dvd_files: failed to read FST at \"%s\"\n", g_resolvedFstPath);
        free(g_fstData);
        g_fstData = NULL;
        fclose(f);
        return;
    }
    fclose(f);

    g_fstMaxEntry = fst_field2(0); /* root dir's own nextEntryOrLength == total entry count */
    g_fstStrings = (const char *)(g_fstData + FST_ENTRY_OFF(g_fstMaxEntry));
    g_fstOk = 1;

    g_entryPaths = (char **)calloc(g_fstMaxEntry, sizeof(char *));
    if (g_entryPaths) {
        fst_walk_build(0, "", 0);
    }
    printf("[DVD] fst.bin loaded: %u entries from \"%s\"\n", g_fstMaxEntry, g_resolvedFstPath);
}

/* Case-insensitive, length-bounded match against a NUL-terminated FST
 * string-table name (mirrors dvdfs.c's isSame(), minus its trailing
 * '/'-or-'\0' check -- our caller already knows segLen precisely instead
 * of needing to walk both strings in lockstep to discover it). */
static BOOL fst_name_matches(const char *seg, u32 segLen, const char *name)
{
    u32 i;
    for (i = 0; i < segLen; i++) {
        if (name[i] == '\0') return FALSE;
        if (tolower((unsigned char)seg[i]) != tolower((unsigned char)name[i])) return FALSE;
    }
    return name[segLen] == '\0';
}

/* Real disc's DVDConvertPathToEntrynum (external_refs/.../dvdfs.c) walks
 * from a persistent `currentDirectory`, reset only by DVDChangeDir -- which
 * has zero call sites anywhere in this decomp's compiled src/game or
 * src/REL (grep-confirmed), so this reimplementation always starts at
 * root; there's no chdir state that could ever legitimately differ.
 *
 * Deliberately drops dvdfs.c's 8.3-filename-legality OSPanic: several real
 * DATADIR() names (e.g. "capsulechar0.bin", "minikoopaBmdl0.bin") are
 * longer than 8.3, and this disc's own FST resolves them fine regardless
 * -- enforcing that restriction here would reject real, valid files the
 * retail SDK build this shipped with evidently tolerated (whatever
 * __DVDLongFileNameFlag ended up being by the time HuDataInit() ran on
 * real hardware; that's a real-hardware SDK-revision detail, not something
 * this shim needs to reproduce to serve real files correctly). */
s32 mp6_dvd_path_to_entrynum(const char *path)
{
    u32 dirLookAt = 0;
    const char *p = path;

    fst_load_once();
    if (!g_fstOk) return -1;

    for (;;) {
        if (*p == '\0') return (s32)dirLookAt;
        if (*p == '/') { dirLookAt = 0; p++; continue; }
        if (p[0] == '.' && p[1] == '.' && p[2] == '/') { dirLookAt = fst_field1(dirLookAt); p += 3; continue; }
        if (p[0] == '.' && p[1] == '.' && p[2] == '\0') { return (s32)fst_field1(dirLookAt); }
        if (p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (p[0] == '.' && p[1] == '\0') { return (s32)dirLookAt; }

        {
            const char *segStart = p;
            u32 segLen = 0;
            u32 i, end, found;
            BOOL wantDir;
            while (segStart[segLen] != '\0' && segStart[segLen] != '/') segLen++;
            wantDir = (segStart[segLen] == '/');

            found = 0xFFFFFFFFu;
            end = fst_field2(dirLookAt);
            for (i = dirLookAt + 1; i < end; ) {
                BOOL isDir = fst_is_dir(i);
                if (!(isDir == FALSE && wantDir == TRUE)) {
                    const char *name = g_fstStrings + fst_string_off(i);
                    if (fst_name_matches(segStart, segLen, name)) { found = i; break; }
                }
                i = isDir ? fst_field2(i) : (i + 1);
            }
            if (found == 0xFFFFFFFFu) return -1;
            if (!wantDir) return (s32)found;
            dirLookAt = found;
            p = segStart + segLen + 1;
        }
    }
}

/* =======================================================================
 * Host file access: an open-file side table (same idiom as dll_bridge.c's
 * own synthetic-REL table) keyed by DVDFileInfo* identity, backed by a
 * plain fopen/fseek/fread against the real extracted file.
 * ======================================================================= */

#define MP6_MAX_OPEN_REAL 32
static struct {
    DVDFileInfo *key;
    FILE *fp;
    u32 length;
} g_openReal[MP6_MAX_OPEN_REAL];

static BOOL real_tag(DVDFileInfo *f, FILE *fp, u32 length)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key == NULL) {
            g_openReal[i].key = f;
            g_openReal[i].fp = fp;
            g_openReal[i].length = length;
            return TRUE;
        }
    }
    fprintf(stderr, "[WARN] dvd_files: real-file table full (%d open), can't track %p\n",
            MP6_MAX_OPEN_REAL, (void *)f);
    return FALSE;
}

static int real_lookup(DVDFileInfo *f)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key == f) return i;
    }
    return -1;
}

static BOOL mp6_dvd_open_relpath(const char *relpath, s32 entrynum, DVDFileInfo *fileInfo)
{
    char hostPath[1024];
    FILE *fp;
    long size;

    mp6_resolve_dvd_paths();
    if ((size_t)snprintf(hostPath, sizeof(hostPath), "%s/%s", g_resolvedFilesRoot, relpath) >= sizeof(hostPath)) {
        fprintf(stderr, "[WARN] dvd_files: host path too long for \"%s\"\n", relpath);
        return FALSE;
    }
    fp = fopen(hostPath, "rb");
    if (!fp) {
        /* The FST says this path/entrynum exists, but the real bytes aren't
         * on this checkout -- report an honest, visible "not found" one
         * layer deeper than a bad path, rather than treating a missing
         * file as fatal (some paths legitimately don't exist on an
         * incomplete disc extraction). */
        printf("[DVD] \"%s\" (entrynum %d) is a real FST entry but the file is missing on this "
               "checkout (looked for \"%s\")\n", relpath, entrynum, hostPath);
        return FALSE;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); return FALSE; }

    memset(fileInfo, 0, sizeof(*fileInfo));
    if (!real_tag(fileInfo, fp, (u32)size)) {
        fclose(fp);
        return FALSE;
    }
    fileInfo->startAddr = 0; /* see file header note: no raw ISO, reads are host-file-relative */
    fileInfo->length = (u32)size;
    return TRUE;
}

BOOL mp6_dvd_open_by_path(const char *path, DVDFileInfo *fileInfo)
{
    s32 entry = mp6_dvd_path_to_entrynum(path);

    if (entry < 0 || fst_is_dir((u32)entry)) return FALSE;
    /* The caller already handed us the exact path string -- no need to
     * reconstruct one from the FST (see mp6_dvd_open_by_entrynum for why
     * that reconstruction exists at all, and isn't just reused here). A
     * leading '/' (root-relative) is the only normalization real call
     * sites could plausibly need; none observed in practice (every real
     * DATADIR()/dll path is already bare, e.g. "data/title.bin"). */
    if (*path == '/') path++;
    return mp6_dvd_open_relpath(path, entry, fileInfo);
}

BOOL mp6_dvd_open_by_entrynum(s32 entrynum, DVDFileInfo *fileInfo)
{
    const char *relpath;

    fst_load_once();
    if (!g_fstOk || entrynum < 0 || (u32)entrynum >= g_fstMaxEntry || fst_is_dir((u32)entrynum)) {
        return FALSE;
    }
    relpath = g_entryPaths ? g_entryPaths[entrynum] : NULL;
    if (!relpath) return FALSE; /* shouldn't happen for a valid non-dir entrynum; defensive */
    return mp6_dvd_open_relpath(relpath, entrynum, fileInfo);
}

BOOL mp6_dvd_read(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset)
{
    int idx = real_lookup(fileInfo);
    FILE *fp;
    long avail;
    size_t toRead;

    if (idx < 0) {
        fprintf(stderr, "[WARN] dvd_files: read on an untracked DVDFileInfo* %p\n", (void *)fileInfo);
        return FALSE;
    }
    if (length < 0 || offset < 0 || (u32)offset > g_openReal[idx].length) return FALSE;

    fp = g_openReal[idx].fp;
    avail = (long)g_openReal[idx].length - offset;
    toRead = (size_t)((avail < (long)length) ? avail : length);

    if (fseek(fp, offset, SEEK_SET) != 0) return FALSE;
    if (toRead > 0 && fread(addr, 1, toRead, fp) != toRead) {
        fprintf(stderr, "[WARN] dvd_files: short read (wanted %zu bytes at offset %d)\n", toRead, offset);
        return FALSE;
    }
    if (toRead < (size_t)length) {
        /* Real GC reads are block/sector-padded (DVD_MIN_TRANSFER_SIZE);
         * this port's individually-extracted files carry no such trailing
         * padding. Zero it rather than leave whatever was already in the
         * caller's (HuMemDirectMalloc'd) destination buffer. */
        memset((u8 *)addr + toRead, 0, (size_t)length - toRead);
    }
    return TRUE;
}

void mp6_dvd_close(DVDFileInfo *fileInfo)
{
    int idx = real_lookup(fileInfo);
    if (idx < 0) return; /* not ours -- e.g. dll_bridge.c's own synthetic-REL handle */
    fclose(g_openReal[idx].fp);
    g_openReal[idx].key = NULL;
    g_openReal[idx].fp = NULL;
    g_openReal[idx].length = 0;
}

/* =======================================================================
 * Savestate support (docs/SAVESTATE.md, W2)
 * =======================================================================
 * This whole TU's statics are carved out of the savestate (see
 * HOST_STATE_SECTION_SOURCES in tools/build.py), because every one of them
 * is host-owned: the FST blob and its string pool are CRT-heap pointers,
 * g_entryPaths is a heap array of heap strings, and the resolved paths
 * belong to the machine that is RUNNING, not the one that captured.
 *
 * The single thing the carve-out gives up is the identity of files that
 * were open at capture time -- g_openReal[] records a DVDFileInfo* key and
 * a FILE*, but no path or entrynum, so an open handle could not be
 * re-established even in principle. That is acceptable ONLY because no
 * compiled caller in this build holds a DVD file open across a frame
 * boundary: every DVDOpen in game/dvd.c closes within the same call,
 * msm_bridge.c reads through a stack-local DVDFileInfo, msmstream.c is not
 * compiled, and THP -- the one caller that would hold a handle across
 * HuPrcVSleep -- is gated off at THPInit (platform/null/shims_manual.c).
 *
 * That is an assumption about the rest of the tree, so it is CHECKED rather
 * than trusted: mp6_dvd_open_handle_count() lets the capture path assert it
 * and warn loudly if it ever stops holding (enabling THP or compiling
 * msmstream.c would do it). When that day comes the fix is to split this
 * table -- {key, entrynum} in restored memory, {FILE*} in a parallel
 * carved-out array -- and re-fopen each slot from the rehydrate hook; the
 * entrynum is a restore-stable identity because the FST is deterministically
 * rebuilt from the resolved path. */
int mp6_dvd_open_handle_count(void)
{
    int i, n = 0;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].fp != NULL) {
            n++;
        }
    }
    return n;
}

/* Post-restore hook. The restored game state refers to no file this process
 * has open, so any handle held here is now orphaned -- close it rather than
 * leak it. Everything else this TU owns (FST blob, path cache, load latch)
 * is carved out and therefore already correct for THIS process; that is the
 * whole point of the carve-out, so there is deliberately nothing to rebuild
 * here. Safe before any DVD path has been resolved: the table is zeroed. */
void mp6_dvd_savestate_rehydrate(void)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].fp != NULL) {
            fclose(g_openReal[i].fp);
            g_openReal[i].fp = NULL;
        }
        g_openReal[i].key = NULL;
        g_openReal[i].length = 0;
    }
}
