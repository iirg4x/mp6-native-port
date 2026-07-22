// MP6 web packager -- output-path safety validator (SECURITY).
//
// A GameCube FST entry's name is attacker-controlled: it is an arbitrary
// NUL-terminated string lifted straight out of the disc image's string
// table (fst.js's parseFst()/readName()) -- and a folder pick's
// webkitRelativePath (folder-source.js) is just as untrusted; nothing
// stops a hostile or malformed tree from carrying a crafted entry name
// either. Both eventually become an output path handed to a sink's
// writeFile() built by simple concatenation, so a crafted name like
// "data/../../evil" (or "/abs/evil", "..\win", "C:\x") could otherwise
// place bytes outside the chosen root -- or, for the File System Access
// API tier, throw an unhandled TypeError that aborts the ENTIRE import
// instead of just that one bad entry (see directory-sink.js).
//
// Mirrors mp6-native's platform/content/content_path_safe.h
// (mp6_content_path_is_safe_rel(), commit 7aa2674) rule-for-rule so both
// ports reject exactly the same class of name. Returns true iff `p` is a
// safe, root-relative output path:
//   - a non-empty string, not absolute (no leading '/' or '\\');
//   - forward-slash separated, with no empty component (rejects a
//     leading, trailing, or doubled '/');
//   - no component is "." or ".." (traversal);
//   - contains no backslash (Windows separator / UNC) or colon (drive
//     letter / NTFS alternate-data-stream) anywhere;
//   - contains no control byte (< 0x20) anywhere.
//
// A path that passes this check cannot escape a root it's joined under:
// with no absolute prefix, no drive/UNC marker, and no ".." component, the
// lexical resolution stays strictly beneath root.
export function isSafeRelPath(p) {
    if (typeof p !== "string" || p.length === 0) return false; // empty
    if (p.startsWith("/") || p.startsWith("\\")) return false; // absolute

    for (let i = 0; i < p.length; i++) {
        const ch = p[i];
        if (p.charCodeAt(i) < 0x20) return false; // control byte
        if (ch === "\\" || ch === ":") return false; // separator / drive-or-ADS, anywhere
    }

    for (const part of p.split("/")) {
        if (part === "" || part === "." || part === "..") return false; // empty / "." / ".." component
    }

    return true;
}
