// LOCAL-ONLY integration test: runs js/iso-source.js (the exact browser
// code path) against a real "Mario Party 6 (USA).iso" via NodeFileShim, so
// the FST parser and wanted-set filter get exercised against real retail
// disc data, not just the synthetic fixture. Nothing about the ISO or its
// extracted content is read into this repo or committed anywhere --
// NodeFileShim streams bounded-size slices straight off disk.
//
// Skipped entirely (not failed) unless $MP6_ISO points at a real image.
// Following this project's own convention (tools/mp6scene/README.md /
// catalog.py's --disc flag both honor $MP6_ISO for the same purpose).
//
// The expected numbers below (961 FST entries, 527 wanted files,
// 418572707 wanted bytes) were measured against the real USA disc while
// building this feature -- see the feature report for the exact commands.
// The task brief's own estimate ("819-ish total FST entries") undershoots
// what this disc actually contains (945 files + 16 directories including
// root = 961); this test asserts the measured truth.

import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { testIf, assertEqual, assertBytesEqual } from "../tiny-test.js";
import { NodeFileShim } from "../node-file-shim.js";
import { openIsoSource } from "../../js/iso-source.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ISO_PATH = process.env.MP6_ISO;

const EXPECTED_NUM_ENTRIES = 961; // 945 files + 16 directories (incl. root)
const EXPECTED_WANTED_FILES = 527;
const EXPECTED_WANTED_BYTES = 418572707; // ~399.1 MiB / ~418.6 MB

async function fileExists(p) {
    try {
        await fs.access(p);
        return true;
    } catch {
        return false;
    }
}

/** Best-effort: locate a pre-extracted GP6E01 tree for the byte-identical comparison. Optional. */
async function findReferenceTree() {
    const candidates = [
        process.env.MP6_REFERENCE_TREE,
        // This workspace's own layout: <_mp6_rebuild>/external_refs/repos/marioparty6/orig/GP6E01,
        // sibling to <_mp6_rebuild>/port/mp6-port-clean (this repo).
        path.resolve(__dirname, "../../../../../external_refs/repos/marioparty6/orig/GP6E01"),
    ].filter(Boolean);
    for (const candidate of candidates) {
        if (await fileExists(path.join(candidate, "sys", "fst.bin"))) return candidate;
    }
    return null;
}

testIf(
    !!ISO_PATH,
    "integration: real MP6 (USA) ISO parses with the expected structure",
    async () => {
        const shim = await NodeFileShim.open(ISO_PATH);
        try {
            const result = await openIsoSource(shim);
            assertEqual(result.ok, true, result.ok ? "" : result.error);
            const { source } = result;

            const wantedBytes = source.wantedFiles.reduce((sum, f) => sum + f.size, 0);
            console.log(`    [integration] source: ${ISO_PATH}`);
            console.log(`    [integration] game ID: ${source.header.gameId}`);
            console.log(
                `    [integration] FST: offset 0x${source.header.fstOffset.toString(16)}, ` +
                    `size ${source.header.fstSize} bytes, ${source.numEntries} entries`
            );
            console.log(`    [integration] wanted set: ${source.wantedFiles.length} files, ${wantedBytes} bytes`);

            assertEqual(source.header.gameId, "GP6E01", "game ID");
            assertEqual(source.numEntries, EXPECTED_NUM_ENTRIES, "total FST entry count");
            assertEqual(source.wantedFiles.length, EXPECTED_WANTED_FILES, "wanted-set file count");
            assertEqual(wantedBytes, EXPECTED_WANTED_BYTES, "wanted-set total bytes");

            const refRoot = await findReferenceTree();
            if (refRoot) {
                const refFst = new Uint8Array(await fs.readFile(path.join(refRoot, "sys", "fst.bin")));
                assertBytesEqual(source.fstBytes, refFst, "sys/fst.bin vs reference tree");

                const refBanner = new Uint8Array(await fs.readFile(path.join(refRoot, "files", "opening.bnr")));
                const openingEntry = source.wantedFiles.find((f) => f.path === "opening.bnr");
                const chunks = [];
                for await (const chunk of source.readWantedFile(openingEntry)) chunks.push(chunk);
                const total = chunks.reduce((sum, c) => sum + c.length, 0);
                const openingBytes = new Uint8Array(total);
                let off = 0;
                for (const c of chunks) {
                    openingBytes.set(c, off);
                    off += c.length;
                }
                assertBytesEqual(openingBytes, refBanner, "files/opening.bnr vs reference tree");
                console.log(`    [integration] sys/fst.bin + files/opening.bnr byte-identical to ${refRoot}`);
            } else {
                console.log(
                    "    [integration] no reference tree found (checked $MP6_REFERENCE_TREE and the " +
                        "workspace-relative external_refs path) -- skipping the byte-identical comparison"
                );
            }
        } finally {
            await shim.close();
        }
    },
    ISO_PATH ? undefined : 'set $MP6_ISO to a real "Mario Party 6 (USA).iso" path to run this'
);
