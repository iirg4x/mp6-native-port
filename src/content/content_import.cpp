/* MP6 native port -- first-run content import engine implementation.
 * See content_import.h for the design contract (wanted set, torn-import
 * safety, threading rules).
 *
 * nod usage follows partyboard's proven shape (their src/port/
 * iso_validate.cpp, referenced as a recipe -- the NodDiscStream-over-
 * SDL_IOStream shim is the load-bearing idea: SDL_IOFromFile transparently
 * opens Android SAF content:// URIs, so one importer serves Windows paths
 * and Android document picks identically). nod itself is encounter/nod
 * v2.0.0-alpha.10 -- the exact version aurora's dependency table pins --
 * consumed through its C FFI (include/nod.h), prebuilt DLL on Windows,
 * cargo-built staticlib on Android (tools/fetch_nod.py).
 */
#include "content_import.h"
#include "content_fst_validate.h"
#include "content_path_safe.h" /* SECURITY: FST-name traversal gate (see below) */
#include "mp6_utf8_file.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <nod.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/* Narrow C seam only: mp6_dvd_files.h also declares the decomp DVD API and
 * therefore includes the competing decomp `dolphin.h` universe. This TU is
 * compiled with Aurora/SDL headers, so importing that umbrella header creates
 * type collisions. The validator itself needs only plain C strings/sizes. */
extern "C" int mp6_dvd_validate_disc_root(const char *discRoot, char *err, size_t errn);

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


/* =======================================================================
 * 1. Shared progress state (one import at a time by design).
 * ======================================================================= */

namespace {

std::mutex g_mutex;             /* guards the string fields + thread handle */
std::mutex g_lifecycleMutex;    /* serializes start/reset and their join handoff */
std::thread g_thread;
std::atomic<int> g_state { MP6_IMPORT_IDLE };
std::atomic<uint64_t> g_bytesDone { 0 };
std::atomic<uint64_t> g_bytesTotal { 0 };
std::atomic<int> g_filesDone { 0 };
std::atomic<int> g_filesTotal { 0 };
std::atomic<bool> g_cancel { false };
char g_currentFile[260];
char g_error[512];

struct ImportWorkerShutdown {
    ~ImportWorkerShutdown()
    {
        std::thread worker;
        std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
        g_cancel.store(true);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_thread.joinable()) worker = std::move(g_thread);
        }
        if (worker.joinable()) worker.join();
    }
};

/* Declared after every object its destructor uses, so they remain alive while
 * a process-exit teardown waits for an import that outlived its UI owner. */
ImportWorkerShutdown g_workerShutdown;

struct SdlIoOwner {
    SDL_IOStream *value;
    explicit SdlIoOwner(SDL_IOStream *p) : value(p) {}
    ~SdlIoOwner() { close(); }
    void close()
    {
        SDL_IOStream *owned = value;
        value = nullptr;
        if (owned != nullptr) SDL_CloseIO(owned);
    }
    void release() { value = nullptr; }
};

struct NodOwner {
    NodHandle *value;
    explicit NodOwner(NodHandle *p = nullptr) : value(p) {}
    ~NodOwner() { if (value != nullptr) nod_free(value); }
    NodOwner(const NodOwner &) = delete;
    NodOwner &operator=(const NodOwner &) = delete;
};

void set_current_file(const char *rel)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(g_currentFile, sizeof(g_currentFile), "%s", rel);
}

void fail(const char *fmtstr, const char *a = nullptr, const char *b = nullptr)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snprintf(g_error, sizeof(g_error), fmtstr, a ? a : "", b ? b : "");
    }
    g_state.store(MP6_IMPORT_FAILED);
}

/* =======================================================================
 * 2. The wanted file set (files-root-relative; see content_import.h).
 * ======================================================================= */

bool path_has_prefix(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

bool wanted_file(const char *rel)
{
    if (strcmp(rel, "opening.bnr") == 0) return true;
    if (strcmp(rel, "sound/MP6_SND.msm") == 0) return true;
    if (strcmp(rel, "sound/MP6_Str.pdt") == 0) return true;
    return path_has_prefix(rel, "data") || path_has_prefix(rel, "mess") || path_has_prefix(rel, "mic");
}

/* =======================================================================
 * 3. Destination plumbing (plain filesystem; SDL_CreateDirectory creates
 * missing parents too -- SDL3 semantics).
 * ======================================================================= */

bool operational_path_supported(const std::string &path)
{
    return !path.empty() && mp6_utf8_path_supported(path.c_str());
}

bool create_directory_checked(const std::string &path)
{
    return operational_path_supported(path) && SDL_CreateDirectory(path.c_str());
}

bool rename_path_checked(const std::string &from, const std::string &to)
{
    return operational_path_supported(from) && operational_path_supported(to) &&
           SDL_RenamePath(from.c_str(), to.c_str());
}

bool remove_path_checked(const std::string &path)
{
    return operational_path_supported(path) && SDL_RemovePath(path.c_str());
}

bool ensure_parent_dirs(const std::string &filePath)
{
    size_t slash = filePath.find_last_of('/');
    if (!operational_path_supported(filePath)) return false;
    if (slash == std::string::npos) return true;
    std::string dir = filePath.substr(0, slash);
    return create_directory_checked(dir);
}

static const char *kIncomingSuffix = ".incoming";
static const char *kPreviousSuffix = ".previous";
static const char *kRecoverySuffix = ".recovery";
static const char *kCompleteMarker = "sys/.mp6-content-complete";

bool transaction_paths_supported(const char *root) noexcept
{
    try {
        if (root == nullptr || root[0] == '\0') return false;
        const std::string dest(root);
        return operational_path_supported(dest) &&
               operational_path_supported(dest + kIncomingSuffix) &&
               operational_path_supported(dest + kPreviousSuffix) &&
               operational_path_supported(dest + kRecoverySuffix) &&
               operational_path_supported(dest + "/" + kCompleteMarker);
    } catch (...) {
        return false;
    }
}

bool path_exists(const std::string &path, SDL_PathType *typeOut = nullptr)
{
    SDL_PathInfo info {};
    if (!operational_path_supported(path)) return false;
    if (!SDL_GetPathInfo(path.c_str(), &info)) return false;
    if (typeOut != nullptr) *typeOut = info.type;
    return true;
}

/* SDL_GetPathInfo deliberately follows symlinks. That is useful for ordinary
 * reads but unsafe for recursive cleanup: a pre-existing `.incoming` symlink
 * or Windows junction could otherwise make cancellation/recovery delete a
 * tree outside the selected content destination. Refuse every link/reparse
 * point before enumerating or removing it; leaving an artifact for the user
 * to resolve is the only fail-closed outcome. */
bool path_safe_to_remove(const std::string &path)
{
    if (!operational_path_supported(path)) return false;
#ifdef _WIN32
    wchar_t native[MP6_WINDOWS_PATH_WCHARS];
    if (mp6_utf8_to_wide_full_path(path.c_str(), native,
                                   MP6_WINDOWS_PATH_WCHARS) != 0) return false;
    const DWORD attrs = GetFileAttributesW(native);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode);
#endif
}

