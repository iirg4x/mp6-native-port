/* MP6 native port -- real DVD file serving over the extracted GameCube disc
 * tree. See src/dvd/dvd_files.c for the implementation;
 * src/os/dll_bridge.c calls these AFTER checking its own synthetic
 * "dll/*.rel" paths, so the statically-linked overlays keep loading as
 * they always have -- this module only ever serves paths dll_bridge.c
 * doesn't already own.
 */
#ifndef MP6_DVD_FILES_H
#define MP6_DVD_FILES_H

#include "dolphin.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int mp6_dvd_open_handle_count(void);

/* Real disc's FST-driven path resolution (external_refs/.../dvdfs.c is the
 * read-only reference this reimplements). Returns -1 (honest "not found")
 * exactly like the real SDK call would for a path genuinely not on this
 * disc; never panics. */
s32 mp6_dvd_path_to_entrynum(const char *path);

/* Opens the real host file a resolved path/entrynum corresponds to under
 * orig/GP6E01/files/. Both fail (FALSE) for a directory entry or a path/
 * entrynum the FST doesn't have -- same "honest not found" contract as
 * DVDOpen/DVDFastOpen. */
BOOL mp6_dvd_open_by_path(const char *path, DVDFileInfo *fileInfo);
BOOL mp6_dvd_open_by_entrynum(s32 entrynum, DVDFileInfo *fileInfo);

/* Reads `length` bytes at file-relative `offset` into `addr` (already
 * allocated by the caller -- see game/dvd.c's HuDvdDataReadWait). Pads
 * with zeros past real end-of-file (real GC sector padding this port's
 * individually-extracted files don't carry) rather than failing outright,
 * since callers routinely round requests up to a block/32-byte boundary.
 * Completes fully before returning -- there is no real async here. */
BOOL mp6_dvd_read(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset);

/* No-op if fileInfo isn't one of this module's own open real-file handles
 * (e.g. it's one of dll_bridge.c's synthetic REL handles instead) -- safe
 * to call unconditionally from DVDClose. */
void mp6_dvd_close(DVDFileInfo *fileInfo);

/* Validates a complete runtime disc root before it is selected or published:
 * exact GP6E01 boot ID, structurally safe fst.bin, and the three mandatory
 * top-level assets used by every supported menu flow. Returns 1 on success,
 * 0 with a human-readable error otherwise. */
int mp6_dvd_validate_disc_root(const char *discRoot, char *err, size_t errn);

/* Launcher helpers (implemented by dvd_files.c). */
int mp6_dvd_probe_root(char *filesRootOut, size_t n);
/* Returns 0 only when both complete paths fit and were published together;
 * otherwise leaves the previous/automatic resolution untouched. */
int mp6_dvd_set_root_override(const char *filesRoot, const char *fstPath);

#ifdef __cplusplus
}
#endif

#endif /* MP6_DVD_FILES_H */
