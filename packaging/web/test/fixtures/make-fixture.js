// MP6 web packager tests -- synthetic fixture: a tiny, fully valid fake
// GameCube disc image built in memory (a few KB), with a real boot.bin
// header, a real 12-byte-entry FST + string table, and real file data at
// the offsets the FST records. It exercises the same code path a real
// "Mario Party 6 (USA).iso" does (js/iso-source.js -> js/fst.js ->
// js/wanted.js), just at a size that's instant to build and diff.
//
// Tree (paths are files/-relative, matching wanted.js's convention):
//
//   data/title.bin        <- wanted (prefix "data")
//   data2/x.bin            <- NOT wanted (prefix boundary: "data2" != "data" + '/')
//   movie/foo.thp           <- NOT wanted (movie/ isn't in the wanted set at all)
//   opening.bnr               <- wanted (exact match)
//   sound/MP6_SND.msm          <- wanted (exact match)
//   sound/MP6_Str.pdt           <- wanted (exact match)
//   sound/other.bin               <- NOT wanted (sound/ has no prefix rule, only two exact files)
//
// 12 FST entries total: root + 4 directories + 7 files.

import {
    GAME_ID_OFFSET,
    BOOT_BIN_LENGTH,
    DOL_OFFSET_FIELD,
    FST_OFFSET_FIELD,
    FST_SIZE_FIELD,
    FST_MAX_SIZE_FIELD,
    FST_ENTRY_SIZE,
} from "../../js/fst.js";

const GC_MAGIC_OFFSET = 0x1c;
const GC_MAGIC_VALUE = 0xc2339f3d;
const textEncoder = new TextEncoder();

function align4(n) {
    return (n + 3) & ~3;
}

function concatBytes(arrays) {
    const total = arrays.reduce((sum, a) => sum + a.length, 0);
    const out = new Uint8Array(total);
    let offset = 0;
    for (const a of arrays) {
        out.set(a, offset);
        offset += a.length;
    }
    return out;
}

/** Deterministic, distinctive fake file content so byte-for-byte checks are meaningful. */
function makeContent(label, length) {
    const bytes = new Uint8Array(length);
    for (let i = 0; i < length; i++) bytes[i] = (label.charCodeAt(i % label.length) + i) & 0xff;
    return bytes;
}

function dir(name, children) {
    return { name, isDir: true, children };
}
function file(name, content) {
    return { name, isDir: false, content };
}

/**
 * Builds the synthetic disc image. Returns:
 *   { bytes, numEntries, allFiles, wantedFiles, wantedTotalBytes, fstOffset, fstSize }
 * where allFiles/wantedFiles are `[{ path, size, offset, content }]`, in FST
 * index order, for direct comparison against parseFst()/openIsoSource()
 * output.
 */