bool remove_tree(const std::string &path)
{
    SDL_PathType type;
    if (!operational_path_supported(path)) return false;
    if (!path_exists(path, &type)) return true;
    if (!path_safe_to_remove(path)) {
        fprintf(stderr, "[CONTENT] refusing to recursively remove symlink/reparse path: %s\n",
                path.c_str());
        return false;
    }
    if (type == SDL_PATHTYPE_DIRECTORY) {
        struct RemoveCtx { bool ok; } ctx { true };
        bool enumerated = SDL_EnumerateDirectory(
            path.c_str(),
            [](void *ud, const char *dirname, const char *fname) noexcept -> SDL_EnumerationResult {
                RemoveCtx *ctx = (RemoveCtx *)ud;
                try {
                    std::string child = std::string(dirname) + fname;
                    if (remove_tree(child)) return SDL_ENUM_CONTINUE;
                } catch (...) {
                    /* Never unwind through SDL's C callback boundary. */
                }
                {
                    ctx->ok = false;
                    return SDL_ENUM_FAILURE;
                }
            },
            &ctx);
        if (!enumerated || !ctx.ok) return false;
    }
    return remove_path_checked(path);
}

bool marker_exists(const std::string &root)
{
    SDL_PathType type;
    return path_exists(root + "/" + kCompleteMarker, &type) && type == SDL_PATHTYPE_FILE;
}

bool valid_disc_root(const std::string &root)
{
    SDL_PathType type;
    char err[512];
    return path_exists(root, &type) && type == SDL_PATHTYPE_DIRECTORY &&
           mp6_dvd_validate_disc_root(root.c_str(), err, sizeof(err));
}

bool replace_via_quarantine(const std::string &candidate, const std::string &dest,
                            const std::string &quarantine)
{
    if (path_exists(quarantine) && !remove_tree(quarantine)) return false;
    if (!rename_path_checked(dest, quarantine)) return false;
    if (rename_path_checked(candidate, dest)) {
        remove_tree(quarantine);
        return true;
    }
    /* Best-effort rollback. If it is itself interrupted, the next recovery
     * recognizes the quarantine and restores it. */
    (void)rename_path_checked(quarantine, dest);
    return false;
}

/* Recover the only two interruption windows in the directory-swap publish:
 * old root renamed to .previous but incoming not installed yet, or a fully
 * validated incoming tree carrying its completion marker not renamed yet. */
