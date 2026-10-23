/* MP6 native port -- real DVD file serving over the extracted GameCube disc
 * tree (GP6E01/{sys/fst.bin,files/}).
 *
 *   - The real disc's File String Table (sys/fst.bin -- the exact bytes
 *     __DVDFSInit() would have pointed BootInfo->FSTLocation at on real
 *     hardware) is loaded once, lazily, and kept as raw big-endian bytes
 *     -- never byte-swapped in place, read field-by-field via be.h's
 *     be32() at each entry, the same discipline as every other BE fix in
 *     compat/decomp/ -- then walked with the SAME directory-hierarchy
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
 * compat/decomp/ patch queue), never this DVD layer's. See
 * docs/ARCHITECTURE.md, "Data endianness".
 */
#include "dolphin.h"
#include "mp6_shim_log.h"
#include "mp6_dvd_files.h"
#include "mp6_path.h"
#include "mp6_utf8_file.h"
#include "../content/content_fst_validate.h"
#include "be.h"
#include "host.h" /* mp6_host_disc_root -- see mp6_resolve_dvd_paths() below */
#include "mp6_savestate.h" /* W2: mp6_dvd_savestate_rehydrate() / open-handle count */
#include "mp6_load_trace.h"
#include "mp6_decode_ahead.h"
#include "mp6_boot.h" /* mp6_tick_count: scene prefetch recording window */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* SAVESTATE CARVE-OUT. Placing this AFTER this TU's own
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
#ifndef MP6_DVD_ROOT_EXPLICIT
#define MP6_DVD_ROOT_EXPLICIT 0
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

#define MP6_DVD_PATH_CAP 1200

/* Open host handles, one per FST file entry (see "Host file access"). */
static int g_scenePrefetch;
static void scene_record_open(s32 entry);
#define MP6_DISC_HANDLE_CAP 64

typedef struct {
    Mp6HostFile *file; /* NULL until first opened, or after eviction */
    u32 lastUse;       /* g_discUseClock stamp, for eviction */
    u32 users;         /* game handles (g_openReal entries) on it */
} DiscFile;

static DiscFile *g_discFiles; /* [g_fstMaxEntry], allocated with the FST */
static int g_discHandleCount;
static u32 g_discUseClock;

static void disc_close_idle(void);

static char g_resolvedFilesRoot[MP6_DVD_PATH_CAP];
static char g_resolvedFstPath[MP6_DVD_PATH_CAP];
static int g_pathsResolved;
static int g_rootOverridden; /* the launcher's configured root wins over auto-detection */

/* Publish the path pair atomically: an oversized member must never leave one
 * new target paired with one old (or truncated) target. */
static int mp6_set_resolved_paths(const char *filesRoot, const char *fstPath)
{
    char checkedFiles[sizeof(g_resolvedFilesRoot)] = {0};
    char checkedFst[sizeof(g_resolvedFstPath)] = {0};
    if (!mp6_utf8_path_supported(filesRoot) || !mp6_utf8_path_supported(fstPath) ||
        mp6_path_copy_checked(checkedFiles, sizeof(checkedFiles), filesRoot) != 0 ||
        mp6_path_copy_checked(checkedFst, sizeof(checkedFst), fstPath) != 0) {
        return -1;
    }
    memcpy(g_resolvedFilesRoot, checkedFiles, sizeof(checkedFiles));
    memcpy(g_resolvedFstPath, checkedFst, sizeof(checkedFst));
    return 0;
}

static int mp6_path_file_nonempty(const char *path)
{
    FILE *f = mp6_fopen_utf8(path, "rb");
    long size = -1;
    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    fclose(f);
    return size > 0;
}

static int mp6_runtime_wanted_file(const char *rel)
{
    if (strcmp(rel, "opening.bnr") == 0 || strcmp(rel, "sound/MP6_SND.msm") == 0 ||
        strcmp(rel, "sound/MP6_Str.pdt") == 0) return 1;
    return (strncmp(rel, "data", 4) == 0 && (rel[4] == '/' || rel[4] == '\0')) ||
           (strncmp(rel, "mess", 4) == 0 && (rel[4] == '/' || rel[4] == '\0')) ||
           (strncmp(rel, "mic", 3) == 0 && (rel[3] == '/' || rel[3] == '\0'));
}

static int mp6_path_file_exact_size(const char *path, uint32_t expected)
{
    FILE *f = mp6_fopen_utf8(path, "rb");
    long size;
    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    fclose(f);
    return size >= 0 && (uint64_t)size == (uint64_t)expected;
}

