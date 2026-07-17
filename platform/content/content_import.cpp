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

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <nod.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* =======================================================================
 * 1. Shared progress state (one import at a time by design).
 * ======================================================================= */

namespace {

std::mutex g_mutex;             /* guards the string fields + thread handle */
std::thread g_thread;
std::atomic<int> g_state { MP6_IMPORT_IDLE };
std::atomic<uint64_t> g_bytesDone { 0 };
std::atomic<uint64_t> g_bytesTotal { 0 };
std::atomic<int> g_filesDone { 0 };
std::atomic<int> g_filesTotal { 0 };
std::atomic<bool> g_cancel { false };
char g_currentFile[260];
char g_error[512];

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

bool ensure_parent_dirs(const std::string &filePath)
{
    size_t slash = filePath.find_last_of('/');
    if (slash == std::string::npos) return true;
    std::string dir = filePath.substr(0, slash);
    return SDL_CreateDirectory(dir.c_str());
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
    FILE *out = fopen(destPath.c_str(), "wb");
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
            remove(destPath.c_str());
            return false;
        }
        size_t want = (size_t)(left < buf.size() ? left : buf.size());
        int64_t got = read(buf.data(), want);
        if (got <= 0) {
            fclose(out);
            fail("read error in %s (source truncated or unreadable)", relName);
            return false;
        }
        if (fwrite(buf.data(), 1, (size_t)got, out) != (size_t)got) {
            fclose(out);
            fail("write error at %s (out of space?)", destPath.c_str());
            return false;
        }
        left -= (uint64_t)got;
        g_bytesDone.fetch_add((uint64_t)got);
    }
    fclose(out);
    return true;
}

