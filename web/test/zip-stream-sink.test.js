// Unit tests for js/zip-stream-sink.js's path-safety gate, mirroring the
// DirectorySink tests in directory-sink.test.js: the same isSafeRelPath()
// gate (path-safe.js) must behave identically on this sink -- skip +
// record, never throw, never abort the run -- so a Firefox/Safari user
// (fallback ZIP tier) gets the same protection a Chromium user
// (DirectorySink tier) does. Uses a fake downloadSink shaped like
// stream-download.js's sink surface (write/close/abort) so no service
// worker or real download is needed.

import { test, assertEqual, assertDeepEqual } from "./tiny-test.js";
import { ZipStreamSink } from "../js/zip-stream-sink.js";
import { parseZip, readZipEntry } from "../js/zip-reader.js";

class FakeDownloadSink {
    constructor() {
        this.chunks = [];
        this.closed = false;
        this.aborted = null;
    }
    async write(chunk) {
        this.chunks.push(chunk.slice());
    }
    async close() {
        this.closed = true;
    }
    async abort(message) {
        this.aborted = message;
    }
    bytes() {
        return concat(this.chunks);
    }
}

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

async function* neverRead() {
    throw new Error("must not be read: writeFile should reject before touching chunks");
}

async function* oneShot(bytes) {
    yield bytes;
}

test("ZipStreamSink: an unsafe path is skipped -- not emitted into the zip, not thrown -- and recorded", async () => {
    const download = new FakeDownloadSink();
    const sink = new ZipStreamSink(download);
    await sink.writeFile("content/GP6E01/files/data/../../evil", neverRead());
    assertDeepEqual(sink.skippedPaths, ["content/GP6E01/files/data/../../evil"]);

    await sink.finish();
    assertEqual(download.closed, true);
    const { entries } = parseZip(download.bytes());
    assertEqual(entries.length, 0, "the unsafe entry must never reach the archive (zip-slip prevention)");
});

test("ZipStreamSink: multiple unsafe paths across a run do not abort later safe writes", async () => {
    const download = new FakeDownloadSink();
    const sink = new ZipStreamSink(download);
    await sink.writeFile("/abs/evil", neverRead());
    await sink.writeFile("data/x.bin", oneShot(new TextEncoder().encode("ok")));
    await sink.writeFile("..\\win", neverRead());
    await sink.finish();

    assertDeepEqual(sink.skippedPaths, ["/abs/evil", "..\\win"]);

    const zipBytes = download.bytes();
    const { entries } = parseZip(zipBytes);
    assertEqual(entries.length, 1, "only the one safe entry should be in the archive");
    assertEqual(entries[0].path, "data/x.bin");

    const chunks = [];
    for await (const chunk of readZipEntry(zipBytes, entries[0])) chunks.push(chunk);
    assertEqual(new TextDecoder().decode(concat(chunks)), "ok");
});
