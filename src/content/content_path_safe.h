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
 * test (src/content/test_path_safe.cpp) both #include it, so the test
 * exercises the identical function without dragging in SDL/nod.
 */
#ifndef MP6_CONTENT_PATH_SAFE_H
#define MP6_CONTENT_PATH_SAFE_H

static inline unsigned char mp6_content_ascii_upper(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ? (unsigned char)(c - ('a' - 'A')) : c;
}

static inline int mp6_content_ascii_component_eq(const char *s, unsigned long n,
                                                 const char *want)
{
    unsigned long i;
    for (i = 0; i < n && want[i] != '\0'; ++i) {
        if (mp6_content_ascii_upper((unsigned char)s[i]) != (unsigned char)want[i]) {
            return 0;
        }
    }
    return i == n && want[i] == '\0';
}

/* Windows resolves these names as devices even when an extension is present
 * (for example, "NUL.bin"). Rejecting them on every host keeps an imported
 * tree portable and prevents a successful-looking write from disappearing
 * into a device on Windows. */
static inline int mp6_content_component_is_windows_device(const char *s, unsigned long n)
{
    unsigned long baseLen = 0;
    while (baseLen < n && s[baseLen] != '.') {
        ++baseLen;
    }
    if (mp6_content_ascii_component_eq(s, baseLen, "CON") ||
        mp6_content_ascii_component_eq(s, baseLen, "PRN") ||
        mp6_content_ascii_component_eq(s, baseLen, "AUX") ||
        mp6_content_ascii_component_eq(s, baseLen, "NUL") ||
        mp6_content_ascii_component_eq(s, baseLen, "CONIN$") ||
        mp6_content_ascii_component_eq(s, baseLen, "CONOUT$") ||
        mp6_content_ascii_component_eq(s, baseLen, "CLOCK$")) {
        return 1;
    }
    if (baseLen == 4) {
        unsigned char a = mp6_content_ascii_upper((unsigned char)s[0]);
        unsigned char b = mp6_content_ascii_upper((unsigned char)s[1]);
        unsigned char c = mp6_content_ascii_upper((unsigned char)s[2]);
        unsigned char d = (unsigned char)s[3];
        if (d >= '1' && d <= '9' &&
            ((a == 'C' && b == 'O' && c == 'M') ||
             (a == 'L' && b == 'P' && c == 'T'))) {
            return 1;
        }
    }
    return 0;
}

/* Returns 1 iff `rel` is a safe files-root-relative extraction path:
 *   - non-empty and not absolute (no leading '/');
 *   - forward-slash separated, with NO empty component (rejects a leading,
 *     trailing, or doubled '/');
 *   - every component a plain name -- never "." or ".." (traversal), never
 *     containing a Windows-illegal separator/metacharacter, ':' (drive letter
 *     or NTFS alternate-data-stream), a non-printable/non-ASCII byte, a
 *     trailing dot/space, or a Windows device basename (NUL, CON, COM1, ...).
 *
 * A path that passes this check cannot escape its root when joined as
 * root + "/" + rel: with no absolute prefix, no drive/UNC, and no ".."
 * component, the lexical resolution stays strictly beneath root.
 */
static inline int mp6_content_path_is_safe_rel(const char *rel)
{
    unsigned long i;
    unsigned long compStart = 0;
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
            if (rel[i - 1] == '.' || rel[i - 1] == ' ' ||
                mp6_content_component_is_windows_device(rel + compStart, compLen)) {
                return 0; /* Windows normalization/device alias */
            }
            if (c == '\0') {
                return 1; /* end reached, every component clean */
            }
            compStart = i + 1;
            compLen = 0;
            compIsDot = 0;
            compIsDotDot = 0;
            continue;
        }
        if (c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' ||
            c == '|' || c == '?' || c == '*' || c < 0x20 || c > 0x7E) {
            return 0; /* separator / drive-or-ADS / metacharacter / non-printable-or-non-ASCII */
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

/* An FST/provider display name is one directory ENTRY, not an already-joined
 * relative path. Reject a forged separator even when both resulting pieces
 * would separately be safe; otherwise it can alias a real nested entry. */
static inline int mp6_content_path_is_safe_component(const char *name)
{
    unsigned long i;
    if (name == 0) return 0;
    for (i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '/') return 0;
    }
    return mp6_content_path_is_safe_rel(name);
}

#endif /* MP6_CONTENT_PATH_SAFE_H */
