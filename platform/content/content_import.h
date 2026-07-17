/* MP6 native port -- first-run content import engine.
 *
 * C interface between the launcher UI (platform/gx/ui/content_setup.cpp)
 * and the import worker (platform/content/content_import.cpp). The worker
 * extracts EXACTLY the file set the port needs (a ~401MB subset --
 * sys/fst.bin, files/opening.bnr,
 * files/sound/MP6_SND.msm, files/sound/MP6_Str.pdt, and everything under
 * files/data, files/mess, files/mic; movie/ deliberately deferred, its
 * 336MB is only needed by THP-playing paths and the DVD layer degrades
 * honestly) from either:
 *
 *   - a GameCube disc image (.iso/.gcm/.ciso/.gcz/.rvz/.wia/...), read via
 *     nod (encounter/nod v2 C FFI -- the exact library+version aurora's own
 *     AURORA_ENABLE_DVD row pins; dual-licensed MIT/Apache-2.0). The image
 *     is opened as an SDL_IOStream and served to nod through a NodDiscStream callback
 *     shim -- partyboard's proven iso_validate.cpp shape -- which is what
 *     makes Android SAF content:// URIs work unmodified (SDL_IOFromFile
 *     opens content:// via the ContentResolver fd bridge).
 *
 *   - an already-extracted GP6E01 tree (folder source), plain stream copy
 *     of the same wanted set. Windows folder picks come through the SDL3
 *     folder dialog; Android trees are imported by the Java side instead
 *     (SAF trees have no filesystem path -- see Mp6Activity.java), so this
 *     backend never sees a content:// source.
 *
 * The import runs on a dedicated std::thread (NEVER a game coroutine stack
 * -- the no-JNI-from-coroutine-stack law; this whole engine is pre-boot only).
 * The UI polls mp6_import_poll() once per frame. The destination is always
 * a plain filesystem "disc root" directory that will directly contain
 * sys/ and files/.
 *
 * Torn-import safety: sys/fst.bin -- the single load-bearing probe file
 * for every content check in the port (launcher validate_root,
 * mp6_dvd_probe_root, mp6_android_has_fst) -- is written LAST, so an
 * interrupted import never presents as bootable content.
 */
#ifndef MP6_CONTENT_IMPORT_H
#define MP6_CONTENT_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MP6_IMPORT_IDLE = 0,
    MP6_IMPORT_RUNNING = 1,
    MP6_IMPORT_DONE = 2,
    MP6_IMPORT_FAILED = 3,
    MP6_IMPORT_CANCELLED = 4,
};

typedef struct Mp6ImportStatus {
    int state;              /* MP6_IMPORT_* */
    uint64_t bytesDone;
    uint64_t bytesTotal;    /* 0 while the file list is still being sized */
    int filesDone;
    int filesTotal;
    char currentFile[260];  /* files-root-relative path being copied */
    char error[512];        /* valid when state == MP6_IMPORT_FAILED */
} Mp6ImportStatus;

/* Starts an import from a disc image (nod-backed). `source` is a plain
 * path on Windows or a content:// URI on Android (both open through
 * SDL_IOFromFile). `destDiscRoot` will directly contain sys/ + files/.
 * Returns 0 on thread start, -1 if an import is already running. */
int mp6_import_start_image(const char *source, const char *destDiscRoot);

/* Starts an import from an extracted GameCube tree at a plain filesystem
 * path. Accepts either the disc root itself (sys/ + files/ children), a
 * directory containing GP6E01/, or a Dolphin-style dump root. Same
 * threading/destination contract as mp6_import_start_image. */
int mp6_import_start_folder(const char *sourceRoot, const char *destDiscRoot);

/* Snapshot of the current import state (thread-safe; UI polls per frame). */
void mp6_import_poll(Mp6ImportStatus *out);

/* Requests cancellation; the worker stops at the next chunk boundary and
 * transitions to MP6_IMPORT_CANCELLED. */
void mp6_import_cancel(void);

/* Reaps the worker thread and resets state to MP6_IMPORT_IDLE. Only valid
 * once state is DONE/FAILED/CANCELLED. */
void mp6_import_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_CONTENT_IMPORT_H */