int recover_disc_root(const std::string &dest)
{
    std::string incoming = dest + kIncomingSuffix;
    std::string previous = dest + kPreviousSuffix;
    std::string quarantine = dest + kRecoverySuffix;
    if (!operational_path_supported(dest) || !operational_path_supported(incoming) ||
        !operational_path_supported(previous) || !operational_path_supported(quarantine)) {
        return 0;
    }
    bool destExists = path_exists(dest);
    bool previousExists = path_exists(previous);
    bool incomingExists = path_exists(incoming);
    bool destValid = destExists && valid_disc_root(dest);
    bool previousValid = previousExists && valid_disc_root(previous);
    bool incomingValid = incomingExists && marker_exists(incoming) && valid_disc_root(incoming);

    if (destValid) {
        if (path_exists(previous) && !remove_tree(previous)) {
            fprintf(stderr, "[CONTENT] warning: could not remove stale backup %s\n", previous.c_str());
        }
        if (path_exists(incoming) && !remove_tree(incoming)) {
            fprintf(stderr, "[CONTENT] warning: could not remove stale incoming tree %s\n", incoming.c_str());
        }
        remove_tree(quarantine);
        return 1;
    }

    if (destExists) {
        if (previousValid && replace_via_quarantine(previous, dest, quarantine)) {
            remove_tree(incoming);
            printf("[CONTENT] recovered previous content tree after interrupted publish: %s\n", dest.c_str());
            return 1;
        }
        if (incomingValid && !previousValid &&
            replace_via_quarantine(incoming, dest, quarantine)) {
            (void)remove_path_checked(dest + "/" + kCompleteMarker);
            remove_tree(previous);
            printf("[CONTENT] recovered completed content tree over an invalid destination: %s\n",
                   dest.c_str());
            return 1;
        }
        /* Never discard a backup unless a complete destination is active.
         * A partial, unmarked incoming tree is the only disposable artifact. */
        if (!incomingValid) remove_tree(incoming);
        return 0;
    }

    if (path_exists(quarantine)) {
        const std::string *candidate = previousValid ? &previous : (incomingValid ? &incoming : nullptr);
        if (candidate != nullptr && rename_path_checked(*candidate, dest)) {
            if (candidate == &incoming) (void)remove_path_checked(dest + "/" + kCompleteMarker);
            remove_tree(quarantine);
            if (candidate != &previous) remove_tree(previous);
            if (candidate != &incoming) remove_tree(incoming);
            return 1;
        }
        (void)rename_path_checked(quarantine, dest);
        return 0;
    }

    if (previousValid && rename_path_checked(previous, dest)) {
        remove_tree(incoming);
        printf("[CONTENT] recovered previous content tree after interrupted publish: %s\n", dest.c_str());
        return 1;
    }
    if (incomingValid) {
        if (rename_path_checked(incoming, dest)) {
            (void)remove_path_checked(dest + "/" + kCompleteMarker);
            remove_tree(previous);
            printf("[CONTENT] completed an interrupted content publish: %s\n", dest.c_str());
            return 1;
        }
        return 0;
    }
    /* Invalid backups are evidence, not bootable content. Preserve them
     * under their sibling names rather than activating them. */
    remove_tree(incoming);
    return 0;
}

bool prepare_incoming(const std::string &dest, std::string &incoming)
{
    int recovered = recover_disc_root(dest);
    incoming = dest + kIncomingSuffix;
    if (!recovered) {
        std::string previous = dest + kPreviousSuffix;
        std::string quarantine = dest + kRecoverySuffix;
        if (valid_disc_root(previous) || valid_disc_root(quarantine) ||
            valid_disc_root(incoming)) {
            /* A failed rename must never be followed by deleting/reusing one
             * of the remaining proven-good trees. Leave every artifact in
             * place so the next recovery attempt (or a user) can resolve it. */
            fail("could not resolve an existing valid transactional content tree near %s",
                 dest.c_str());
            return false;
        }
    }
    if (!remove_tree(incoming) || !create_directory_checked(incoming)) {
        fail("could not prepare transactional import directory %s", incoming.c_str());
        return false;
    }
    return true;
}

void cleanup_failed_incoming(const std::string &incoming, const std::string &dest)
{
    if (incoming.empty() || !path_exists(incoming)) return;
    std::string previous = dest + kPreviousSuffix;
    std::string quarantine = dest + kRecoverySuffix;
    if (valid_disc_root(incoming) && !valid_disc_root(dest) &&
        !valid_disc_root(previous) && !valid_disc_root(quarantine)) {
        fprintf(stderr,
                "[CONTENT] preserving the only validated content tree after a failed publish: %s\n",
                incoming.c_str());
        return;
    }
    if (!remove_tree(incoming)) {
        fprintf(stderr, "[CONTENT] warning: could not clean failed incoming tree %s\n",
                incoming.c_str());
    }
}

/* One output file, chunk-copied from an abstract reader. Returns false on
 * failure or cancel (g_state/g_error already set on failure; caller checks
 * g_cancel for the distinction). */
template <typename ReadFn>
bool write_dest_file(const std::string &destPath, const char *relName, uint64_t size, ReadFn read)
{
    if (!ensure_parent_dirs(destPath)) {
        fail("could not create directory for %s", destPath.c_str());
        return false;
    }
    FILE *out = mp6_fopen_utf8(destPath.c_str(), "wb");
    if (out == nullptr) {
        fail("could not create %s (check free space and permissions)", destPath.c_str());
        return false;
    }
    static thread_local std::vector<uint8_t> buf;
    buf.resize(1024 * 1024);
    uint64_t left = size;
    while (left > 0) {
        if (g_cancel.load()) {
            fclose(out);
            mp6_remove_utf8(destPath.c_str());
            return false;
        }
        size_t want = (size_t)(left < buf.size() ? left : buf.size());
        int64_t got = read(buf.data(), want);
        if (got <= 0 || (uint64_t)got > left || (size_t)got > want) {
            fclose(out);
            mp6_remove_utf8(destPath.c_str());
            fail("read error in %s (source truncated or unreadable)", relName);
            return false;
        }
        if (fwrite(buf.data(), 1, (size_t)got, out) != (size_t)got) {
            fclose(out);
            mp6_remove_utf8(destPath.c_str());
            fail("write error at %s (out of space?)", destPath.c_str());
            return false;
        }
        left -= (uint64_t)got;
        g_bytesDone.fetch_add((uint64_t)got);
    }
    {
        int flushFailed = fflush(out) != 0 || ferror(out);
        int closeFailed = fclose(out) != 0;
        if (flushFailed || closeFailed) {
        mp6_remove_utf8(destPath.c_str());
        fail("write error at %s (flush/close failed)", destPath.c_str());
        return false;
        }
    }
    return true;
}