static int mp6_validate_fst_files_walk(const unsigned char *fst, uint32_t dir,
                                       const char *prefix, const char *filesRoot,
                                       unsigned int *found, uint64_t *wantedBytes,
                                       char *err, size_t errn)
{
    uint32_t count = mp6_content_fst_be32(fst + 8);
    const char *strings = (const char *)(fst + (size_t)count * 12u);
    uint32_t i = dir + 1;
    uint32_t end = mp6_content_fst_be32(fst + (size_t)dir * 12u + 8);
    while (i < end) {
        const unsigned char *entry = fst + (size_t)i * 12u;
        uint32_t word0 = mp6_content_fst_be32(entry);
        const char *name = strings + (word0 & 0x00FFFFFFu);
        int isDir = (word0 & 0xFF000000u) != 0;
        char rel[1024];
        char path[1200];
        int n = prefix[0] ? snprintf(rel, sizeof(rel), "%s/%s", prefix, name)
                          : snprintf(rel, sizeof(rel), "%s", name);
        if (n < 0 || n >= (int)sizeof(rel)) {
            if (err && errn) snprintf(err, errn, "FST path is too long at entry %u", i);
            return 0;
        }
        if (isDir) {
            if (!mp6_validate_fst_files_walk(fst, i, rel, filesRoot, found,
                                             wantedBytes, err, errn)) return 0;
            i = mp6_content_fst_be32(entry + 8);
        } else {
            if (mp6_runtime_wanted_file(rel)) {
                uint32_t fileSize = mp6_content_fst_be32(entry + 8);
                if ((uint64_t)fileSize > MP6_CONTENT_WANTED_MAX_BYTES - *wantedBytes) {
                    if (err && errn) snprintf(err, errn, "wanted FST manifest exceeds 1.5 GiB");
                    return 0;
                }
                *wantedBytes += (uint64_t)fileSize;
                if (mp6_path_join_checked(path, sizeof(path), filesRoot, rel) != 0 ||
                    !mp6_path_file_exact_size(path, fileSize)) {
                    if (err && errn) snprintf(err, errn, "missing/truncated FST file: %s", rel);
                    return 0;
                }
                if (fileSize > 0) {
                    if (strcmp(rel, "opening.bnr") == 0) *found |= 1u << 0;
                    if (strcmp(rel, "sound/MP6_SND.msm") == 0) *found |= 1u << 1;
                    if (strcmp(rel, "sound/MP6_Str.pdt") == 0) *found |= 1u << 2;
                    if (strncmp(rel, "data/", 5) == 0) *found |= 1u << 3;
                    if (strncmp(rel, "mess/", 5) == 0) *found |= 1u << 4;
                    if (strncmp(rel, "mic/", 4) == 0) *found |= 1u << 5;
                }
            }
            ++i;
        }
    }
    return 1;
}

