/* MP6 native port -- extraction path-safety validator (SECURITY).
 *
 * A GameCube FST entry's name is attacker-controlled: it is an arbitrary
 * NUL-terminated string lifted straight out of the disc image's string
 * table. content_import.cpp and setup/lib/nod_ffi.py both build a
 * destination path by concatenating that name onto the extraction root, so
 * a crafted disc whose FST names a file "data/../../evil" (or "/abs/evil",
 * or "..\\win", or "C:\\x") would otherwise place bytes OUTSIDE the chosen
 * root. This validator is the single gate that rejects such names; a
 * rejected entry is skipped + logged, never written.
 *
 * Header-only and dependency-free ON PURPOSE: the importer TU and the unit
 * test (platform/content/test_path_safe.cpp) both #include it, so the test
 * exercises the identical function without dragging in SDL/nod.
 */
#ifndef MP6_CONTENT_PATH_SAFE_H
#define MP6_CONTENT_PATH_SAFE_H

/* Returns 1 iff `rel` is a safe files-root-relative extraction path:
 *   - non-empty and not absolute (no leading '/');
 *   - forward-slash separated, with NO empty component (rejects a leading,
 *     trailing, or doubled '/');
 *   - every component a plain name -- never "." or ".." (traversal), never
 *     containing '\\' (Windows separator / UNC), ':' (drive letter or NTFS
 *     alternate-data-stream), or any control byte < 0x20 (embedded NUL can
 *     never appear mid-string, but a stray CR/LF/etc. is refused too).
 *
 * A path that passes this check cannot escape its root when joined as
 * root + "/" + rel: with no absolute prefix, no drive/UNC, and no ".."
 * component, the lexical resolution stays strictly beneath root.
 */
static inline int mp6_content_path_is_safe_rel(const char *rel)
{
    unsigned long i;
    unsigned long compLen = 0; /* chars in the component being scanned */
    int compIsDot = 0;         /* component so far is exactly "."      */
    int compIsDotDot = 0;      /* component so far is exactly ".."     */

    if (rel == 0 || rel[0] == '\0') {
        return 0; /* empty */
    }
    if (rel[0] == '/') {
        return 0; /* absolute (POSIX) */
    }

    for (i = 0;; ++i) {
        unsigned char c = (unsigned char)rel[i];
        if (c == '\0' || c == '/') {
            if (compLen == 0) {
                return 0; /* empty component: leading/trailing/doubled slash */
            }
            if (compIsDot || compIsDotDot) {
                return 0; /* "." or ".." traversal component */
            }
            if (c == '\0') {
                return 1; /* end reached, every component clean */
            }
            compLen = 0;
            compIsDot = 0;
            compIsDotDot = 0;
            continue;
        }
        if (c == '\\' || c == ':' || c < 0x20) {
            return 0; /* separator / drive-or-ADS / control byte */
        }
        if (compLen == 0) {
            compIsDot = (c == '.');
            compIsDotDot = 0;
        } else if (compLen == 1 && compIsDot && c == '.') {
            compIsDot = 0;
            compIsDotDot = 1;
        } else {
            compIsDot = 0;
            compIsDotDot = 0;
        }
        ++compLen;
    }
}

#endif /* MP6_CONTENT_PATH_SAFE_H */
