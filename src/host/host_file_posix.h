/* MP6 native port -- the POSIX read-only content file (host.h,
 * mp6_host_file_*): one descriptor, positional pread, no seek state.
 * Shared by host_android.c and host_emscripten.c (whose web content
 * provider handles the paths it serves before falling back to this). */
#ifndef MP6_HOST_FILE_POSIX_H
#define MP6_HOST_FILE_POSIX_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline int mp6_posix_file_open(const char *path)
{
    int fd;
    if (path == NULL) return -1;
    do {
        fd = open(path, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static inline int64_t mp6_posix_file_size(int fd)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

static inline int64_t mp6_posix_file_read_at(int fd, void *dst, size_t len, uint64_t offset)
{
    size_t done = 0;
    if (fd < 0 || (dst == NULL && len > 0)) return -1;
    while (done < len) {
        ssize_t got = pread(fd, (char *)dst + done, len - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) break;
        done += (size_t)got;
    }
    return (int64_t)done;
}

#endif /* MP6_HOST_FILE_POSIX_H */