int mp6_dvd_validate_disc_root(const char *discRoot, char *err, size_t errn)
{
    static const char *critical[] = {
        "files/opening.bnr",
        "files/sound/MP6_SND.msm",
        "files/sound/MP6_Str.pdt",
    };
    char path[1200];
    char filesRoot[1200];
    char validationError[256];
    unsigned char boot[MP6_CONTENT_BOOT_BYTES];
    unsigned char *fst = NULL;
    FILE *f = NULL;
    long size;
    unsigned int found = 0;
    uint64_t wantedBytes = 0;
    int i, ok = 0;

    if (err != NULL && errn > 0) err[0] = '\0';
    if (discRoot == NULL || discRoot[0] == '\0') {
        if (err && errn) snprintf(err, errn, "content root is empty");
        return 0;
    }
    if (mp6_path_join_checked(path, sizeof(path), discRoot, "sys/boot.bin") != 0) {
        if (err && errn) snprintf(err, errn, "content root path is too long");
        return 0;
    }
    f = mp6_fopen_utf8(path, "rb");
    if (f == NULL || fread(boot, 1, sizeof(boot), f) != sizeof(boot) ||
        fgetc(f) != EOF || ferror(f)) {
        if (f) fclose(f);
        if (err && errn) snprintf(err, errn, "sys/boot.bin must be exactly 0x440 bytes: %s", path);
        return 0;
    }
    fclose(f);
    f = NULL;
    if (!mp6_content_boot_validate(boot, sizeof(boot))) {
        if (err && errn) snprintf(err, errn, "sys/boot.bin is not Mario Party 6 USA (GP6E01)");
        return 0;
    }

    if (mp6_path_join_checked(path, sizeof(path), discRoot, "sys/fst.bin") != 0 ||
        mp6_path_join_checked(filesRoot, sizeof(filesRoot), discRoot, "files") != 0) {
        if (err && errn) snprintf(err, errn, "content root path is too long");
        return 0;
    }
    f = mp6_fopen_utf8(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 12 ||
        size > (long)MP6_CONTENT_FST_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        if (f) fclose(f);
        if (err && errn) snprintf(err, errn, "missing or implausible: %s", path);
        return 0;
    }
    fst = (unsigned char *)malloc((size_t)size);
    if (fst == NULL || fread(fst, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(fst);
        if (err && errn) snprintf(err, errn, "could not read: %s", path);
        return 0;
    }
    fclose(f);
    f = NULL;
    if (!mp6_content_fst_validate(fst, (size_t)size, validationError, sizeof(validationError))) {
        if (err && errn) snprintf(err, errn, "invalid sys/fst.bin: %s", validationError);
        goto done;
    }
    if (!mp6_validate_fst_files_walk(fst, 0, "", filesRoot, &found,
                                     &wantedBytes, err, errn)) goto done;
    if ((found & 0x3fu) != 0x3fu) {
        if (err && errn) {
            snprintf(err, errn,
                     "FST is incomplete (need opening.bnr, both sound banks, and data/mess/mic files)");
        }
        goto done;
    }
    for (i = 0; i < (int)(sizeof(critical) / sizeof(critical[0])); ++i) {
        if (mp6_path_join_checked(path, sizeof(path), discRoot, critical[i]) != 0) {
            if (err && errn) snprintf(err, errn, "content root path is too long");
            goto done;
        }
        if (!mp6_path_file_nonempty(path)) {
            if (err && errn) snprintf(err, errn, "missing or empty: %s", path);
            goto done;
        }
    }
    ok = 1;
done:
    free(fst);
    return ok;
}

static void mp6_resolve_dvd_paths(void)
{
    char discRoot[MP6_DVD_PATH_CAP];
    char filesRoot[MP6_DVD_PATH_CAP];
    char fstPath[MP6_DVD_PATH_CAP];

    if (g_pathsResolved) return;
    g_pathsResolved = 1;

    /* Build-time paths normally fit. If an unusual checkout path does not,
     * keep both targets empty rather than operating on truncated names; the
     * runtime-relative candidate below still gets a chance to replace them. */
    if (mp6_set_resolved_paths(MP6_DVD_FILES_ROOT, MP6_DVD_FST_PATH) != 0) {
        g_resolvedFilesRoot[0] = '\0';
        g_resolvedFstPath[0] = '\0';
        fprintf(stderr, "[WARN] dvd_files: build-time disc paths are too long -- fallback disabled\n");
    }

    /* The exe-relative walk lives in mp6_host_disc_root (src/host/),
     * WITH the fst.bin openability probe -- it returns 0 only for a
     * verified-existing disc root, so appending "/files" and
     * "/sys/fst.bin" here always yields usable paths. */
    /* A build with MP6_DISC_ROOT explicitly selected must not silently use
     * another checkout's assets merely because its exe-relative tree exists.
     * The launcher's deliberate content-root override remains available. */
    if (!MP6_DVD_ROOT_EXPLICIT &&
        mp6_host_disc_root(discRoot, sizeof(discRoot)) == 0 &&
        mp6_path_join_checked(filesRoot, sizeof(filesRoot), discRoot, "files") == 0 &&
        mp6_path_join_checked(fstPath, sizeof(fstPath), discRoot, "sys/fst.bin") == 0 &&
        mp6_set_resolved_paths(filesRoot, fstPath) == 0) {
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
 * (src/gx/ui/launcher_core.cpp). Both are dead code in every other
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
    char root[MP6_DVD_PATH_CAP];
    char err[256];
    char *slash, *backslash;
    mp6_resolve_dvd_paths();
    if (filesRootOut != NULL &&
        mp6_path_copy_checked(filesRootOut, n, g_resolvedFilesRoot) != 0) {
        return 0;
    }
    if (mp6_path_copy_checked(root, sizeof(root), g_resolvedFstPath) != 0) return 0;
    slash = strrchr(root, '/');
    backslash = strrchr(root, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) return 0;
    *slash = '\0'; /* strip fst.bin */
    slash = strrchr(root, '/');
    backslash = strrchr(root, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) return 0;
    *slash = '\0'; /* strip sys/ */
    return mp6_dvd_validate_disc_root(root, err, sizeof(err));
}

/* Launcher menu override: replace both resolved paths outright (the
 * launcher validated fst.bin/files existence first). Must be called
 * before the first file open (the launcher calls it pre-GameMain); also
 * marks resolution done so a later mp6_resolve_dvd_paths() can't
 * overwrite it. */
int mp6_dvd_set_root_override(const char *filesRoot, const char *fstPath)
{
    if (mp6_set_resolved_paths(filesRoot, fstPath) != 0) {
        fprintf(stderr, "[DVD] launcher content override rejected: path is too long or invalid\n");
        fflush(stderr);
        return -1;
    }
    g_pathsResolved = 1;
    g_rootOverridden = 1;
    disc_close_idle();
    printf("[DVD] launcher-configured content root: \"%s\"\n", g_resolvedFilesRoot);
    fflush(stdout);
    return 0;
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

/* The launcher links content while it runs (first import, re-import). The
 * auto-detected root was resolved once at its first probe, before that
 * content existed; look again until the game has read the FST. */
void mp6_dvd_rescan_root(void)
{
    if (!g_fstLoadAttempted && !g_rootOverridden) g_pathsResolved = 0;
}

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
static BOOL fst_walk_build(u32 dirIdx, const char *prefix, size_t prefixLen)
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
            fprintf(stderr, "[WARN] dvd_files: FST entry path exceeds runtime path cap\n");
            return FALSE;
        }

        if (isDir) {
            if (!fst_walk_build(i, full, fullLen)) return FALSE;
            i = fst_field2(i);
        } else {
            g_entryPaths[i] = fst_dup(full, fullLen);
            if (g_entryPaths[i] == NULL) return FALSE;
            i++;
        }
    }
    return TRUE;
}

static void fst_discard_loaded_data(void)
{
    u32 i;
    if (g_entryPaths != NULL) {
        for (i = 0; i < g_fstMaxEntry; i++) free(g_entryPaths[i]);
        free(g_entryPaths);
    }
    free(g_fstData);
    g_fstData = NULL;
    g_fstMaxEntry = 0;
    g_fstStrings = NULL;
    g_entryPaths = NULL;
    g_fstOk = 0;
}

