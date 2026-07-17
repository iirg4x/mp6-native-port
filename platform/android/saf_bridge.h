/* MP6 native port -- Android SAF (Storage Access Framework) bridge for the
 * extracted-folder onboarding path.
 *
 * SAF trees (ACTION_OPEN_DOCUMENT_TREE) have no filesystem path and can
 * only be traversed through the Java ContentResolver/DocumentsContract
 * APIs -- so unlike the disc-image path (whose content:// URI opens as a
 * plain fd through SDL_IOFromFile and feeds the native nod importer), the
 * folder pick AND the folder import both live on the Java side
 * (platforms/android/.../Mp6Activity.java). This bridge is the thin JNI
 * poll surface the launcher UI (platform/gx/ui/content_setup.cpp) drives
 * once per frame.
 *
 * Threading contract: every function here is called from the SDL main
 * thread only (the launcher's pre-boot menu loop) -- a Java-attached
 * thread by construction, and NEVER a game coroutine stack (the
 * no-JNI-from-coroutine-stack law; the launcher runs before any
 * coroutine exists).
 * The Java import itself runs on a plain Java thread and publishes
 * progress through volatile statics.
 *
 * Android --windowed builds only (tools/build.py).
 */
#ifndef MP6_SAF_BRIDGE_H
#define MP6_SAF_BRIDGE_H

#include "../content/content_import.h" /* Mp6ImportStatus */

#ifdef __cplusplus
extern "C" {
#endif

/* Fires the ACTION_OPEN_DOCUMENT_TREE picker (on the Java UI thread).
 * Returns 0 on dispatch, -1 if the JNI bridge is unavailable. */
int mp6_saf_open_tree_picker(void);

/* Polls the picker result. Returns 1 with the tree URI copied into
 * uriOut when the user picked a folder, 0 while pending, -1 when the
 * picker was dismissed/cancelled. One-shot: a returned result is
 * consumed. */
int mp6_saf_poll_tree_pick(char *uriOut, size_t n);

/* Starts the Java-side tree import (validate GP6E01, copy the wanted
 * file set into destDiscRoot, fst.bin last -- the same contract as the
 * native importer). Returns 0 on start, -1 on bridge failure or if an
 * import is already running. */
int mp6_saf_tree_import_start(const char *treeUri, const char *destDiscRoot);

/* Snapshot of the Java import's progress into the shared status shape. */
void mp6_saf_tree_import_poll(Mp6ImportStatus *out);

/* Requests cancellation of the Java import. */
void mp6_saf_tree_import_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_SAF_BRIDGE_H */
