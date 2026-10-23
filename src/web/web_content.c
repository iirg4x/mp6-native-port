/* MP6 native port -- read-only game content for the browser build.
 *
 * The extracted disc (sys/ + files/, ~750 MB) is too large to copy into
 * Emscripten's in-memory filesystem, and the browser can only read it
 * asynchronously (fetch Range requests from a local server, or Blob slices
 * of files the user picked / stored in OPFS). The disc layer reads through
 * mp6_web_content_open/read_at (host_emscripten.c's mp6_host_file_*), and
 * mp6_fopen_utf8 routes every other read-only open under the disc root
 * here as an fopencookie stream. Either way a read is served synchronously
 * when the page's content provider (Module.mp6Content,
 * packaging/pwa/content.js) already holds its bytes -- it reads ahead --
 * and otherwise awaits the provider via ASYNCIFY.
 *
 * With no provider registered (headless runs under node, which use
 * NODERAWFS), everything falls through to plain fopen. */
#include "mp6_web_content.h"

#include <emscripten.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* Browser-host state, carved out of savestates (include/mp6_host_section.h). */
#include "mp6_host_section.h"

EM_JS(int, mp6_web_content_has, (const char *path), {
    return (Module.mp6Content && Module.mp6Content.handles(UTF8ToString(path))) ? 1 : 0;
});

/* Every provider knows its sizes up front (manifest, Blob.size, disc FST),
 * so this is synchronous: an open costs no await. */
EM_JS(double, mp6_web_content_size, (const char *path), {
    return Module.mp6Content.size(UTF8ToString(path));
});

EM_ASYNC_JS(int, mp6_web_content_read, (const char *path, double offset, char *dst, int len), {
    const bytes = await Module.mp6Content.read(UTF8ToString(path), offset, len);
    HEAPU8.set(bytes, dst); /* HEAPU8 re-read after the await: memory may have grown */
    return bytes.length;
});

/* The same read when the provider already holds every byte of it (fetched
 * ahead), without suspending the program; -1 when it does not. */
EM_JS(int, mp6_web_content_read_cached, (const char *path, double offset, char *dst, int len), {
    const bytes = Module.mp6Content.readCached
        ? Module.mp6Content.readCached(UTF8ToString(path), offset, len) : null;
    if (bytes === null) return -1;
    HEAPU8.set(bytes, dst);
    return bytes.length;
});

typedef struct {
    int64_t size;
    int64_t pos;
    char path[1];
} Mp6WebFile;

static ssize_t mp6_web_read(void *cookie, char *buf, size_t n)
{
    Mp6WebFile *f = (Mp6WebFile *)cookie;
    int got;
    if (f->pos >= f->size || n == 0) return 0;
    if ((int64_t)n > f->size - f->pos) n = (size_t)(f->size - f->pos);
    if (n > 0x40000000u) n = 0x40000000u;
    got = mp6_web_content_read_cached(f->path, (double)f->pos, buf, (int)n);
    if (got < 0) got = mp6_web_content_read(f->path, (double)f->pos, buf, (int)n);
    if (got < 0) {
        errno = EIO;
        return -1;
    }
    f->pos += got;
    return got;
}

static int mp6_web_seek(void *cookie, off_t *offset, int whence)
{
    Mp6WebFile *f = (Mp6WebFile *)cookie;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : f->size;
    int64_t next = base + (int64_t)*offset;
    if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || next < 0) {
        errno = EINVAL;
        return -1;
    }
    f->pos = next;
    *offset = (off_t)next;
    return 0;
}

static int mp6_web_close(void *cookie)
{
    free(cookie);
    return 0;
}

EM_JS(int, mp6_web_content_prefetch_js, (const char *path, double offset, double len), {
    if (!Module.mp6Content || !Module.mp6Content.prefetch) return 0;
    if (path) Module.mp6Content.prefetch(UTF8ToString(path), offset, len);
    return 1;
});

int mp6_web_content_prefetch(const char *path, uint64_t offset, uint64_t len)
{
    if (path != NULL && !mp6_web_content_has(path)) return 0;
    return mp6_web_content_prefetch_js(path, (double)offset, (double)len);
}

int mp6_web_content_open(const char *path, int64_t *sizeOut)
{
    double size;
    if (path == NULL || !mp6_web_content_has(path)) return 0;
    size = mp6_web_content_size(path);
    *sizeOut = size < 0 ? -1 : (int64_t)size;
    return 1;
}

int64_t mp6_web_content_read_cached_at(const char *path, void *dst, size_t len, uint64_t offset)
{
    int got;
    if (dst == NULL || len > 0x40000000u) return -1;
    got = mp6_web_content_read_cached(path, (double)offset, (char *)dst, (int)len);
    return got < 0 ? -1 : (int64_t)got;
}

int64_t mp6_web_content_read_at(const char *path, int64_t size, void *dst, size_t len,
                                uint64_t offset)
{
    size_t done = 0;
    if ((dst == NULL && len > 0) || size < 0) return -1;
    if (offset >= (uint64_t)size) return 0;
    if (len > (uint64_t)size - offset) len = (size_t)((uint64_t)size - offset);
    while (done < len) {
        int want = (len - done) > 0x40000000u ? 0x40000000 : (int)(len - done);
        int got = mp6_web_content_read_cached(path, (double)(offset + done), (char *)dst + done, want);
        if (got < 0) got = mp6_web_content_read(path, (double)(offset + done), (char *)dst + done, want);
        if (got < 0) return -1;
        if (got == 0) break;
        done += (size_t)got;
    }
    return (int64_t)done;
}

FILE *mp6_web_fopen(const char *path, const char *mode)
{
    Mp6WebFile *f;
    double size;
    size_t len;
    FILE *fp;
    cookie_io_functions_t io = { mp6_web_read, NULL, mp6_web_seek, mp6_web_close };

    if (path == NULL || mode == NULL || mode[0] != 'r' || strchr(mode, '+') != NULL ||
        !mp6_web_content_has(path)) {
        return fopen(path, mode);
    }
    size = mp6_web_content_size(path);
    if (size < 0) {
        errno = ENOENT;
        return NULL;
    }
    len = strlen(path);
    f = (Mp6WebFile *)malloc(sizeof(*f) + len);
    if (f == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    f->size = (int64_t)size;
    f->pos = 0;
    memcpy(f->path, path, len + 1);
    fp = fopencookie(f, mode, io);
    if (fp == NULL) free(f);
    return fp;
}