bool write_dest_blob(const std::string &destPath, const void *data, size_t size)
{
    if (!ensure_parent_dirs(destPath)) {
        fail("could not create directory for %s", destPath.c_str());
        return false;
    }
    FILE *out = mp6_fopen_utf8(destPath.c_str(), "wb");
    if (out == nullptr) {
        fail("could not create %s (check free space and permissions)", destPath.c_str());
        return false;
    }
    size_t wrote = fwrite(data, 1, size, out);
    {
        int flushFailed = fflush(out) != 0 || ferror(out);
        int closeFailed = fclose(out) != 0;
        if (wrote != size || flushFailed || closeFailed) {
        mp6_remove_utf8(destPath.c_str());
        fail("write error at %s (out of space?)", destPath.c_str());
        return false;
        }
    }
    return true;
}

bool publish_incoming(const std::string &incoming, const std::string &dest)
{
    std::string previous = dest + kPreviousSuffix;
    std::string marker = incoming + "/" + kCompleteMarker;
    char err[512];
    static const unsigned char complete[] = "MP6-CONTENT-1\n";
    bool hadDest = path_exists(dest);

    if (!mp6_dvd_validate_disc_root(incoming.c_str(), err, sizeof(err))) {
        fail("imported content failed validation: %s", err);
        return false;
    }
    if (!write_dest_blob(marker, complete, sizeof(complete) - 1)) return false;
    if (!remove_tree(previous)) {
        fail("could not remove stale content backup %s", previous.c_str());
        return false;
    }
    if (hadDest && !rename_path_checked(dest, previous)) {
        fail("could not preserve existing content before publish: %s", SDL_GetError());
        return false;
    }
    if (!rename_path_checked(incoming, dest)) {
        char renameError[256];
        snprintf(renameError, sizeof(renameError), "%s", SDL_GetError());
        if (hadDest && !rename_path_checked(previous, dest)) {
            fprintf(stderr, "[CONTENT] FATAL: publish rollback failed: %s\n", SDL_GetError());
        }
        fail("could not publish imported content: %s", renameError);
        return false;
    }
    (void)remove_path_checked(dest + "/" + kCompleteMarker);
    if (hadDest && !remove_tree(previous)) {
        fprintf(stderr, "[CONTENT] warning: new content is active but old backup cleanup failed: %s\n",
                previous.c_str());
    }
    return true;
}

/* =======================================================================
 * 4. Game-ID validation. GP6E01 = Mario Party 6 (USA) -- the only
 * supported image; the port's data/endianness/save contracts are all
 * verified against it. Region siblings get a precise message.
 * ======================================================================= */

bool validate_game_id(const char id[6])
{
    if (memcmp(id, "GP6E01", 6) == 0) return true;
    char shown[7];
    memcpy(shown, id, 6);
    shown[6] = '\0';
    for (int i = 0; i < 6; i++) {
        if ((unsigned char)shown[i] < 0x20 || (unsigned char)shown[i] > 0x7E) shown[i] = '?';
    }
    if (memcmp(id, "GP6", 3) == 0) {
        fail("wrong region: this disc is %s -- the port needs the USA release (GP6E01)", shown);
    } else {
        fail("not Mario Party 6: game ID %s (need GP6E01, Mario Party 6 USA)", shown);
    }
    return false;
}

/* =======================================================================
 * 5. Disc-image source (nod over an SDL_IOStream -- partyboard's shim
 * shape; the stream is opened on the CALLER's thread so the one JNI-using
 * step of a content:// open never happens on the worker).
 * ======================================================================= */

int64_t stream_read_at(void *ud, uint64_t offset, void *out, size_t len)
{
    SdlIoOwner *owner = (SdlIoOwner *)ud;
    SDL_IOStream *io = owner != nullptr ? owner->value : nullptr;
    if (io == nullptr) return -1;
    if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0) return -1;
    size_t total = 0;
    uint8_t *dst = (uint8_t *)out;
    while (total < len) {
        size_t got = SDL_ReadIO(io, dst + total, len - total);
        if (got == 0) break; /* EOF or error; partial read is reported below */
        total += got;
    }
    return (int64_t)total;
}

int64_t stream_len(void *ud)
{
    SdlIoOwner *owner = (SdlIoOwner *)ud;
    return owner != nullptr && owner->value != nullptr
        ? (int64_t)SDL_GetIOSize(owner->value) : -1;
}

void stream_close(void *ud)
{
    SdlIoOwner *owner = (SdlIoOwner *)ud;
    if (owner != nullptr) owner->close();
}

struct FstFile {
    uint32_t index;
    uint32_t size;
    std::string rel; /* files-root-relative path */
};

struct FstWalk {
    /* Directory stack: (endIndex, pathPrefix). GC FST semantics: a
     * directory node's size is its child-end index.
     *
     * NOD CONTRACT: nod_partition_iterate_fst() NEVER delivers the root
     * node -- its loop starts at node index 1 (nod-ffi/src/lib.rs:680,
     * `let mut idx: usize = 1; // skip root node`; the safe-Rust Fst::iter()
     * at nod/src/disc/fst.rs:157 starts at 1 as well). The root frame is
     * therefore seeded by seed_root() below from the SAME raw fst.bin nod
     * parses (Fst::new reads the node count from the root node's length
     * field, fst.rs:141-152 -- byte offset 8 of entry 0, exactly what
     * mp6_content_fst_validate() has already range-checked). With the root
     * frame present the "stack is empty" case is a real integrity failure
     * again -- an index outside the root subtree -- and matches the runtime
     * validator's own `depth == 0` rejection. */
    std::vector<std::pair<uint32_t, std::string>> dirs;
    std::vector<FstFile> files;
    unsigned int found = 0;
    bool ok = true;
    char error[256] {};