bool write_dest_blob(const std::string &destPath, const void *data, size_t size)
{
    if (!ensure_parent_dirs(destPath)) {
        fail("could not create directory for %s", destPath.c_str());
        return false;
    }
    FILE *out = fopen(destPath.c_str(), "wb");
    if (out == nullptr) {
        fail("could not create %s (check free space and permissions)", destPath.c_str());
        return false;
    }
    size_t wrote = fwrite(data, 1, size, out);
    fclose(out);
    if (wrote != size) {
        fail("write error at %s (out of space?)", destPath.c_str());
        return false;
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
    SDL_IOStream *io = (SDL_IOStream *)ud;
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
    return (int64_t)SDL_GetIOSize((SDL_IOStream *)ud);
}

void stream_close(void *ud)
{
    SDL_CloseIO((SDL_IOStream *)ud);
}

struct FstFile {
    uint32_t index;
    uint32_t size;
    std::string rel; /* files-root-relative path */
};

struct FstWalk {
    /* Directory stack: (endIndex, pathPrefix). GC FST semantics: a
     * directory node's size is its child-end index. */
    std::vector<std::pair<uint32_t, std::string>> dirs;
    std::vector<FstFile> files;
};

uint32_t fst_callback(uint32_t index, enum NodNodeKind kind, const char *name, uint32_t size, void *ud)
{
    FstWalk *walk = (FstWalk *)ud;
    while (!walk->dirs.empty() && index >= walk->dirs.back().first) {
        walk->dirs.pop_back();
    }
    std::string prefix = walk->dirs.empty() ? std::string() : walk->dirs.back().second;
    if (index == 0 && kind == NOD_NODE_KIND_DIRECTORY) {
        /* root node: keep prefix empty */
        walk->dirs.push_back({ size, std::string() });
        return index + 1;
    }
    std::string rel = prefix.empty() ? std::string(name ? name : "") : prefix + "/" + (name ? name : "");
    if (kind == NOD_NODE_KIND_DIRECTORY) {
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
        walk->files.push_back({ index, size, rel });
    }
    return index + 1;
}

void import_image_worker(std::string source, std::string dest, SDL_IOStream *io)
{
    NodDiscStream shim {};
    shim.user_data = io;
    shim.read_at = &stream_read_at;
    shim.stream_len = &stream_len;
    shim.close = &stream_close;

    NodHandle *disc = nullptr;
    if (nod_disc_open_stream(&shim, nullptr, &disc) != NOD_RESULT_OK || disc == nullptr) {
        const char *msg = nod_error_message();
        /* shim.close ownership passed to nod only on success; close here. */
        SDL_CloseIO(io);
        fail("could not read the disc image (%s)", msg ? msg : "unrecognized format");
        return;
    }

    NodDiscHeader header {};
    if (nod_disc_header(disc, &header) != NOD_RESULT_OK) {
        nod_free(disc);
        fail("could not read the disc header (%s)", nod_error_message());
        return;
    }
    if (!validate_game_id(header.game_id)) {
        nod_free(disc);
        return;
    }

    NodHandle *part = nullptr;
    if (nod_disc_open_partition(disc, 0, nullptr, &part) != NOD_RESULT_OK || part == nullptr) {
        nod_free(disc);
        fail("could not open the disc's data partition (%s)", nod_error_message());
        return;
    }

    NodPartitionMeta meta {};
    if (nod_partition_meta(part, &meta) != NOD_RESULT_OK || meta.raw_fst.data == nullptr ||
        meta.raw_boot.data == nullptr) {
        nod_free(part);
        nod_free(disc);
        fail("could not read the disc's file system table (%s)", nod_error_message());
        return;
    }

    FstWalk walk;
    nod_partition_iterate_fst(part, &fst_callback, &walk);
    if (walk.files.empty()) {
        nod_free(part);
        nod_free(disc);
        fail("the disc's file table has none of the expected Mario Party 6 files%s", "");
        return;
    }

    uint64_t total = 0;
    for (const FstFile &f : walk.files) total += f.size;
    total += meta.raw_boot.size + meta.raw_fst.size;
    g_bytesTotal.store(total);
    g_filesTotal.store((int)walk.files.size() + 2 /* boot.bin + fst.bin */);

    for (const FstFile &f : walk.files) {
        if (g_cancel.load()) break;
        set_current_file(f.rel.c_str());
        NodHandle *fh = nullptr;
        if (nod_partition_open_file(part, f.index, &fh) != NOD_RESULT_OK || fh == nullptr) {
            fail("could not open %s in the image (%s)", f.rel.c_str(), nod_error_message());
            break;
        }
        bool ok = write_dest_file(dest + "/files/" + f.rel, f.rel.c_str(), f.size,
                                  [fh](uint8_t *buf, size_t want) { return nod_read(fh, buf, want); });
        nod_free(fh);
        if (!ok) break;
        g_filesDone.fetch_add(1);
    }

    /* sys/ last, fst.bin very last (the torn-import contract). */
    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        set_current_file("sys/boot.bin");
        if (write_dest_blob(dest + "/sys/boot.bin", meta.raw_boot.data, meta.raw_boot.size)) {
            g_bytesDone.fetch_add(meta.raw_boot.size);
            g_filesDone.fetch_add(1);
            set_current_file("sys/fst.bin");
            if (write_dest_blob(dest + "/sys/fst.bin", meta.raw_fst.data, meta.raw_fst.size)) {
                g_bytesDone.fetch_add(meta.raw_fst.size);
                g_filesDone.fetch_add(1);
            }
        }
    }

    nod_free(part);
    nod_free(disc); /* frees the stream via shim.close */

    if (g_state.load() == MP6_IMPORT_RUNNING) {
        g_state.store(g_cancel.load() ? MP6_IMPORT_CANCELLED : MP6_IMPORT_DONE);
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

void folder_collect(const std::string &filesRoot, const std::string &relDir, std::vector<FolderFile> &out)
{
    struct Ctx {
        const std::string *filesRoot;
        const std::string *relDir;
        std::vector<FolderFile> *out;
        std::vector<std::string> subdirs;
    } ctx { &filesRoot, &relDir, &out, {} };

    std::string abs = relDir.empty() ? filesRoot : filesRoot + "/" + relDir;
    SDL_EnumerateDirectory(
        abs.c_str(),
        [](void *ud, const char *dirname, const char *fname) -> SDL_EnumerationResult {
            Ctx *c = (Ctx *)ud;
            (void)dirname;
            std::string rel = c->relDir->empty() ? std::string(fname) : *c->relDir + "/" + fname;
            std::string full = *c->filesRoot + "/" + rel;
            SDL_PathInfo info {};
            if (!SDL_GetPathInfo(full.c_str(), &info)) return SDL_ENUM_CONTINUE;
            if (info.type == SDL_PATHTYPE_DIRECTORY) {
                c->subdirs.push_back(rel);
            } else if (info.type == SDL_PATHTYPE_FILE && wanted_file(rel.c_str())) {
                c->out->push_back({ rel, (uint64_t)info.size });
            }
            return SDL_ENUM_CONTINUE;
        },
        &ctx);
    for (const std::string &sub : ctx.subdirs) {
        /* Only descend into trees that can still contain wanted files. */
        if (path_has_prefix(sub.c_str(), "data") || path_has_prefix(sub.c_str(), "mess") ||
            path_has_prefix(sub.c_str(), "mic") || path_has_prefix(sub.c_str(), "sound")) {
            folder_collect(filesRoot, sub, out);
        }
    }
}

void import_folder_worker(std::string picked, std::string dest)
{
    std::string src;
    if (!folder_disc_root(picked, src)) {
        fail("%s doesn't look like an extracted GameCube disc (need sys/fst.bin + files/ inside it, "
             "or a GP6E01 folder containing them)", picked.c_str());
        return;
    }

    /* Game-ID check from sys/boot.bin (always present in a real extraction;
     * offset 0 is the game ID exactly like a disc image). */
    {
        FILE *boot = fopen((src + "/sys/boot.bin").c_str(), "rb");
        if (boot != nullptr) {
            char id[6] = { 0 };
            size_t got = fread(id, 1, 6, boot);
            fclose(boot);
            if (got == 6 && !validate_game_id(id)) return;
        }
        /* No boot.bin: tolerated (some hand-assembled trees) -- fst.bin +
         * files/ shape was already validated; the DVD layer's own honest
         * missing-file reporting covers residual mismatches. */
    }

    std::vector<FolderFile> files;
    folder_collect(src + "/files", std::string(), files);
    if (files.empty()) {
        fail("no Mario Party 6 files found under %s/files", src.c_str());
        return;
    }

    SDL_PathInfo fstInfo {};
    SDL_GetPathInfo((src + "/sys/fst.bin").c_str(), &fstInfo);
    SDL_PathInfo bootInfo {};
    bool haveBoot = SDL_GetPathInfo((src + "/sys/boot.bin").c_str(), &bootInfo) &&
                    bootInfo.type == SDL_PATHTYPE_FILE;

    uint64_t total = (uint64_t)fstInfo.size + (haveBoot ? (uint64_t)bootInfo.size : 0);
    for (const FolderFile &f : files) total += f.size;
    g_bytesTotal.store(total);
    g_filesTotal.store((int)files.size() + 1 + (haveBoot ? 1 : 0));

    auto copy_one = [&](const std::string &srcPath, const std::string &destPath, const char *rel,
                        uint64_t size) -> bool {
        SDL_IOStream *in = SDL_IOFromFile(srcPath.c_str(), "rb");
        if (in == nullptr) {
            fail("could not open %s", srcPath.c_str());
            return false;
        }
        bool ok = write_dest_file(destPath, rel, size,
                                  [in](uint8_t *buf, size_t want) -> int64_t {
                                      size_t got = SDL_ReadIO(in, buf, want);
                                      return got == 0 ? -1 : (int64_t)got;
                                  });
        SDL_CloseIO(in);
        return ok;
    };

    for (const FolderFile &f : files) {
        if (g_cancel.load()) break;
        set_current_file(f.rel.c_str());
        if (!copy_one(src + "/files/" + f.rel, dest + "/files/" + f.rel, f.rel.c_str(), f.size)) break;
        g_filesDone.fetch_add(1);
    }

    if (g_state.load() == MP6_IMPORT_RUNNING && !g_cancel.load()) {
        bool ok = true;
        if (haveBoot) {
            set_current_file("sys/boot.bin");
            ok = copy_one(src + "/sys/boot.bin", dest + "/sys/boot.bin", "sys/boot.bin",
                          (uint64_t)bootInfo.size);
            if (ok) g_filesDone.fetch_add(1);
        }
        if (ok) {
            set_current_file("sys/fst.bin");
            if (copy_one(src + "/sys/fst.bin", dest + "/sys/fst.bin", "sys/fst.bin",
                         (uint64_t)fstInfo.size)) {
                g_filesDone.fetch_add(1);
            }
        }
    }

    if (g_state.load() == MP6_IMPORT_RUNNING) {
        g_state.store(g_cancel.load() ? MP6_IMPORT_CANCELLED : MP6_IMPORT_DONE);
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
    if (source == nullptr || destDiscRoot == nullptr || !begin_import()) return -1;

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
    std::lock_guard<std::mutex> lock(g_mutex);
    g_thread = std::thread(import_image_worker, std::string(source), std::string(destDiscRoot), io);
    return 0;
}

extern "C" int mp6_import_start_folder(const char *sourceRoot, const char *destDiscRoot)
{
    if (sourceRoot == nullptr || destDiscRoot == nullptr || !begin_import()) return -1;
    printf("[CONTENT] import (extracted folder) starting: %s -> %s\n", sourceRoot, destDiscRoot);
    fflush(stdout);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_thread = std::thread(import_folder_worker, std::string(sourceRoot), std::string(destDiscRoot));
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
    int s = g_state.load();
    if (s == MP6_IMPORT_RUNNING || s == MP6_IMPORT_IDLE) {
        if (s == MP6_IMPORT_RUNNING) return; /* must cancel first */
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_thread.joinable()) g_thread.join();
    g_currentFile[0] = '\0';
    g_error[0] = '\0';
    g_state.store(MP6_IMPORT_IDLE);
}
