// Unit tests for js/fst.js: boot.bin header fields, raw FST entry decoding
// (built by hand, byte-by-byte, independent of the synthetic-fixture
// builder) and the full walk against the synthetic fixture.

import { test, assertEqual, assertDeepEqual, assertBytesEqual, assertThrows } from "./tiny-test.js";
import { parseBootBin, parseFst, BOOT_BIN_LENGTH, FST_OFFSET_FIELD, FST_SIZE_FIELD } from "../js/fst.js";
import { buildSyntheticIso } from "./fixtures/make-fixture.js";

function u32be(view, off, value) {
    view.setUint32(off, value, false);
}

// ---------------------------------------------------------------------
// parseBootBin
// ---------------------------------------------------------------------

test("parseBootBin: reads game ID and FST offset/size fields", () => {
    const bytes = new Uint8Array(BOOT_BIN_LENGTH);
    bytes.set(new TextEncoder().encode("GP6E01"), 0);
    const view = new DataView(bytes.buffer);
    u32be(view, FST_OFFSET_FIELD, 0x26e300);
    u32be(view, FST_SIZE_FIELD, 24145);
    const header = parseBootBin(bytes);
    assertEqual(header.gameId, "GP6E01");
    assertEqual(header.fstOffset, 0x26e300);
    assertEqual(header.fstSize, 24145);
});

test("parseBootBin: throws on a too-short buffer", async () => {
    await assertThrows(() => parseBootBin(new Uint8Array(10)), "too short");
});

// ---------------------------------------------------------------------
// parseFst: hand-built raw entries (independent of make-fixture.js)
// ---------------------------------------------------------------------

/** Encodes one 12-byte FST entry. */
function encodeEntry(view, index, { isDir, nameOffset, field1, field2 }) {
    const off = index * 12;
    view.setUint32(off, ((isDir ? 1 : 0) << 24) | (nameOffset & 0xffffff), false);
    view.setUint32(off + 4, field1, false);
    view.setUint32(off + 8, field2, false);
}

test("parseFst: flat root with two files (minimal case)", () => {
    // entries: 0=root(dir,end=3), 1=file "a", 2=file "bee"
    const names = "\0a\0bee\0"; // table offset 0 unused/empty, "a" at 1, "bee" at 3
    const nameBytes = new TextEncoder().encode(names);
    const bytes = new Uint8Array(3 * 12 + nameBytes.length);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 3 });
    encodeEntry(view, 1, { isDir: false, nameOffset: 1, field1: 0x1000, field2: 5 });
    encodeEntry(view, 2, { isDir: false, nameOffset: 3, field1: 0x2000, field2: 7 });
    bytes.set(nameBytes, 3 * 12);

    const result = parseFst(bytes);
    assertEqual(result.numEntries, 3);
    assertEqual(result.files.length, 2);
    assertDeepEqual(result.files[0], { index: 1, path: "a", offset: 0x1000, size: 5 });
    assertDeepEqual(result.files[1], { index: 2, path: "bee", offset: 0x2000, size: 7 });
});

test("parseFst: string table entries out of index order still resolve correctly", () => {
    // Deliberately place entry 2's name BEFORE entry 1's name in the table,
    // to make sure nothing assumes name-table order tracks entry order.
    const names = "\0second\0first\0"; // "second" at 1, "first" at 8
    const nameBytes = new TextEncoder().encode(names);
    const bytes = new Uint8Array(3 * 12 + nameBytes.length);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 3 });
    encodeEntry(view, 1, { isDir: false, nameOffset: 8, field1: 100, field2: 4 }); // "first"
    encodeEntry(view, 2, { isDir: false, nameOffset: 1, field1: 200, field2: 4 }); // "second"
    bytes.set(nameBytes, 3 * 12);

    const result = parseFst(bytes);
    assertEqual(result.files[0].path, "first");
    assertEqual(result.files[1].path, "second");
});

test("parseFst: empty directory (zero children) is walked without error", () => {
    // 0=root(end=2), 1=dir "empty"(end=2, i.e. no children)
    const names = "\0empty\0";
    const nameBytes = new TextEncoder().encode(names);
    const bytes = new Uint8Array(2 * 12 + nameBytes.length);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 2 });
    encodeEntry(view, 1, { isDir: true, nameOffset: 1, field1: 0, field2: 2 });
    bytes.set(nameBytes, 2 * 12);

    const result = parseFst(bytes);
    assertEqual(result.numEntries, 2);
    assertEqual(result.files.length, 0);
    assertEqual(result.dirs.length, 1);
    assertEqual(result.dirs[0].path, "empty");
});