    /* nodeCount == the root directory's child-end index: every valid node
     * index nod can hand us is 1..nodeCount-1. */
    void seed_root(uint32_t nodeCount) { dirs.push_back({ nodeCount, std::string() }); }
};

uint32_t fst_callback(uint32_t index, enum NodNodeKind kind, const char *name,
                      uint32_t size, void *ud) noexcept
{
    FstWalk *walk = (FstWalk *)ud;
    if (!walk->ok) return NOD_FST_STOP;
    try {
    while (!walk->dirs.empty() && index >= walk->dirs.back().first) {
        walk->dirs.pop_back();
    }
    std::string prefix = walk->dirs.empty() ? std::string() : walk->dirs.back().second;
    /* index 0 is the root, which nod never emits (see FstWalk); an empty
     * stack means the walk left the root subtree. Either is a contract
     * violation, not a shape this importer should guess its way through. */
    if (index == 0 || walk->dirs.empty() || name == nullptr) {
        walk->ok = false;
        snprintf(walk->error, sizeof(walk->error), "nod returned an invalid FST traversal at entry %u", index);
        return NOD_FST_STOP;
    }
    std::string rel = prefix.empty() ? std::string(name ? name : "") : prefix + "/" + (name ? name : "");
    if (kind == NOD_NODE_KIND_DIRECTORY) {
        /* SECURITY: the name came straight from the disc's FST string table.
         * A crafted directory named "..", or one carrying a separator/drive
         * qualifier, would steer the path prefix (and every child written
         * under it) outside the extraction root -- prune the whole subtree. */
        if (!mp6_content_path_is_safe_rel(rel.c_str())) {
            printf("[CONTENT] skipping unsafe disc directory entry: %s\n", rel.c_str());
            fflush(stdout);
            return size; /* child-end index: skip the subtree */
        }
        /* Prune subtrees we can never want (movie/, dll/, ...) so a full
         * FST walk stays cheap; wanted_file() remains the single filter
         * authority for files. */
        bool interesting = wanted_file(rel.c_str()) || path_has_prefix("data", rel.c_str()) ||
                           path_has_prefix("mess", rel.c_str()) || path_has_prefix("mic", rel.c_str()) ||
                           path_has_prefix("sound", rel.c_str());
        if (!interesting) {
            return size; /* child-end index: skip the subtree */
        }
        walk->dirs.push_back({ size, rel });
        return index + 1;
    }
    if (wanted_file(rel.c_str())) {
        /* SECURITY: reject a traversal/absolute/drive-qualified FST file
         * name before it becomes a write destination (dest/files/<rel>). */
        if (!mp6_content_path_is_safe_rel(rel.c_str())) {
            printf("[CONTENT] skipping unsafe disc file entry: %s\n", rel.c_str());
            fflush(stdout);
            return index + 1;
        }
        walk->files.push_back({ index, size, rel });
        if (size > 0) {
            if (rel == "opening.bnr") walk->found |= 1u << 0;
            if (rel == "sound/MP6_SND.msm") walk->found |= 1u << 1;
            if (rel == "sound/MP6_Str.pdt") walk->found |= 1u << 2;
            if (path_has_prefix(rel.c_str(), "data") && rel != "data") walk->found |= 1u << 3;
            if (path_has_prefix(rel.c_str(), "mess") && rel != "mess") walk->found |= 1u << 4;
            if (path_has_prefix(rel.c_str(), "mic") && rel != "mic") walk->found |= 1u << 5;
        }
    }
    return index + 1;
    } catch (const std::exception &e) {
        walk->ok = false;
        snprintf(walk->error, sizeof(walk->error), "FST callback allocation failed: %s", e.what());
        return NOD_FST_STOP;
    } catch (...) {
        walk->ok = false;
        snprintf(walk->error, sizeof(walk->error), "FST callback failed");
        return NOD_FST_STOP;
    }
}

