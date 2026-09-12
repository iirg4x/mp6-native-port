// Unit tests for js/folder-source.js against an in-memory fake FileList,
// covering the same "which folder shape is the disc root" logic as
// mp6-native's content_import.cpp:366-381 (folder_disc_root) without
// touching a real filesystem.

import { test, assertEqual, assertDeepEqual } from "./tiny-test.js";
import { openFolderSource } from "../js/folder-source.js";

class FakeFile {
    constructor(webkitRelativePath, bytes) {
        this.webkitRelativePath = webkitRelativePath;
        this.name = webkitRelativePath.split("/").pop();
        this._bytes = bytes;
        this.size = bytes.length;
    }
    slice(start, end) {
        const b = this._bytes.subarray(start, end === undefined ? this._bytes.length : end);
        return {
            async arrayBuffer() {
                return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
            },
        };
    }
    async arrayBuffer() {
        return this._bytes.buffer.slice(this._bytes.byteOffset, this._bytes.byteOffset + this._bytes.byteLength);
    }
}

function textBytes(s) {
    return new TextEncoder().encode(s);
}

function bootBinWithGameId(id) {
    const b = new Uint8Array(0x440);
    b.set(textBytes(id), 0);
    return b;
}

function makeFileList(prefix, { withBoot = true, gameId = "GP6E01" } = {}) {
    const files = [
        new FakeFile(`${prefix}sys/fst.bin`, textBytes("fake-fst-bytes")),
        new FakeFile(`${prefix}files/opening.bnr`, textBytes("banner")),
        new FakeFile(`${prefix}files/data/title.bin`, textBytes("title-data-longer-than-a-few-bytes")),
        new FakeFile(`${prefix}files/movie/intro.thp`, textBytes("movie-not-wanted")),
        new FakeFile(`${prefix}files/sound/MP6_SND.msm`, textBytes("snd")),
    ];
    if (withBoot) files.push(new FakeFile(`${prefix}sys/boot.bin`, bootBinWithGameId(gameId)));
    return files;
}

test("openFolderSource: picked folder itself is the disc root (no GP6E01/DATA wrapper)", async () => {
    const files = makeFileList("MyDump/");
    const result = await openFolderSource(files);
    assertEqual(result.ok, true);
    assertDeepEqual(
        result.source.wantedFiles.map((f) => f.path).sort(),
        ["data/title.bin", "opening.bnr", "sound/MP6_SND.msm"]
    );
});

test("openFolderSource: disc root one level down at <picked>/GP6E01", async () => {
    const files = makeFileList("SomeDump/GP6E01/");
    const result = await openFolderSource(files);
    assertEqual(result.ok, true);
    assertEqual(result.source.wantedFiles.length, 3);
});

test("openFolderSource: disc root one level down at <picked>/DATA (Dolphin dump style)", async () => {
    const files = makeFileList("Riivolution/DATA/");
    const result = await openFolderSource(files);
    assertEqual(result.ok, true);
    assertEqual(result.source.wantedFiles.length, 3);
});

test("openFolderSource: missing sys/fst.bin fails with a clear message", async () => {
    const files = makeFileList("MyDump/").filter((f) => !f.webkitRelativePath.endsWith("fst.bin"));
    const result = await openFolderSource(files);
    assertEqual(result.ok, false);
    assertEqual(result.error.includes("doesn't look like an extracted GameCube disc"), true);
});

test("openFolderSource: wrong game ID in sys/boot.bin fails validation", async () => {
    const files = makeFileList("MyDump/", { gameId: "GP7E01" });
    const result = await openFolderSource(files);
    assertEqual(result.ok, false);
    assertEqual(result.error.includes("Not Mario Party 6"), true);
});

test("openFolderSource: missing sys/boot.bin is tolerated (not fatal)", async () => {
    const files = makeFileList("MyDump/", { withBoot: false });
    const result = await openFolderSource(files);
    assertEqual(result.ok, true);
});

test("openFolderSource: empty file list fails cleanly", async () => {
    const result = await openFolderSource([]);
    assertEqual(result.ok, false);
});
