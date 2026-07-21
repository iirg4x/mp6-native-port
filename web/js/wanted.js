// MP6 web packager -- the "wanted file set" filter.
//
// Transcribed verbatim (logic, not syntax) from mp6-native's
// platform/content/content_import.cpp:76-88:
//
//   bool path_has_prefix(const char *path, const char *prefix) {
//       size_t n = strlen(prefix);
//       return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == '\0');
//   }
//   bool wanted_file(const char *rel) {
//       if (strcmp(rel, "opening.bnr") == 0) return true;
//       if (strcmp(rel, "sound/MP6_SND.msm") == 0) return true;
//       if (strcmp(rel, "sound/MP6_Str.pdt") == 0) return true;
//       return path_has_prefix(rel, "data") || path_has_prefix(rel, "mess") || path_has_prefix(rel, "mic");
//   }
//
// and the design-contract comment at content_import.h:5-10, which names the
// same set in prose: sys/fst.bin, files/opening.bnr,
// files/sound/MP6_SND.msm, files/sound/MP6_Str.pdt, and everything under
// files/data, files/mess, files/mic ("a ~401MB subset"). `rel` here is
// always a files/-relative POSIX path (forward slashes), matching what
// parseFst() and the folder-source reader both produce.
//
// content_import.cpp additionally writes sys/boot.bin (1088 bytes) and
// sys/fst.bin itself into the destination disc root -- see the worker
// functions' "sys/ last, fst.bin very last" torn-import-safety comment
// (content_import.cpp:338-350, :489-504). Those two are not files/-relative
// paths and are handled directly by the packager orchestration
// (js/packager.js), not by this filter.

const EXACT_MATCHES = new Set(["opening.bnr", "sound/MP6_SND.msm", "sound/MP6_Str.pdt"]);
const PREFIX_MATCHES = ["data", "mess", "mic"];

/** Mirrors content_import.cpp's path_has_prefix(). */
export function pathHasPrefix(path, prefix) {
    if (!path.startsWith(prefix)) return false;
    const next = path.charAt(prefix.length);
    return next === "" || next === "/";
}

/**
 * Mirrors content_import.cpp's wanted_file(). `rel` is a files/-relative
 * POSIX path, e.g. "data/title.bin", "sound/MP6_SND.msm", "movie/foo.thp".
 */
export function wantedFile(rel) {
    if (EXACT_MATCHES.has(rel)) return true;
    return PREFIX_MATCHES.some((prefix) => pathHasPrefix(rel, prefix));
}

/** Filters a list of {path, ...} objects (e.g. parseFst()'s `files`) down to the wanted set. */
export function filterWanted(files) {
    return files.filter((f) => wantedFile(f.path));
}