void import_image_worker(std::string source, std::string dest, SDL_IOStream *io)
{
    std::string incoming;
    try {
    SdlIoOwner sourceIo(io);
    NodDiscStream shim {};
    /* nod's FfiDiscStream calls close on both successful-handle teardown and
     * parse failure.  Point it at the idempotent owner itself: whichever side
     * closes first nulls the handle, so malformed images cannot double-close
     * SDL_IOStream and an early FFI argument failure still closes at scope
     * exit.  sourceIo outlives discOwner by declaration order. */
    shim.user_data = &sourceIo;
    shim.read_at = &stream_read_at;
    shim.stream_len = &stream_len;
    shim.close = &stream_close;

    NodHandle *disc = nullptr;
    if (nod_disc_open_stream(&shim, nullptr, &disc) != NOD_RESULT_OK || disc == nullptr) {
        const char *msg = nod_error_message();
        fail("could not read the disc image (%s)", msg ? msg : "unrecognized format");
        return;
    }
    NodOwner discOwner(disc);

    NodDiscHeader header {};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK) {
        fail("could not read the disc header (%s)", nod_error_message());
        return;
    }
    if (!validate_game_id(header.game_id)) {
        return;
    }

    NodHandle *part = nullptr;
    if (nod_disc_open_partition(disc, 0, nullptr, &part) != NOD_RESULT_OK || part == nullptr) {
        fail("could not open the disc's data partition (%s)", nod_error_message());
        return;
    }
    NodOwner partOwner(part);

    NodPartitionMeta meta {};
    if (nod_partition_meta(part, &meta) != NOD_RESULT_OK || meta.raw_fst.data == nullptr ||
        meta.raw_boot.data == nullptr) {
        fail("could not read the disc's file system table (%s)", nod_error_message());
        return;
    }
    if (!mp6_content_boot_validate(meta.raw_boot.data, meta.raw_boot.size)) {
        fail("the disc's sys/boot.bin is not exactly 0x440 bytes of GP6E01 metadata%s", "");
        return;
    }
    {
        char fstError[256];
        if (!mp6_content_fst_validate(meta.raw_fst.data, meta.raw_fst.size,
                                      fstError, sizeof(fstError))) {
            fail("the disc contains an invalid file system table: %s", fstError);
            return;
        }
    }

    FstWalk walk;
    /* Seed the root frame nod's iteration skips (see FstWalk). Reading the
     * count here rather than trusting a callback is what mp6_content_fst_validate
     * just made safe: it proved entry 0 is the canonical root directory with
     * parent 0 and that this count fits inside raw_fst -- the identical value
     * nod's own Fst::new() sizes its node slice with. */
    walk.seed_root(mp6_content_fst_be32((const unsigned char *)meta.raw_fst.data + 8));
    nod_partition_iterate_fst(part, &fst_callback, &walk);
    if (!walk.ok) {
        fail("could not enumerate the disc's file table: %s", walk.error);
        return;
    }
    if (walk.files.empty()) {
        fail("the disc's file table has none of the expected Mario Party 6 files%s", "");
        return;
    }

    uint64_t total = 0;
    for (const FstFile &f : walk.files) {
        if ((uint64_t)f.size > MP6_CONTENT_WANTED_MAX_BYTES - total) {
            fail("the disc's wanted-file manifest exceeds the 1.5 GiB safety limit%s", "");
            return;
        }
        total += f.size;
    }
    total += meta.raw_boot.size + meta.raw_fst.size;
    g_bytesTotal.store(total);
    g_filesTotal.store((int)walk.files.size() + 2 /* boot.bin + fst.bin */);

    if (!prepare_incoming(dest, incoming)) {
        return;
    }
    if ((walk.found & 0x3fu) != 0x3fu) {
        fail("the disc's wanted-file manifest is incomplete or has zero-byte sentinels%s", "");
        return;
    }

    for (const FstFile &f : walk.files) {
        if (g_cancel.load()) break;
        set_current_file(f.rel.c_str());
        NodHandle *fh = nullptr;
        if (nod_partition_open_file(part, f.index, &fh) != NOD_RESULT_OK || fh == nullptr) {
            fail("could not open %s in the image (%s)", f.rel.c_str(), nod_error_message());
            break;
        }
        NodOwner fileOwner(fh);
        bool ok = write_dest_file(incoming + "/files/" + f.rel, f.rel.c_str(), f.size,
                                  [fh](uint8_t *buf, size_t want) { return nod_read(fh, buf, want); });
        if (!ok) break;
        g_filesDone.fetch_add(1);
    }

    /* sys/ last, fst.bin very last (the torn-import contract). */
    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        set_current_file("sys/boot.bin");
        if (write_dest_blob(incoming + "/sys/boot.bin", meta.raw_boot.data, meta.raw_boot.size)) {
            g_bytesDone.fetch_add(meta.raw_boot.size);
            g_filesDone.fetch_add(1);
            set_current_file("sys/fst.bin");
            if (write_dest_blob(incoming + "/sys/fst.bin", meta.raw_fst.data, meta.raw_fst.size)) {
                g_bytesDone.fetch_add(meta.raw_fst.size);
                g_filesDone.fetch_add(1);
            }
        }
    }

    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        if (publish_incoming(incoming, dest)) {
            g_state.store(MP6_IMPORT_DONE);
        } else {
            cleanup_failed_incoming(incoming, dest);
        }
    } else {
        cleanup_failed_incoming(incoming, dest);
        if (g_state.load() == MP6_IMPORT_RUNNING) g_state.store(MP6_IMPORT_CANCELLED);
    }
    } catch (const std::exception &e) {
        try { cleanup_failed_incoming(incoming, dest); } catch (...) {}
        fail("unexpected disc-image import failure: %s", e.what());
    } catch (...) {
        try { cleanup_failed_incoming(incoming, dest); } catch (...) {}
        fail("unexpected disc-image import failure%s", "");
    }
}

/* =======================================================================
 * 6. Extracted-folder source (plain stream copy of the same wanted set;
 * fst.bin still written last). Windows/dev-path only -- Android SAF trees
 * are imported by the Java side (no filesystem path exists for them).
 * ======================================================================= */

/* Resolves where the actual disc root is under a user-picked folder:
 * the folder itself, <folder>/GP6E01, or a Dolphin-style DATA/ child. */
