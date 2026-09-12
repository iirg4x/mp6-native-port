// Round-trip test: js/zip-writer.js's STORE-only streaming output, read
// back by js/zip-reader.js. This is the part of the ZIP pipeline that's
// fully exercised under Node -- the DEFLATE decode path in zip-reader.js
// (for reading a normally-built engine release zip) depends on the
// browser/Node-native DecompressionStream and is covered by manual/browser
// testing instead (see MANUAL_TEST.md and the feature report).

import { test, assertEqual, assertBytesEqual, assertDeepEqual } from "./tiny-test.js";
import { ZipWriter } from "../js/zip-writer.js";
import { parseZip, readZipEntry } from "../js/zip-reader.js";

async function buildTestZip(files) {
    const parts = [];
    const writer = new ZipWriter((chunk) => {
        parts.push(chunk);
    });
    for (const f of files) {
        await writer.beginFile(f.path, new Date(2024, 0, 1));
        // Split content into a couple of chunks to exercise multi-chunk entries too.
        const mid = Math.ceil(f.content.length / 2);
        if (mid > 0) await writer.writeChunk(f.content.subarray(0, mid));
        if (f.content.length - mid > 0) await writer.writeChunk(f.content.subarray(mid));
        await writer.endFile();
    }
    await writer.finish();
    const total = parts.reduce((s, p) => s + p.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

function textFile(path, text) {
    return { path, content: new TextEncoder().encode(text) };
}

test("zip round-trip: single small file", async () => {
    const zipBytes = await buildTestZip([textFile("hello.txt", "hello, mp6")]);
    const { entries } = parseZip(zipBytes);
    assertEqual(entries.length, 1);
    assertEqual(entries[0].path, "hello.txt");
    assertEqual(entries[0].method, 0);
    assertEqual(entries[0].uncompressedSize, "hello, mp6".length);

    const chunks = [];
    for await (const chunk of readZipEntry(zipBytes, entries[0])) chunks.push(chunk);
    const decoded = new TextDecoder().decode(concat(chunks));
    assertEqual(decoded, "hello, mp6");
});

test("zip round-trip: multiple files, nested paths, binary content, CRC verified", async () => {
    const binary = new Uint8Array(5000);
    for (let i = 0; i < binary.length; i++) binary[i] = (i * 37 + 11) & 0xff;

    const files = [
        textFile("mp6native.exe", "not a real exe, just bytes"),
        { path: "content/GP6E01/sys/fst.bin", content: binary },
        textFile("res/rml/window.rcss", "/* css */"),
        textFile("empty.bin", ""),
    ];
    const zipBytes = await buildTestZip(files);
    const { entries } = parseZip(zipBytes);
    assertEqual(entries.length, files.length);
    assertDeepEqual(
        entries.map((e) => e.path).sort(),
        files.map((f) => f.path).sort()
    );

    for (const f of files) {
        const entry = entries.find((e) => e.path === f.path);
        const chunks = [];
        for await (const chunk of readZipEntry(zipBytes, entry)) chunks.push(chunk);
        assertBytesEqual(concat(chunks), f.content, `content of ${f.path}`);
    }
});

test("zip round-trip: a corrupted entry byte fails CRC verification", async () => {
    const zipBytes = await buildTestZip([textFile("a.txt", "hello world, this is more than a few bytes long")]);
    const { entries } = parseZip(zipBytes);
    const corrupted = zipBytes.slice();
    // Flip a byte inside the file's data region (right after its local header).
    const dataStart = entries[0].localHeaderOffset + 30 + "a.txt".length;
    corrupted[dataStart] ^= 0xff;

    let threw = false;
    try {
        for await (const _chunk of readZipEntry(corrupted, entries[0])) {
            /* draining */
        }
    } catch (e) {
        threw = true;
        assertEqual(e.message.includes("CRC-32"), true, "expected a CRC-32 failure message");
    }
    assertEqual(threw, true, "expected the corrupted entry to fail CRC verification");
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