test("parseFst: nested directories two levels deep build correct paths", () => {
    // root(0,end=4) -> a(1,dir,end=4) -> b(2,dir,end=4) -> leaf.txt(3,file)
    const names = "\0a\0b\0leaf.txt\0";
    const nameBytes = new TextEncoder().encode(names);
    const bytes = new Uint8Array(4 * 12 + nameBytes.length);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 4 });
    encodeEntry(view, 1, { isDir: true, nameOffset: 1, field1: 0, field2: 4 });
    encodeEntry(view, 2, { isDir: true, nameOffset: 3, field1: 1, field2: 4 });
    encodeEntry(view, 3, { isDir: false, nameOffset: 5, field1: 999, field2: 42 });
    bytes.set(nameBytes, 4 * 12);

    const result = parseFst(bytes);
    assertEqual(result.files.length, 1);
    assertEqual(result.files[0].path, "a/b/leaf.txt");
    assertEqual(result.files[0].offset, 999);
    assertEqual(result.files[0].size, 42);
});

test("parseFst: throws when entry 0 isn't a directory", async () => {
    const bytes = new Uint8Array(12);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: false, nameOffset: 0, field1: 0, field2: 1 });
    await assertThrows(() => parseFst(bytes), "not a directory");
});

test("parseFst: throws when declared entry count doesn't fit the buffer", async () => {
    const bytes = new Uint8Array(12); // room for exactly 1 entry
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 999 }); // claims 999 entries
    await assertThrows(() => parseFst(bytes), "does not fit");
});

test("parseFst: throws on a name that runs off the end without a NUL terminator", async () => {
    // 0=root(end=2), 1=file whose name starts right after the entries but
    // is never NUL-terminated before the buffer ends.
    const bytes = new Uint8Array(2 * 12 + 3); // + "abc", deliberately no trailing NUL
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 2 });
    encodeEntry(view, 1, { isDir: false, nameOffset: 0, field1: 0, field2: 0 });
    bytes.set(new TextEncoder().encode("abc"), 2 * 12);
    await assertThrows(() => parseFst(bytes), "not NUL-terminated");
});

test("parseFst: throws on a directory whose end index doesn't make sense", async () => {
    const names = "\0bad\0";
    const nameBytes = new TextEncoder().encode(names);
    const bytes = new Uint8Array(2 * 12 + nameBytes.length);
    const view = new DataView(bytes.buffer);
    encodeEntry(view, 0, { isDir: true, nameOffset: 0, field1: 0, field2: 2 });
    encodeEntry(view, 1, { isDir: true, nameOffset: 1, field1: 0, field2: 0 }); // end (0) <= own index (1)
    bytes.set(nameBytes, 2 * 12);
    await assertThrows(() => parseFst(bytes), "invalid end index");
});

// ---------------------------------------------------------------------
// parseFst against the synthetic fixture (path/offset/size correctness)
// ---------------------------------------------------------------------

test("parseFst: synthetic fixture entry count matches the tree (12: root + 4 dirs + 7 files)", () => {
    const fixture = buildSyntheticIso();
    const result = parseFst(fixture.fstBytes);
    assertEqual(result.numEntries, 12);
    assertEqual(result.numEntries, fixture.numEntries);
});

test("parseFst: synthetic fixture file list matches expected paths/offsets/sizes exactly", () => {
    const fixture = buildSyntheticIso();
    const result = parseFst(fixture.fstBytes);
    assertEqual(result.files.length, fixture.allFiles.length, "file count");
    for (const expected of fixture.allFiles) {
        const actual = result.files.find((f) => f.path === expected.path);
        assertTrueFound(actual, expected.path);
        assertEqual(actual.offset, expected.offset, `offset for ${expected.path}`);
        assertEqual(actual.size, expected.size, `size for ${expected.path}`);
    }
});

function assertTrueFound(actual, path) {
    if (!actual) throw new Error(`expected fst.bin to contain a file at "${path}", but it did not`);
}

test("parseFst: synthetic fixture file bytes at their recorded offsets match the source content exactly", () => {
    const fixture = buildSyntheticIso();
    const result = parseFst(fixture.fstBytes);
    for (const expected of fixture.allFiles) {
        const entry = result.files.find((f) => f.path === expected.path);
        const actualBytes = fixture.bytes.subarray(entry.offset, entry.offset + entry.size);
        assertBytesEqual(actualBytes, expected.content, `content bytes for ${expected.path}`);
    }
});

test("parseFst: synthetic fixture directory count is 4 (data, data2, movie, sound)", () => {
    const fixture = buildSyntheticIso();
    const result = parseFst(fixture.fstBytes);
    assertEqual(result.dirs.length, 4);
    assertDeepEqual(
        result.dirs.map((d) => d.path).sort(),
        ["data", "data2", "movie", "sound"]
    );
});