static void fst_load_once(void)
{
    FILE *f;
    long size = -1;
    char validationError[256];

    if (g_fstLoadAttempted) return;
    g_fstLoadAttempted = 1;

    mp6_resolve_dvd_paths();
    f = mp6_fopen_utf8(g_resolvedFstPath, "rb");
    if (!f) {
        fprintf(stderr,
                "[WARN] dvd_files: couldn't open FST at \"%s\" -- every real-file DVDOpen/"
                "DVDConvertPathToEntrynum will honestly report \"not found\"\n",
                g_resolvedFstPath);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 12 ||
        size > (long)MP6_CONTENT_FST_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[WARN] dvd_files: FST at \"%s\" has invalid size/seek (%ld bytes)\n",
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

    if (!mp6_content_fst_validate(g_fstData, (size_t)size,
                                  validationError, sizeof(validationError))) {
        fprintf(stderr, "[WARN] dvd_files: refusing malformed FST at \"%s\": %s\n",
                g_resolvedFstPath, validationError);
        free(g_fstData);
        g_fstData = NULL;
        return;
    }

    g_fstMaxEntry = fst_field2(0); /* root dir's own nextEntryOrLength == total entry count */
    g_fstStrings = (const char *)(g_fstData + FST_ENTRY_OFF(g_fstMaxEntry));

    g_entryPaths = (char **)calloc(g_fstMaxEntry, sizeof(char *));
    if (!g_entryPaths) {
        fprintf(stderr, "[WARN] dvd_files: out of memory building FST path table\n");
        free(g_fstData);
        g_fstData = NULL;
        g_fstStrings = NULL;
        g_fstMaxEntry = 0;
        return;
    }
    if (!fst_walk_build(0, "", 0)) {
        fprintf(stderr, "[WARN] dvd_files: out of memory/path capacity building FST path table\n");
        fst_discard_loaded_data();
        return;
    }
    g_discFiles = (DiscFile *)calloc(g_fstMaxEntry, sizeof(DiscFile));
    if (!g_discFiles) {
        fprintf(stderr, "[WARN] dvd_files: out of memory building the disc file table\n");
        fst_discard_loaded_data();
        return;
    }
    g_fstOk = 1;
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

    if (path == NULL) return -1;
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
 * Host file access.
 *
 * Disc files stay open. The first DVDOpen of an FST file entry opens its
 * host file (mp6_host_file_open), checks its size against fst.bin once, and
 * keeps the handle in g_discFiles[entrynum]; every later DVDOpen of that
 * entry reuses it, DVDClose only drops the game's tag, and reads are
 * positional (no seek state), so a handle can serve any number of game
 * handles at once. The game opens a file for every read (game/dvd.c and
 * the sound engine's sample fetches open, read and close each time), so
 * this removes a host open/size query/close per read: on Windows each of
 * those was a kernel round trip, on the web an awaited call into the page.
 *
 * At most MP6_DISC_HANDLE_CAP host handles stay open; past that the least
 * recently used one with no game handle on it is closed.
 *
 * g_openReal is the game-handle side table (same idiom as dll_bridge.c's
 * synthetic-REL table), keyed by DVDFileInfo* identity.
 * ======================================================================= */

#define MP6_MAX_OPEN_REAL 32
static struct {
    DVDFileInfo *key;
    s32 entry;
    u32 length;
    int archive; /* a data/ .bin archive: whole reads go to decode-ahead */
    u8 *archiveBase; /* where the game is reading it, block by block */
    u32 archiveFilled;
} g_openReal[MP6_MAX_OPEN_REAL];

static void disc_evict_idle(void)
{
    u32 i;
    DiscFile *oldest = NULL;
    for (i = 0; i < g_fstMaxEntry; i++) {
        DiscFile *d = &g_discFiles[i];
        if (d->file != NULL && d->users == 0 && (oldest == NULL || d->lastUse < oldest->lastUse)) {
            oldest = d;
        }
    }
    if (oldest != NULL) {
        mp6_host_file_close(oldest->file);
        oldest->file = NULL;
        g_discHandleCount--;
    }
}

/* Closes every host handle no game handle is using. */
static void disc_close_idle(void)
{
    u32 i;
    if (g_discFiles == NULL) return;
    for (i = 0; i < g_fstMaxEntry; i++) {
        if (g_discFiles[i].file != NULL && g_discFiles[i].users == 0) {
            mp6_host_file_close(g_discFiles[i].file);
            g_discFiles[i].file = NULL;
            g_discHandleCount--;
        }
    }
}

/* The host handle for FST file entry `entrynum` (whose disc-relative path is
 * relpath), opened and size-checked on first use; NULL when the file is
 * missing or has the wrong size. Counts one more user on success. */
static Mp6HostFile *disc_file_acquire(const char *relpath, s32 entrynum)
{
    char hostPath[MP6_DVD_PATH_CAP];
    DiscFile *d;
    Mp6HostFile *file;
    int64_t size;

    if (g_discFiles == NULL || entrynum < 0 || (u32)entrynum >= g_fstMaxEntry) return NULL;
    d = &g_discFiles[entrynum];
    if (d->file == NULL) {
        mp6_resolve_dvd_paths();
        if (mp6_path_join_checked(hostPath, sizeof(hostPath), g_resolvedFilesRoot, relpath) != 0) {
            fprintf(stderr, "[WARN] dvd_files: host path too long for \"%s\"\n", relpath);
            return NULL;
        }
        file = mp6_host_file_open(hostPath);
        if (file == NULL) {
            /* The FST says this path/entrynum exists, but the real bytes
             * aren't on this checkout -- report an honest, visible "not
             * found" one layer deeper than a bad path, rather than treating
             * a missing file as fatal (some paths legitimately don't exist
             * on an incomplete disc extraction). */
            printf("[DVD] \"%s\" (entrynum %d) is a real FST entry but the file is missing on this "
                   "checkout (looked for \"%s\")\n", relpath, entrynum, hostPath);
            return NULL;
        }
        size = mp6_host_file_size(file);
        if (size < 0 || (uint64_t)size != fst_field2((u32)entrynum)) {
            fprintf(stderr,
                    "[WARN] dvd_files: host file size for \"%s\" is %lld, but fst.bin requires %u -- refusing\n",
                    relpath, (long long)size, (unsigned)fst_field2((u32)entrynum));
            mp6_host_file_close(file);
            return NULL;
        }
        if (g_discHandleCount >= MP6_DISC_HANDLE_CAP) disc_evict_idle();
        d->file = file;
        g_discHandleCount++;
    }
    d->users++;
    d->lastUse = ++g_discUseClock;
    if (g_scenePrefetch > 0) scene_record_open(entrynum);
    return d->file;
}

/* =======================================================================
 * Scene prefetch.
 *
 * The game loads a scene's files one after another, each read waiting for
 * the one before: on the web every read is a round trip to the page, and
 * on a phone a file that has left the OS page cache is a flash read (the
 * S22+'s party menu read its board's 7.5 MB ARAM preload in one 35 ms
 * tick). Each overlay call or start (mp6_dvd_scene_hint, from the overlay
 * events) records the disc files the scene opens until the next overlay;
 * the next time that overlay comes up, all of them are fetched ahead
 * before the scene asks for the first: by the page's content provider on
 * the web (mp6_host_file_prefetch), elsewhere by a worker thread that reads
 * them once so the game's own reads find them in the page cache. The lists
 * are kept across launches in <pref dir>/mp6_disc_prefetch.txt, one line
 * per overlay: its name, then the disc-relative path of each file,
 * tab-separated. Until that file exists, the lists shipped in
 * res/disc_prefetch_seed.txt (same format, recorded from the game's scenes)
 * stand in, so a first launch prefetches too. MP6_DISC_PREFETCH=0 turns it
 * off.
 * ======================================================================= */

#define MP6_SCENE_MAX 160
#define MP6_SCENE_FILES 64
#define MP6_SCENE_FILE_MAX (8u * 1024u * 1024u) /* larger files are streamed, not loaded */
#define MP6_SCENE_LIST_FILE "mp6_disc_prefetch.txt"
#define MP6_SCENE_SEED_FILE "res/disc_prefetch_seed.txt"
#define MP6_PREFETCH_QUEUE 128
#define MP6_PREFETCH_CHUNK (1u << 20)

typedef struct {
    char name[32];
    s32 files[MP6_SCENE_FILES];
    int count;
} SceneFiles;

static SceneFiles g_scenes[MP6_SCENE_MAX];
static int g_sceneCount;
/* -1 = not decided yet, 0 = off, 1 = the host prefetches, 2 = the worker below */
static int g_scenePrefetch = -1;
static int g_scenesDirty;
static SceneFiles g_sceneRecording; /* name[0] == 0 when not recording */

/* The prefetch worker's queue of file ranges to read. The paths it builds
 * them from (g_resolvedFilesRoot, g_entryPaths) are fixed once the FST has
 * loaded, before the worker starts. */
typedef struct {
    s32 entry;
    u32 offset, length;
} PrefetchItem;
static PrefetchItem g_prefetchQueue[MP6_PREFETCH_QUEUE];
static unsigned g_prefetchHead, g_prefetchTail;
static Mp6Mutex g_prefetchLock;
static Mp6Sem g_prefetchSem;

static int mp6_dvd_is_archive(const char *relpath);

static void prefetch_worker(void *unused)
{
    unsigned char *buf = (unsigned char *)malloc(MP6_PREFETCH_CHUNK);
    (void)unused;
    for (;;) {
        char hostPath[MP6_DVD_PATH_CAP];
        Mp6HostFile *file;
        uint64_t end, at;
        PrefetchItem item;
        mp6_host_sem_wait(&g_prefetchSem);
        mp6_host_mutex_lock(&g_prefetchLock);
        item = g_prefetchQueue[g_prefetchHead++ % MP6_PREFETCH_QUEUE];
        mp6_host_mutex_unlock(&g_prefetchLock);
        if (buf == NULL ||
            mp6_path_join_checked(hostPath, sizeof(hostPath), g_resolvedFilesRoot, g_entryPaths[item.entry]) != 0 ||
            (file = mp6_host_file_open(hostPath)) == NULL) {
            continue;
        }
        end = (uint64_t)item.offset + item.length;
        if (end > fst_field2((u32)item.entry)) end = fst_field2((u32)item.entry);
        /* A whole archive goes to decode-ahead too, so the scene's files are
         * decoded off the game thread before the scene asks for them. */
        at = item.offset;
        if (item.offset == 0 && end == fst_field2((u32)item.entry) && mp6_dvd_is_archive(g_entryPaths[item.entry])) {
            unsigned char *whole = (unsigned char *)malloc((size_t)end);
            if (whole != NULL && mp6_host_file_read_at(file, whole, (size_t)end, 0) == (int64_t)end) {
                mp6_decode_ahead_preload(whole, (uint32_t)end, item.entry);
                at = end;
            }
            free(whole);
            if (at == end) {
                mp6_host_file_close(file);
                continue;
            }
        }
        for (at = item.offset; at < end; at += MP6_PREFETCH_CHUNK) {
            uint64_t want = end - at < MP6_PREFETCH_CHUNK ? end - at : MP6_PREFETCH_CHUNK;
            if (mp6_host_file_read_at(file, buf, (size_t)want, at) <= 0) break;
        }
        mp6_host_file_close(file);
    }
}

static int prefetch_worker_start(void)
{
    if (!mp6_host_sem_init(&g_prefetchSem)) return 0; /* no threads (the web without a provider) */
    mp6_host_mutex_init(&g_prefetchLock);
    return mp6_host_thread_start(prefetch_worker, NULL) == 0;
}

/* Reads [offset, offset+length) of FST file `entry` ahead: the page's
 * provider on the web, the worker elsewhere. */
static void prefetch_range(s32 entry, u32 offset, u32 length)
{
    char hostPath[MP6_DVD_PATH_CAP];
    if (g_scenePrefetch == 1) {
        if (mp6_path_join_checked(hostPath, sizeof(hostPath), g_resolvedFilesRoot, g_entryPaths[entry]) == 0) {
            mp6_host_file_prefetch(hostPath, offset, length);
            /* Decoded in idle time once the page has it (cooperative decode-ahead). */
            if (offset == 0 && length == fst_field2((u32)entry) && mp6_dvd_is_archive(g_entryPaths[entry]))
                mp6_decode_ahead_want_disc(entry, length);
        }
        return;
    }
    if (g_scenePrefetch != 2) return;
    mp6_host_mutex_lock(&g_prefetchLock);
    if (g_prefetchTail - g_prefetchHead < MP6_PREFETCH_QUEUE) {
        PrefetchItem item;
        item.entry = entry;
        item.offset = offset;
        item.length = length;
        g_prefetchQueue[g_prefetchTail++ % MP6_PREFETCH_QUEUE] = item;
        mp6_host_mutex_unlock(&g_prefetchLock);
        mp6_host_sem_post(&g_prefetchSem);
        return;
    }
    mp6_host_mutex_unlock(&g_prefetchLock); /* full: this file is read on demand */
}

static void scene_prefetch_file(s32 entry)
{
    prefetch_range(entry, 0, fst_field2((u32)entry));
}

static SceneFiles *scene_find(const char *name, int create)
{
    int i;
    for (i = 0; i < g_sceneCount; i++) {
        if (strcmp(g_scenes[i].name, name) == 0) return &g_scenes[i];
    }
    if (!create || g_sceneCount >= MP6_SCENE_MAX) return NULL;
    memset(&g_scenes[g_sceneCount], 0, sizeof(g_scenes[0]));
    snprintf(g_scenes[g_sceneCount].name, sizeof(g_scenes[0].name), "%s", name);
    return &g_scenes[g_sceneCount++];
}

static int scene_list_path(char *out, size_t n)
{
    char dir[MP6_DVD_PATH_CAP];
    return mp6_host_pref_dir(dir, sizeof(dir)) == 0 &&
           mp6_path_join_checked(out, n, dir, MP6_SCENE_LIST_FILE) == 0;
}

static void scene_list_load(void)
{
    char path[MP6_DVD_PATH_CAP];
    char line[4096];
    FILE *f = NULL;
    if (scene_list_path(path, sizeof(path))) f = mp6_fopen_utf8(path, "r");
#ifndef __ANDROID__ /* Android ships res/ inside the APK, out of stdio's reach */
    if (f == NULL) f = mp6_fopen_utf8(MP6_SCENE_SEED_FILE, "r");
#endif
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *field = strtok(line, "\t\r\n");
        SceneFiles *scene = field != NULL ? scene_find(field, 1) : NULL;
        if (scene == NULL) continue;
        scene->count = 0;
        while ((field = strtok(NULL, "\t\r\n")) != NULL && scene->count < MP6_SCENE_FILES) {
            s32 entry = mp6_dvd_path_to_entrynum(field);
            if (entry > 0 && !fst_is_dir((u32)entry)) scene->files[scene->count++] = entry;
        }
    }
    fclose(f);
}

static void scene_list_save(void)
{
    char path[MP6_DVD_PATH_CAP];
    char tmp[MP6_DVD_PATH_CAP];
    FILE *f;
    int i, j;
    if (!scene_list_path(path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
        (f = mp6_fopen_utf8(tmp, "w")) == NULL) {
        return;
    }
    for (i = 0; i < g_sceneCount; i++) {
        fputs(g_scenes[i].name, f);
        for (j = 0; j < g_scenes[i].count; j++) fprintf(f, "\t%s", g_entryPaths[g_scenes[i].files[j]]);
        fputc('\n', f);
    }
    if (fclose(f) == 0 && mp6_host_atomic_replace_file(tmp, path) == 0) g_scenesDirty = 0;
}

/* Ends the recording in progress, keeping it as its scene's list. */
static void scene_record_finish(void)
{
    SceneFiles *scene;
    if (g_sceneRecording.name[0] == '\0') return;
    scene = g_sceneRecording.count > 0 ? scene_find(g_sceneRecording.name, 1) : NULL;
    if (scene != NULL &&
        (scene->count != g_sceneRecording.count ||
         memcmp(scene->files, g_sceneRecording.files, sizeof(s32) * (size_t)scene->count) != 0)) {
        *scene = g_sceneRecording;
        g_scenesDirty = 1;
    }
    g_sceneRecording.name[0] = '\0';
    if (g_scenesDirty) scene_list_save();
}

/* disc_file_acquire: a scene being recorded opened `entry`. */
static void scene_record_open(s32 entry)
{
    int i;
    if (g_sceneRecording.name[0] == '\0') return;
    if (fst_field2((u32)entry) > MP6_SCENE_FILE_MAX) return;
    for (i = 0; i < g_sceneRecording.count; i++) {
        if (g_sceneRecording.files[i] == entry) return;
    }
    if (g_sceneRecording.count < MP6_SCENE_FILES) g_sceneRecording.files[g_sceneRecording.count++] = entry;
}

void mp6_dvd_prefetch_range(const char *path, u32 offset, u32 length)
{
    s32 entry;
    if (g_scenePrefetch <= 0) return; /* off, or no scene has started the machinery yet */
    entry = mp6_dvd_path_to_entrynum(path);
    if (entry <= 0 || fst_is_dir((u32)entry) || g_entryPaths == NULL || g_entryPaths[entry] == NULL) return;
    prefetch_range(entry, offset, length);
}

int64_t mp6_dvd_read_entry_now(int32_t entry, void *dst, uint32_t length)
{
    char hostPath[MP6_DVD_PATH_CAP];
    Mp6HostFile *file;
    int64_t got;
    fst_load_once();
    if (!g_fstOk || entry <= 0 || g_entryPaths == NULL || g_entryPaths[entry] == NULL ||
        mp6_path_join_checked(hostPath, sizeof(hostPath), g_resolvedFilesRoot, g_entryPaths[entry]) != 0) {
        return -1;
    }
#ifdef __EMSCRIPTEN__
    /* The page's provider: only what it already holds, never a wait. */
    if (mp6_web_content_has(hostPath)) return mp6_web_content_read_cached_at(hostPath, dst, length, 0);
#endif
    file = mp6_host_file_open(hostPath);
    if (file == NULL) return -1;
    got = mp6_host_file_read_at(file, dst, length, 0);
    mp6_host_file_close(file);
    return got;
}

void mp6_dvd_scene_prefetch(const char *scene)
{
    SceneFiles *known;
    int i;
    /* Only once a scene start has set the machinery up (mp6_dvd_scene_hint). */
    if (scene == NULL || g_scenePrefetch <= 0) return;
    known = scene_find(scene, 0);
    for (i = 0; known != NULL && i < known->count; i++) scene_prefetch_file(known->files[i]);
}

void mp6_dvd_scene_hint(const char *scene)
{
    SceneFiles *known;
    int i;
    if (scene == NULL || scene[0] == '\0' || g_scenePrefetch == 0) return;
    if (g_scenePrefetch < 0) {
        const char *env = getenv("MP6_DISC_PREFETCH");
        g_scenePrefetch = 0;
        if (env != NULL && env[0] == '0') return;
        fst_load_once();
        if (!g_fstOk) return;
        if (mp6_host_file_prefetch(NULL, 0, 0)) g_scenePrefetch = 1;
        else if (prefetch_worker_start()) g_scenePrefetch = 2;
        else return;
        scene_list_load();
        atexit(scene_record_finish); /* keep what the last scene recorded */
    }
    /* The overlay's call and its start both hint; record once. */
    if (strcmp(g_sceneRecording.name, scene) == 0) return;
    scene_record_finish();

    known = scene_find(scene, 0);
    for (i = 0; known != NULL && i < known->count; i++) scene_prefetch_file(known->files[i]);
    memset(&g_sceneRecording, 0, sizeof(g_sceneRecording));
    snprintf(g_sceneRecording.name, sizeof(g_sceneRecording.name), "%s", scene);
}

static void disc_file_release(s32 entrynum)
{
    DiscFile *d = &g_discFiles[entrynum];
    if (d->users > 0) d->users--;
    d->lastUse = ++g_discUseClock;
}

static BOOL real_tag(DVDFileInfo *f, s32 entry, u32 length, int archive)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key == NULL) {
            g_openReal[i].key = f;
            g_openReal[i].entry = entry;
            g_openReal[i].length = length;
            g_openReal[i].archive = archive;
            g_openReal[i].archiveBase = NULL;
            g_openReal[i].archiveFilled = 0;
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
    if (f == NULL) return -1;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key == f) return i;
    }
    return -1;
}

