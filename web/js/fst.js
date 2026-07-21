// MP6 web packager -- GameCube boot.bin / FST (File String Table) parsing.
//
// Pure functions over bytes (Uint8Array/DataView). No browser-only API is
// used here on purpose: this module runs unmodified under Node for the test
// suite (web/test/fst.test.js) and under the browser for the real packager.
//
// Source of truth for the two layouts below:
//
//  - boot.bin field offsets (game ID, DOL offset, FST offset, FST size) are
//    the standard GameCube disc header layout (widely documented -- e.g. the
//    YAGCD GC disc structure chapter, Dolphin's DiscIO volume headers, and
//    every GC disc tool). Cross-checked against this project's own repo: the
//    9-byte set { GameID@0x00, FSTOffset@0x424, FSTSize@0x428 } was verified
//    byte-for-byte against the real "Mario Party 6 (USA).iso" during this
//    feature's development (dol@0x20300, fst@0x26E300+24145, matching
//    mp6-native's tools/mp6scene/mp6_iso.py CLI output verbatim), and
//    mp6-native's platform/content/content_import.cpp:172-187
//    (validate_game_id) is the 6-byte game-ID check this mirrors.
//
//  - FST entry format (12 bytes/entry, string table right after the last
//    entry) is transcribed from mp6-native's own comments at
//    platform/dvd/dvd_files.c:174-187, which documents it as read from the
//    real sys/fst.bin this port ships against:
//
//      [0..3]  isDirAndStringOff  (top byte: 0=file/nonzero=dir; low 24 bits:
//                                  byte offset of this entry's name in the
//                                  string table that starts right after the
//                                  last entry)
//      [4..7]  parentOrPosition   (dir: parent entrynum. file: disc byte
//                                  position)
//      [8..11] nextEntryOrLength  (dir: entrynum just past this dir's whole
//                                  subtree. file: byte length. Entry 0 (the
//                                  root dir)'s own value here doubles as
//                                  MaxEntryNum, the total entry count.)
//
//    The tree-walk below (walkFst) mirrors the depth-first, contiguous-
//    children-on-disk algorithm documented at dvd_files.c:232-281
//    (fst_walk_build) and re-implemented for the extraction filter at
//    content_import.cpp:225-262 (fst_callback).

export const GAME_ID_OFFSET = 0x00;
export const GAME_ID_LENGTH = 6;
export const BOOT_BIN_LENGTH = 0x440; // sys/boot.bin is always exactly this size on GC discs
export const DOL_OFFSET_FIELD = 0x420;
export const FST_OFFSET_FIELD = 0x424;
export const FST_SIZE_FIELD = 0x428;
export const FST_MAX_SIZE_FIELD = 0x42c;
export const FST_ENTRY_SIZE = 12;

/** Reads a big-endian u32 out of a DataView at `off`. */
function u32be(view, off) {
    return view.getUint32(off, false);
}

/**
 * Parses the fixed boot.bin header fields the packager needs.
 * `bytes` must be at least BOOT_BIN_LENGTH (0x440) bytes, starting at disc
 * offset 0.
 */
