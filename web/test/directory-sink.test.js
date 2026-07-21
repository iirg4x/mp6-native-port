// Unit tests for js/directory-sink.js's path-splitting and directory-handle
// memoization, against fake objects shaped like the File System Access
// API (FileSystemDirectoryHandle/FileSystemFileHandle/
// FileSystemWritableFileStream). The real API only exists in a browser
// with a user-granted directory pick, so this is what stands in for it
// under Node; the real thing is covered by manual/browser testing (see
// MANUAL_TEST.md).

import { test, assertEqual, assertBytesEqual } from "./tiny-test.js";
import { DirectorySink } from "../js/directory-sink.js";

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

class FakeWritable {
    constructor(file) {
        this.file = file;
        this.chunks = [];
    }
    async write(chunk) {
        this.chunks.push(chunk.slice());
    }
    async close() {
        this.file.bytes = concat(this.chunks);
        this.file.closed = true;
    }
    async abort(message) {
        this.file.aborted = message;
    }
}

class FakeFileHandle {
    constructor(name) {
        this.name = name;
        this.bytes = null;
        this.closed = false;
    }
    async createWritable() {
        return new FakeWritable(this);
    }
}

class FakeDirHandle {
    constructor(name) {
        this.name = name;
        this.dirs = new Map();
        this.files = new Map();
        this.getDirectoryHandleCalls = 0;
    }
    async getDirectoryHandle(name, { create } = {}) {
        this.getDirectoryHandleCalls++;
        if (!this.dirs.has(name)) {
            if (!create) throw new Error(`not found: ${name}`);
            this.dirs.set(name, new FakeDirHandle(name));
        }
        return this.dirs.get(name);
    }
    async getFileHandle(name, { create } = {}) {
        if (!this.files.has(name)) {
            if (!create) throw new Error(`not found: ${name}`);
            this.files.set(name, new FakeFileHandle(name));
        }
        return this.files.get(name);
    }
}

async function* oneShot(bytes) {
    yield bytes;
}

test("DirectorySink: writes a deeply nested file, creating directories along the way", async () => {
    const root = new FakeDirHandle("root");
    const sink = new DirectorySink(root);
    const bytes = new TextEncoder().encode("hello");
    await sink.writeFile("content/GP6E01/sys/fst.bin", oneShot(bytes));

    const fstHandle = root.dirs.get("content").dirs.get("GP6E01").dirs.get("sys").files.get("fst.bin");
    assertEqual(fstHandle.closed, true);
    assertBytesEqual(fstHandle.bytes, bytes);
});

test("DirectorySink: a top-level file needs no directory lookups", async () => {
    const root = new FakeDirHandle("root");
    const sink = new DirectorySink(root);
    await sink.writeFile("mp6native.exe", oneShot(new TextEncoder().encode("exe")));
    assertEqual(root.getDirectoryHandleCalls, 0);
    assertEqual(root.files.get("mp6native.exe").closed, true);
});

test("DirectorySink: directory handles are memoized across multiple files sharing a prefix", async () => {
    const root = new FakeDirHandle("root");
    const sink = new DirectorySink(root);
    await sink.writeFile("content/GP6E01/files/data/a.bin", oneShot(new Uint8Array([1])));
    await sink.writeFile("content/GP6E01/files/data/b.bin", oneShot(new Uint8Array([2])));
    await sink.writeFile("content/GP6E01/files/mess/c.bin", oneShot(new Uint8Array([3])));

    // "content" and "GP6E01" and "files" are each looked up 3 times (once per
    // writeFile call, since only the resulting handles are memoized, not the
    // lookup calls themselves) -- what matters is that a fresh directory
    // object is never created twice for the same path.
    assertEqual(root.dirs.size, 1);
    assertEqual(root.dirs.get("content").dirs.size, 1);
    const filesDir = root.dirs.get("content").dirs.get("GP6E01").dirs.get("files");
    assertEqual(filesDir.dirs.size, 2); // "data" and "mess", each created exactly once
    assertEqual(filesDir.dirs.get("data").files.size, 2);
});

test("DirectorySink: a write error aborts the writable and propagates", async () => {
    const root = new FakeDirHandle("root");
    const sink = new DirectorySink(root);
    async function* failingChunks() {
        yield new Uint8Array([1]);
        throw new Error("simulated read failure");
    }
    let threw = false;
    try {
        await sink.writeFile("a.bin", failingChunks());
    } catch (e) {
        threw = true;
        assertEqual(e.message, "simulated read failure");
    }
    assertEqual(threw, true);
    assertEqual(root.files.get("a.bin").aborted, "simulated read failure");
});