static void real_untag(int idx)
{
    disc_file_release(g_openReal[idx].entry);
    g_openReal[idx].key = NULL;
    g_openReal[idx].entry = -1;
    g_openReal[idx].length = 0;
    g_openReal[idx].archiveBase = NULL;
}

static int mp6_dvd_is_archive(const char *relpath)
{
    size_t n = strlen(relpath);
    return strncmp(relpath, "data/", 5) == 0 && n > 4 && strcmp(relpath + n - 4, ".bin") == 0;
}

static BOOL mp6_dvd_open_relpath(const char *relpath, s32 entrynum, DVDFileInfo *fileInfo)
{
    u32 size;

    if (relpath == NULL || relpath[0] == '\0' || fileInfo == NULL) return FALSE;
    if (disc_file_acquire(relpath, entrynum) == NULL) return FALSE;
    size = fst_field2((u32)entrynum); /* the host file's size, checked when it was opened */

    memset(fileInfo, 0, sizeof(*fileInfo));
    if (!real_tag(fileInfo, entrynum, size, mp6_dvd_is_archive(relpath))) {
        disc_file_release(entrynum);
        return FALSE;
    }
    fileInfo->startAddr = 0; /* see file header note: no raw ISO, reads are host-file-relative */
    fileInfo->length = size;
    return TRUE;
}