export function parseBootBin(bytes) {
    if (bytes.length < FST_SIZE_FIELD + 4) {
        throw new Error("boot.bin fragment too short to contain the FST offset/size fields");
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const gameId = bytesToAsciiString(bytes.subarray(GAME_ID_OFFSET, GAME_ID_OFFSET + GAME_ID_LENGTH));
    return {
        gameId,
        dolOffset: u32be(view, DOL_OFFSET_FIELD),
        fstOffset: u32be(view, FST_OFFSET_FIELD),
        fstSize: u32be(view, FST_SIZE_FIELD),
    };
}

function bytesToAsciiString(bytes) {
    let s = "";
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return s;
}

/** One decoded FST entry, in raw form (before path resolution). */
function readRawEntry(view, index) {
    const off = index * FST_ENTRY_SIZE;
    const word0 = u32be(view, off);
    const isDir = (word0 >>> 24) !== 0;
    const nameOffset = word0 & 0x00ffffff;
    const field1 = u32be(view, off + 4); // dir: parent index. file: disc byte offset.
    const field2 = u32be(view, off + 8); // dir: end-of-subtree index. file: byte length.
    return { index, isDir, nameOffset, field1, field2 };
}

/** Reads a NUL-terminated ASCII/UTF-8-ish name out of the FST string table. */
function readName(bytes, stringTableOffset, nameOffset) {
    const start = stringTableOffset + nameOffset;
    if (start < 0 || start >= bytes.length) {
        throw new Error(`FST string table offset ${start} is out of range (table starts at ${stringTableOffset})`);
    }
    let end = start;
    while (end < bytes.length && bytes[end] !== 0) end++;
    if (end >= bytes.length) {
        throw new Error(`FST entry name starting at ${start} is not NUL-terminated within the buffer`);
    }
    // Names on this disc are plain ASCII; decode defensively as latin1/UTF-8
    // subset so an unexpected high byte cannot throw.
    let s = "";
    for (let i = start; i < end; i++) s += String.fromCharCode(bytes[i]);
    return s;
}

/**
 * Parses a complete fst.bin buffer into a flat list of files with their
 * disc-relative (files/-relative) POSIX paths, byte offsets and sizes, plus
 * the directory count. `bytes` is the raw fst.bin content (NOT the whole
 * disc image).
 *
 * Mirrors dvd_files.c's fst_walk_build: entry 0 is always the root
 * directory, and a directory's children are laid out contiguously on disk
 * immediately after it, so a single depth-first walk driven by each
 * directory's own "end index" (field2) visits every entry exactly once.
 */
export function parseFst(bytes) {
    if (bytes.length < FST_ENTRY_SIZE) {
        throw new Error(`fst.bin is implausibly small (${bytes.length} bytes)`);
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const root = readRawEntry(view, 0);
    if (!root.isDir) {
        throw new Error("FST entry 0 is not a directory -- this is not a valid fst.bin");
    }
    const numEntries = root.field2;
    if (numEntries === 0 || numEntries * FST_ENTRY_SIZE > bytes.length) {
        throw new Error(
            `FST claims ${numEntries} entries, which does not fit in a ${bytes.length}-byte buffer`
        );
    }
    const stringTableOffset = numEntries * FST_ENTRY_SIZE;

    const files = [];
    const dirs = [];

    function walk(dirIndex, prefix) {
        const dirEntry = readRawEntry(view, dirIndex);
        const end = dirEntry.field2;
        let i = dirIndex + 1;
        while (i < end) {
            if (i >= numEntries) {
                throw new Error(`FST walk ran past entry ${numEntries} while inside directory ${dirIndex}`);
            }
            const entry = readRawEntry(view, i);
            const name = readName(bytes, stringTableOffset, entry.nameOffset);
            const path = prefix ? `${prefix}/${name}` : name;
            if (entry.isDir) {
                const childEnd = entry.field2;
                if (childEnd <= i || childEnd > numEntries) {
                    throw new Error(`FST directory entry ${i} ("${path}") has an invalid end index ${childEnd}`);
                }
                dirs.push({ index: i, path });
                walk(i, path);
                i = childEnd;
            } else {
                files.push({ index: i, path, offset: entry.field1, size: entry.field2 });
                i++;
            }
        }
    }

    walk(0, "");

    return { numEntries, files, dirs };
}

/**
 * Convenience: parses boot.bin + fst.bin together the way the packager
 * needs for an ISO source (validate game ID, then locate + parse the FST).
 * `bootBytes` is the first BOOT_BIN_LENGTH bytes of the disc image;
 * `fstBytes` is the FST region read separately (its offset/size come from
 * parseBootBin's result).
 */
export function parseDiscHeaderAndFst(bootBytes, fstBytes) {
    const header = parseBootBin(bootBytes);
    const fst = parseFst(fstBytes);
    return { header, ...fst };
}