bool folder_disc_root(const std::string &picked, std::string &out)
{
    const char *candidates[] = { "", "/GP6E01", "/DATA" };
    for (const char *c : candidates) {
        std::string root = picked + c;
        SDL_PathInfo info {};
        if (SDL_GetPathInfo((root + "/sys/fst.bin").c_str(), &info) && info.type == SDL_PATHTYPE_FILE &&
            SDL_GetPathInfo((root + "/files").c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            out = root;
            return true;
        }
    }
    return false;
}

struct FolderFile {
    std::string rel;
    uint64_t size;
};

bool folder_fst_manifest(const std::string &src, std::vector<FolderFile> &out)
{
    std::string path = src + "/sys/fst.bin";
    SDL_PathInfo info {};
    if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE ||
        info.size < MP6_CONTENT_FST_ENTRY_SIZE || info.size > MP6_CONTENT_FST_MAX_BYTES) {
        fail("missing or implausible FST at %s", path.c_str());
        return false;
    }
    std::vector<unsigned char> fst((size_t)info.size);
    FILE *f = mp6_fopen_utf8(path.c_str(), "rb");
    if (f == nullptr) {
        fail("could not open %s", path.c_str());
        return false;
    }
    size_t got = fread(fst.data(), 1, fst.size(), f);
    int extra = fgetc(f);
    int readFailed = ferror(f);
    int closeFailed = fclose(f) != 0;
    if (got != fst.size() || extra != EOF || readFailed || closeFailed) {
        fail("source changed or ended early while reading %s", path.c_str());
        return false;
    }
    char validationError[256];
    if (!mp6_content_fst_validate(fst.data(), fst.size(),
                                  validationError, sizeof(validationError))) {
        fail("invalid source FST: %s", validationError);
        return false;
    }

    uint32_t count = mp6_content_fst_be32(fst.data() + 8);
    const char *strings = (const char *)(fst.data() + (size_t)count * MP6_CONTENT_FST_ENTRY_SIZE);
    std::vector<std::pair<uint32_t, std::string>> dirs;
    dirs.push_back({ count, std::string() });
    for (uint32_t i = 1; i < count; ++i) {
        const unsigned char *entry = fst.data() + (size_t)i * MP6_CONTENT_FST_ENTRY_SIZE;
        while (i >= dirs.back().first) dirs.pop_back();
        const char *name = strings + (mp6_content_fst_be32(entry) & 0x00FFFFFFu);
        std::string rel = dirs.back().second.empty() ? std::string(name)
                                                     : dirs.back().second + "/" + name;
        if ((mp6_content_fst_be32(entry) >> 24) == 1u) {
            dirs.push_back({ mp6_content_fst_be32(entry + 8), rel });
        } else if (wanted_file(rel.c_str())) {
            out.push_back({ rel, (uint64_t)mp6_content_fst_be32(entry + 8) });
        }
    }
    return true;
}

void import_folder_worker(std::string picked, std::string dest)
{
    std::string src;
    std::string incoming;
    try {
    char sourceError[512];
    if (!folder_disc_root(picked, src)) {
        fail("%s doesn't look like an extracted GameCube disc (need sys/fst.bin + files/ inside it, "
             "or a GP6E01 folder containing them)", picked.c_str());
        return;
    }

    if (!mp6_dvd_validate_disc_root(src.c_str(), sourceError, sizeof(sourceError))) {
        fail("the selected folder is not a complete GP6E01 content tree: %s", sourceError);
        return;
    }

    std::vector<FolderFile> files;
    if (!folder_fst_manifest(src, files)) return;
    if (files.empty()) {
        fail("no Mario Party 6 files found under %s/files", src.c_str());
        return;
    }

    SDL_PathInfo fstInfo {};
    SDL_GetPathInfo((src + "/sys/fst.bin").c_str(), &fstInfo);
    SDL_PathInfo bootInfo {};
    bool haveBoot = SDL_GetPathInfo((src + "/sys/boot.bin").c_str(), &bootInfo) &&
                    bootInfo.type == SDL_PATHTYPE_FILE;
    if (!haveBoot || bootInfo.size != MP6_CONTENT_BOOT_BYTES) {
        fail("the selected folder's sys/boot.bin must be exactly 0x440 bytes%s", "");
        return;
    }

    uint64_t wantedBytes = 0;
    for (const FolderFile &f : files) {
        if (f.size > MP6_CONTENT_WANTED_MAX_BYTES - wantedBytes) {
            fail("the selected folder's wanted-file manifest exceeds the 1.5 GiB safety limit%s", "");
            return;
        }
        wantedBytes += f.size;
    }
    uint64_t total = (uint64_t)fstInfo.size + (haveBoot ? (uint64_t)bootInfo.size : 0) + wantedBytes;
    g_bytesTotal.store(total);
    g_filesTotal.store((int)files.size() + 1 + (haveBoot ? 1 : 0));

    if (!prepare_incoming(dest, incoming)) return;

    auto copy_one = [&](const std::string &srcPath, const std::string &destPath, const char *rel,
                        uint64_t size) -> bool {
        if (!operational_path_supported(srcPath)) {
            fail("source path is too long or unsupported: %s", srcPath.c_str());
            return false;
        }
        SDL_IOStream *in = SDL_IOFromFile(srcPath.c_str(), "rb");
        if (in == nullptr) {
            fail("could not open %s", srcPath.c_str());
            return false;
        }
        SdlIoOwner inOwner(in);
        bool ok = write_dest_file(destPath, rel, size,
                                  [in](uint8_t *buf, size_t want) -> int64_t {
                                      size_t got = SDL_ReadIO(in, buf, want);
                                      return got == 0 ? -1 : (int64_t)got;
                                  });
        return ok;
    };

    for (const FolderFile &f : files) {
        if (g_cancel.load()) break;
        set_current_file(f.rel.c_str());
        if (!copy_one(src + "/files/" + f.rel, incoming + "/files/" + f.rel, f.rel.c_str(), f.size)) break;
        g_filesDone.fetch_add(1);
    }

    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        bool ok = true;
        if (haveBoot) {
            set_current_file("sys/boot.bin");
            ok = copy_one(src + "/sys/boot.bin", incoming + "/sys/boot.bin", "sys/boot.bin",
                          (uint64_t)bootInfo.size);
            if (ok) g_filesDone.fetch_add(1);
        }
        if (ok) {
            set_current_file("sys/fst.bin");
            if (copy_one(src + "/sys/fst.bin", incoming + "/sys/fst.bin", "sys/fst.bin",
                         (uint64_t)fstInfo.size)) {
                g_filesDone.fetch_add(1);
            }
        }
    }

    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        if (publish_incoming(incoming, dest)) {
            g_state.store(MP6_IMPORT_DONE);
        } else {
            cleanup_failed_incoming(incoming, dest);
        }
    } else {
        cleanup_failed_incoming(incoming, dest);
        if (g_state.load() == MP6_IMPORT_RUNNING) g_state.store(MP6_IMPORT_CANCELLED);
    }
    } catch (const std::exception &e) {
        try { cleanup_failed_incoming(incoming, dest); } catch (...) {}
        fail("unexpected extracted-folder import failure: %s", e.what());
    } catch (...) {
        try { cleanup_failed_incoming(incoming, dest); } catch (...) {}
        fail("unexpected extracted-folder import failure%s", "");
    }
}