BOOL mp6_dvd_open_by_path(const char *path, DVDFileInfo *fileInfo)
{
    s32 entry;
    const char *canonical;

    if (path == NULL || fileInfo == NULL) return FALSE;
    entry = mp6_dvd_path_to_entrynum(path);

    if (entry < 0 || fst_is_dir((u32)entry)) return FALSE;
    /* The lookup grammar deliberately supports SDK-style root/current-dir
     * spellings (leading '/', './', and '../'). Never concatenate that
     * caller spelling onto the host root: at root, "../opening.bnr" resolves
     * to the real opening.bnr FST entry but would otherwise open a sibling
     * outside files/. The validated FST reverse table is the canonical,
     * traversal-free host-relative identity for both path and entry opens. */
    canonical = g_entryPaths ? g_entryPaths[entry] : NULL;
    if (canonical == NULL) return FALSE;
    return mp6_dvd_open_relpath(canonical, entry, fileInfo);
}

BOOL mp6_dvd_open_by_entrynum(s32 entrynum, DVDFileInfo *fileInfo)
{
    const char *relpath;

    if (fileInfo == NULL) return FALSE;
    fst_load_once();
    if (!g_fstOk || entrynum < 0 || (u32)entrynum >= g_fstMaxEntry || fst_is_dir((u32)entrynum)) {
        return FALSE;
    }
    relpath = g_entryPaths ? g_entryPaths[entrynum] : NULL;
    if (!relpath) return FALSE; /* shouldn't happen for a valid non-dir entrynum; defensive */
    return mp6_dvd_open_relpath(relpath, entrynum, fileInfo);
}

