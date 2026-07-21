// Unit tests for js/packager.js: output item ordering/paths (mirroring
// content_import.cpp's "sys/fst.bin written last" torn-import contract),
// progress reporting, and cancellation -- all against a fake in-memory
// sink, so no directory picker or service worker is needed.

import { test, assertEqual, assertDeepEqual, assertBytesEqual } from "./tiny-test.js";
import { buildOutputItems, runPackage, createCancelToken, STATE } from "../js/packager.js";
import { openIsoSource } from "../js/iso-source.js";
import { buildSyntheticIso } from "./fixtures/make-fixture.js";
import { ZipWriter } from "../js/zip-writer.js";
import { parseZip } from "../js/zip-reader.js";

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

class RecordingSink {
    constructor() {
        this.written = [];
        this.finished = false;
        this.aborted = null;
    }
    async writeFile(path, chunks) {
        const parts = [];
        for await (const c of chunks) parts.push(c.slice());
        this.written.push({ path, bytes: concat(parts) });
    }
    async finish() {
        this.finished = true;
    }
    async abort(message) {
        this.aborted = message;
    }
}

async function openFixtureSource() {
    const fixture = buildSyntheticIso();
    const blob = new Blob([fixture.bytes]);
    const { source } = await openIsoSource(blob);
    return { fixture, source };
}

test("buildOutputItems: content-only run writes wanted files, then boot.bin, then fst.bin last", async () => {
    const { source } = await openFixtureSource();
    const items = buildOutputItems(source, null);
    assertEqual(items.length, source.wantedFiles.length + 2); // + sys/boot.bin + sys/fst.bin
    assertEqual(items[items.length - 1].path, "content/GP6E01/sys/fst.bin");
    assertEqual(items[items.length - 2].path, "content/GP6E01/sys/boot.bin");
    for (const item of items.slice(0, -2)) {
        assertEqual(item.path.startsWith("content/GP6E01/files/"), true, item.path);
    }
});

test("buildOutputItems: engine zip entries are included unprefixed, ahead of disc content", async () => {
    const { source } = await openFixtureSource();

    const parts = [];
    const writer = new ZipWriter((chunk) => parts.push(chunk));
    await writer.writeFile("mp6native.exe", oneShot(new TextEncoder().encode("fake-exe")));
    await writer.writeFile("res/rml/window.rcss", oneShot(new TextEncoder().encode("/* css */")));
    await writer.finish();
    const zipBytes = concat(parts);
    const { entries } = parseZip(zipBytes);

    const items = buildOutputItems(source, { bytes: zipBytes, entries });
    assertDeepEqual(items.slice(0, 2).map((i) => i.path), ["mp6native.exe", "res/rml/window.rcss"]);
    assertEqual(items[items.length - 1].path, "content/GP6E01/sys/fst.bin");
});

test("runPackage: writes every item with correct content and reports DONE with full progress", async () => {
    const { fixture, source } = await openFixtureSource();
    const sink = new RecordingSink();
    const status = await runPackage({ discSource: source, engineZip: null, sink });

    assertEqual(status.state, STATE.DONE);
    assertEqual(status.bytesDone, status.bytesTotal);
    assertEqual(status.filesDone, status.filesTotal);
    assertEqual(sink.finished, true);

    const fstWrite = sink.written.find((w) => w.path === "content/GP6E01/sys/fst.bin");
    assertBytesEqual(fstWrite.bytes, fixture.fstBytes, "fst.bin content");
    const titleWrite = sink.written.find((w) => w.path === "content/GP6E01/files/data/title.bin");
    const expectedTitle = fixture.wantedFiles.find((f) => f.path === "data/title.bin").content;
    assertBytesEqual(titleWrite.bytes, expectedTitle, "data/title.bin content");
});

test("runPackage: progress callback fires with monotonically increasing bytesDone up to the total", async () => {
    const { source } = await openFixtureSource();
    const sink = new RecordingSink();
    const seen = [];
    await runPackage({
        discSource: source,
        engineZip: null,
        sink,
        onProgress: (s) => seen.push(s.bytesDone),
    });
    for (let i = 1; i < seen.length; i++) {
        assertEqual(seen[i] >= seen[i - 1], true, `bytesDone went backwards at step ${i}`);
    }
    assertEqual(seen[seen.length - 1], source.totalWantedBytes);
});

test("runPackage: cancelling mid-run stops early and reports CANCELLED", async () => {
    const { source } = await openFixtureSource();
    const sink = new RecordingSink();
    const cancelToken = createCancelToken();
    let firstProgressSeen = false;
    const status = await runPackage({
        discSource: source,
        engineZip: null,
        sink,
        cancelToken,
        onProgress: () => {
            if (!firstProgressSeen) {
                firstProgressSeen = true;
                cancelToken.cancelled = true;
            }
        },
    });
    assertEqual(status.state, STATE.CANCELLED);
    assertEqual(sink.finished, false);
    assertEqual(sink.written.length < source.wantedFiles.length + 2, true, "expected an early stop");
});

test("runPackage: a sink failure is reported as FAILED with the error message, and abort() is called", async () => {
    const { source } = await openFixtureSource();
    const sink = new RecordingSink();
    sink.writeFile = async () => {
        throw new Error("disk full (simulated)");
    };
    const status = await runPackage({ discSource: source, engineZip: null, sink });
    assertEqual(status.state, STATE.FAILED);
    assertEqual(status.error.includes("disk full"), true);
    assertEqual(sink.aborted, "disk full (simulated)");
});

async function* oneShot(bytes) {
    yield bytes;
}