export function buildSyntheticIso() {
    const tree = dir("", [
        dir("data", [file("title.bin", makeContent("title.bin", 40))]),
        dir("data2", [file("x.bin", makeContent("data2/x.bin", 24))]),
        dir("movie", [file("foo.thp", makeContent("movie/foo.thp", 32))]),
        file("opening.bnr", makeContent("opening.bnr", 16)),
        dir("sound", [
            file("MP6_SND.msm", makeContent("sound/MP6_SND.msm", 20)),
            file("MP6_Str.pdt", makeContent("sound/MP6_Str.pdt", 28)),
            file("other.bin", makeContent("sound/other.bin", 12)),
        ]),
    ]);

    // 1. Preorder DFS index assignment -- a directory's whole subtree must
    //    occupy the contiguous index range right after it (real GC FST
    //    layout invariant; see fst.js's header comment).
    let nextIndex = 0;
    function assignIndices(node, path) {
        node.index = nextIndex++;
        node.path = path;
        if (node.isDir) {
            for (const child of node.children) {
                child.parentIndex = node.index;
                assignIndices(child, path ? `${path}/${child.name}` : child.name);
            }
            node.endIndex = nextIndex;
        }
    }
    assignIndices(tree, "");
    const numEntries = nextIndex;

    // 2. String table (root/index 0 is never looked up by name -- see
    //    fst.js's walk(), which starts at dirIndex+1 -- but every real
    //    fst.bin's table begins with a leading NUL, so this does too).
    const nameChunks = [new Uint8Array(1)]; // offset 0: empty string
    let cursor = 1;
    function collectNames(node, isRoot) {
        if (!isRoot) {
            node.nameOffset = cursor;
            const bytes = textEncoder.encode(node.name);
            nameChunks.push(bytes, new Uint8Array(1)); // name + NUL
            cursor += bytes.length + 1;
        }
        if (node.isDir) for (const child of node.children) collectNames(child, false);
    }
    collectNames(tree, true);
    const stringTable = concatBytes(nameChunks);

    // 3. Enumerate files in index order, then lay out their disc offsets
    //    right after the FST region (4-byte aligned, like a real disc).
    const allFileNodes = [];
    (function collectFiles(node) {
        if (node.isDir) node.children.forEach(collectFiles);
        else allFileNodes.push(node);
    })(tree);

    const fstEntriesLength = numEntries * FST_ENTRY_SIZE;
    const fstSize = fstEntriesLength + stringTable.length;
    const fstOffset = BOOT_BIN_LENGTH;
    let dataCursor = align4(fstOffset + fstSize);
    for (const f of allFileNodes) {
        f.discOffset = dataCursor;
        dataCursor = align4(dataCursor + f.content.length);
    }
    const totalSize = dataCursor;

    // 4. Encode the FST entries (12 bytes each; see fst.js's header comment
    //    for the field layout this mirrors).
    const fstBytes = new Uint8Array(fstSize);
    const fstView = new DataView(fstBytes.buffer);
    (function writeEntry(node) {
        const off = node.index * FST_ENTRY_SIZE;
        const nameOffset = node.index === 0 ? 0 : node.nameOffset;
        if (node.isDir) {
            fstView.setUint32(off, (1 << 24) | (nameOffset & 0xffffff), false);
            fstView.setUint32(off + 4, node.index === 0 ? 0 : node.parentIndex, false); // unused by our parser
            fstView.setUint32(off + 8, node.endIndex, false);
            node.children.forEach(writeEntry);
        } else {
            fstView.setUint32(off, nameOffset & 0xffffff, false);
            fstView.setUint32(off + 4, node.discOffset, false);
            fstView.setUint32(off + 8, node.content.length, false);
        }
    })(tree);
    fstBytes.set(stringTable, fstEntriesLength);

    // 5. Assemble the full image: boot.bin, fst.bin, then every file's data.
    const image = new Uint8Array(totalSize);
    image.set(textEncoder.encode("GP6E01"), GAME_ID_OFFSET);
    const bootView = new DataView(image.buffer, 0, BOOT_BIN_LENGTH);
    bootView.setUint32(GC_MAGIC_OFFSET, GC_MAGIC_VALUE, false);
    bootView.setUint32(DOL_OFFSET_FIELD, 0x2000, false); // plausible but unused by the packager
    bootView.setUint32(FST_OFFSET_FIELD, fstOffset, false);
    bootView.setUint32(FST_SIZE_FIELD, fstSize, false);
    bootView.setUint32(FST_MAX_SIZE_FIELD, fstSize, false);
    image.set(fstBytes, fstOffset);
    for (const f of allFileNodes) image.set(f.content, f.discOffset);

    const allFiles = allFileNodes.map((f) => ({
        path: f.path,
        size: f.content.length,
        offset: f.discOffset,
        content: f.content,
    }));
    const WANTED_PATHS = new Set(["data/title.bin", "opening.bnr", "sound/MP6_SND.msm", "sound/MP6_Str.pdt"]);
    const wantedFiles = allFiles.filter((f) => WANTED_PATHS.has(f.path));

    return {
        bytes: image,
        numEntries,
        allFiles,
        wantedFiles,
        wantedTotalBytes: wantedFiles.reduce((sum, f) => sum + f.size, 0),
        fstOffset,
        fstSize,
        fstBytes,
        bootBytes: image.slice(0, BOOT_BIN_LENGTH),
    };
}
