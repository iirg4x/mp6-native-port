// Unit tests for js/iso-source.js against the synthetic fixture, wrapped in
// Node's built-in Blob -- which already implements the exact ByteSource
// shape (byte-source.js: `.size`, `.slice(start,end).arrayBuffer()`) a
// browser File does, so this exercises the real chunked-read code path,
// not a simplified stand-in.

import { test, assertEqual, assertBytesEqual, assertDeepEqual } from "./tiny-test.js";
import { openIsoSource } from "../js/iso-source.js";
import { buildSyntheticIso } from "./fixtures/make-fixture.js";

test("openIsoSource: synthetic fixture opens successfully and reports the right header", async () => {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const result = await openIsoSource(blob);
    assertEqual(result.ok, true);
    assertEqual(result.source.header.gameId, "GP6E01");
    assertEqual(result.source.header.fstOffset, fixture.fstOffset);
    assertEqual(result.source.header.fstSize, fixture.fstSize);
    assertEqual(result.source.numEntries, fixture.numEntries);
});

test("openIsoSource: wantedFiles matches the fixture's expected wanted set (paths + sizes + total)", async () => {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const { source } = await openIsoSource(blob);
    assertDeepEqual(
        source.wantedFiles.map((f) => f.path).sort(),
        fixture.wantedFiles.map((f) => f.path).sort()
    );
    const wantedBytesOnly = source.wantedFiles.reduce((sum, f) => sum + f.size, 0);
    assertEqual(wantedBytesOnly, fixture.wantedTotalBytes);
});

test("openIsoSource: readWantedFile() streams back byte-identical content (single big chunk)", async () => {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const { source } = await openIsoSource(blob);
    for (const entry of source.wantedFiles) {
        const expected = fixture.wantedFiles.find((f) => f.path === entry.path).content;
        const chunks = [];
        for await (const chunk of source.readWantedFile(entry)) chunks.push(chunk);
        assertBytesEqual(concat(chunks), expected, entry.path);
    }
});

test("openIsoSource: readWantedFile() reassembles correctly even when forced into many tiny chunks", async () => {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const { source } = await openIsoSource(blob);
    const entry = source.wantedFiles.find((f) => f.path === "sound/MP6_Str.pdt");
    const expected = fixture.wantedFiles.find((f) => f.path === "sound/MP6_Str.pdt").content;
    const chunks = [];
    for await (const chunk of source.readWantedFile(entry, 3 /* tiny chunk size */)) chunks.push(chunk);
    assertEqual(chunks.length > 1, true, "expected the tiny chunk size to force multiple reads");
    assertBytesEqual(concat(chunks), expected);
});

test("openIsoSource: sys/fst.bin and sys/boot.bin bytes are captured verbatim for output", async () => {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const { source } = await openIsoSource(blob);
    assertBytesEqual(source.fstBytes, fixture.fstBytes, "fst.bin");
    assertBytesEqual(source.bootBytes, fixture.bootBytes, "boot.bin");
});

test("openIsoSource: rejects a disc image with the wrong game ID", async () => {
    const fixture = buildSyntheticIso();
    const tampered = fixture.bytes.slice();
    tampered.set(new TextEncoder().encode("GAFE01"), 0); // Animal Crossing
    const result = await openIsoSource(new Blob([tampered]));
    assertEqual(result.ok, false);
    assertEqual(result.error.includes("Not Mario Party 6"), true);
});

test("openIsoSource: rejects an RVZ file with a specific conversion hint", async () => {
    const rvz = new Uint8Array(2048);
    rvz.set([0x52, 0x56, 0x5a, 0x01], 0);
    const result = await openIsoSource(new Blob([rvz]));
    assertEqual(result.ok, false);
    assertEqual(result.error.includes("RVZ"), true);
    assertEqual(result.error.includes("Dolphin"), true);
});

test("openIsoSource: rejects a file too small to contain a boot.bin", async () => {
    const result = await openIsoSource(new Blob([new Uint8Array(100)]));
    assertEqual(result.ok, false);
    assertEqual(result.error.includes("too small"), true);
});

function concat(chunks) {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        out.set(c, off);
        off += c.length;
    }
    return out;
}