/* The game reads archives whole, either at once or in consecutive blocks
 * into one buffer; once all of it is in, decode-ahead gets the bytes. */
static void mp6_dvd_note_archive_read(int idx, u8 *addr, s32 offset, u32 got)
{
    if (offset == 0) {
        g_openReal[idx].archiveBase = addr;
        g_openReal[idx].archiveFilled = 0;
    } else if (g_openReal[idx].archiveBase == NULL || (u32)offset != g_openReal[idx].archiveFilled ||
               addr != g_openReal[idx].archiveBase + offset) {
        g_openReal[idx].archiveBase = NULL;
        return;
    }
    g_openReal[idx].archiveFilled += got;
    if (g_openReal[idx].archiveFilled == g_openReal[idx].length) {
        mp6_decode_ahead_disc_read(g_openReal[idx].archiveBase, g_openReal[idx].length,
                                   g_openReal[idx].entry);
        g_openReal[idx].archiveBase = NULL;
    }
}

BOOL mp6_dvd_read(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset)
{
    int idx = real_lookup(fileInfo);
    u32 avail;
    size_t toRead;

    if (idx < 0 || (length > 0 && addr == NULL)) {
        fprintf(stderr, "[WARN] dvd_files: read on an untracked DVDFileInfo* %p\n", (void *)fileInfo);
        return FALSE;
    }
    if (length < 0 || offset < 0 || (u32)offset > g_openReal[idx].length) return FALSE;

    avail = g_openReal[idx].length - (u32)offset;
    toRead = avail < (u32)length ? avail : (size_t)length;

    uint64_t traceStart = mp6_load_trace_begin();
    int64_t got = toRead > 0
        ? mp6_host_file_read_at(g_discFiles[g_openReal[idx].entry].file, addr, toRead, (uint64_t)offset)
        : 0;
    mp6_load_trace_end(traceStart, MP6_LOAD_DVD, got > 0 ? (uint64_t)got : 0);
    if (got != (int64_t)toRead) {
        fprintf(stderr, "[WARN] dvd_files: short read (wanted %zu bytes at offset %d)\n", toRead, offset);
        return FALSE;
    }
    if (g_openReal[idx].archive) {
        /* One file read out of an archive: decode-ahead may hold it already.
         * Before noting the read, so a whole archive read in blocks ends with
         * its whole view, not its last block's. */
        if ((u32)toRead != g_openReal[idx].length)
            mp6_decode_ahead_disc_part(addr, (u32)toRead, g_openReal[idx].entry, (u32)offset);
        mp6_dvd_note_archive_read(idx, (u8 *)addr, offset, (u32)toRead);
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
    real_untag(idx);
}

/* =======================================================================
 * Savestate support
 * =======================================================================
 * This whole TU's statics are carved out of the savestate (see
 * HOST_STATE_SECTION_SOURCES in tools/build.py), because every one of them
 * is host-owned: the FST blob and its string pool are CRT-heap pointers,
 * g_entryPaths is a heap array of heap strings, and the resolved paths
 * belong to the machine that is RUNNING, not the one that captured.
 *
 * The single thing the carve-out gives up is the identity of files that
 * were open at capture time -- g_openReal[] records a DVDFileInfo* key the
 * restored game state may not hold any more, so its game handles are
 * dropped on restore. That is acceptable ONLY because no
 * compiled caller in this build holds a DVD file open across a frame
 * boundary: every DVDOpen in game/dvd.c closes within the same call,
 * msm_bridge.c reads through a stack-local DVDFileInfo, msmstream.c is not
 * compiled, and THP -- the one caller that would hold a handle across
 * HuPrcVSleep -- is gated off at THPInit (src/null/shims_manual.c).
 *
 * That is an assumption about the rest of the tree, so it is CHECKED rather
 * than trusted: mp6_dvd_open_handle_count() lets the capture path assert it
 * and warn loudly if it ever stops holding (enabling THP or compiling
 * msmstream.c would do it). When that day comes the fix is to move
 * {key, entrynum} into restored memory and re-tag each slot from the
 * rehydrate hook; the entrynum is a restore-stable identity because the FST
 * is deterministically rebuilt from the resolved path. The host handles in
 * g_discFiles belong to this process, not the game, and stay open. */
int mp6_dvd_open_handle_count(void)
{
    int i, n = 0;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key != NULL) {
            n++;
        }
    }
    return n;
}

/* Post-restore hook. The restored game state refers to no file this process
 * has open, so any game handle tagged here is now orphaned -- drop it rather
 * than leak its host handle's use count. Everything else this TU owns (FST
 * blob, open host handles, path cache, load latch) is carved out and
 * therefore already correct for THIS process; that is the whole point of
 * the carve-out, so there is deliberately nothing to rebuild here. Safe
 * before any DVD path has been resolved: the table is zeroed. */
void mp6_dvd_savestate_rehydrate(void)
{
    int i;
    for (i = 0; i < MP6_MAX_OPEN_REAL; i++) {
        if (g_openReal[i].key != NULL) real_untag(i);
    }
}