/* =======================================================================
 * 7. Thread lifecycle.
 * ======================================================================= */

bool begin_import()
{
    int expected = MP6_IMPORT_IDLE;
    if (!g_state.compare_exchange_strong(expected, MP6_IMPORT_RUNNING)) {
        return false;
    }
    g_bytesDone.store(0);
    g_bytesTotal.store(0);
    g_filesDone.store(0);
    g_filesTotal.store(0);
    g_cancel.store(false);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_currentFile[0] = '\0';
    g_error[0] = '\0';
    return true;
}

} // namespace

extern "C" int mp6_import_start_image(const char *source, const char *destDiscRoot)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (source == nullptr || source[0] == '\0' || !mp6_utf8_path_supported(source) ||
        !transaction_paths_supported(destDiscRoot) || !begin_import()) return -1;

    /* Open on the caller's thread: for a content:// URI this is the one
     * step that crosses into Java (ContentResolver.openFileDescriptor via
     * SDL); every later read on the returned stream is a plain fd read. */
    SDL_IOStream *io = SDL_IOFromFile(source, "rb");
    if (io == nullptr) {
        fail("could not open %s (%s)", source, SDL_GetError());
        return 0; /* state=FAILED is the report channel, matching the poll contract */
    }

    printf("[CONTENT] import (disc image) starting: %s -> %s\n", source, destDiscRoot);
    fflush(stdout);
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_thread = std::thread(import_image_worker, std::string(source), std::string(destDiscRoot), io);
    } catch (...) {
        SDL_CloseIO(io);
        fprintf(stderr, "[CONTENT] could not start the content import worker\n");
        fflush(stderr);
        g_state.store(MP6_IMPORT_IDLE);
        return -1;
    }
    return 0;
}

extern "C" int mp6_import_start_folder(const char *sourceRoot, const char *destDiscRoot)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (sourceRoot == nullptr || sourceRoot[0] == '\0' ||
        !mp6_utf8_path_supported(sourceRoot) ||
        !transaction_paths_supported(destDiscRoot) || !begin_import()) return -1;
    printf("[CONTENT] import (extracted folder) starting: %s -> %s\n", sourceRoot, destDiscRoot);
    fflush(stdout);
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_thread = std::thread(import_folder_worker, std::string(sourceRoot), std::string(destDiscRoot));
    } catch (...) {
        fprintf(stderr, "[CONTENT] could not start the content import worker\n");
        fflush(stderr);
        g_state.store(MP6_IMPORT_IDLE);
        return -1;
    }
    return 0;
}

extern "C" void mp6_import_poll(Mp6ImportStatus *out)
{
    if (out == nullptr) return;
    out->state = g_state.load();
    out->bytesDone = g_bytesDone.load();
    out->bytesTotal = g_bytesTotal.load();
    out->filesDone = g_filesDone.load();
    out->filesTotal = g_filesTotal.load();
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(out->currentFile, sizeof(out->currentFile), "%s", g_currentFile);
    snprintf(out->error, sizeof(out->error), "%s", g_error);
}

extern "C" void mp6_import_cancel(void)
{
    g_cancel.store(true);
}

extern "C" void mp6_import_reset(void)
{
    std::thread worker;
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (g_state.load() == MP6_IMPORT_RUNNING) {
        g_cancel.store(true);
    }
    /* Never hold g_mutex across join: the worker may be between chunks and
     * about to acquire it in set_current_file()/fail(). Move the handle out,
     * join without the lock, then clear the observable state. */
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_thread.joinable()) worker = std::move(g_thread);
    }
    if (worker.joinable()) worker.join();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_currentFile[0] = '\0';
        g_error[0] = '\0';
    }
    g_cancel.store(false);
    g_state.store(MP6_IMPORT_IDLE);
}

extern "C" int mp6_import_recover_disc_root(const char *destDiscRoot)
{
    if (!transaction_paths_supported(destDiscRoot)) return 0;
    try {
        return recover_disc_root(std::string(destDiscRoot));
    } catch (...) {
        return 0;
    }
}
