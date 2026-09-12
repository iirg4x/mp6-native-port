// MP6 web packager -- minimal ZIP reader for the fetched engine release
// asset (js/github-release.js) or a user-provided engine zip. Supports
// STORE (method 0, needed to read back this project's own zip-writer.js
// output for the round-trip test) and DEFLATE (method 8, what a normal zip
// tool like `zip`/Compress-Archive/GitHub's own archive step produces for
// the small engine build). DEFLATE decompression uses the browser/Node
// native `DecompressionStream('deflate-raw')` rather than shipping a hand
// written inflate -- see readZipEntry()'s comment for why, and the
// packager's report for the resulting test-coverage boundary (the STORE
// path round-trips under Node; the DEFLATE path is exercised live in the
// browser against a real engine zip, not by the Node test suite).
//
// The whole zip is expected as one in-memory Uint8Array. That's fine here:
// unlike the multi-hundred-MB disc image, the engine release asset is a
// small (tens of MB) Windows build or an APK, already fully buffered by
// the fetch() (or file input) that produced it.

import { crc32Update } from "./crc32.js";

const EOCD_SIG = 0x06054b50;
const CENTRAL_HEADER_SIG = 0x02014b50;
const LOCAL_HEADER_SIG = 0x04034b50;
const EOCD_MIN_SIZE = 22;
const MAX_COMMENT = 0xffff;

const textDecoder = new TextDecoder("utf-8");

function findEndOfCentralDirectory(bytes) {
    const maxBack = Math.min(bytes.length, EOCD_MIN_SIZE + MAX_COMMENT);
    for (let back = EOCD_MIN_SIZE; back <= maxBack; back++) {
        const pos = bytes.length - back;
        if (
            bytes[pos] === 0x50 &&
            bytes[pos + 1] === 0x4b &&
            bytes[pos + 2] === 0x05 &&
            bytes[pos + 3] === 0x06
        ) {
            return pos;
        }
    }
    return -1;
}

/**
 * Parses a complete ZIP archive's central directory. Returns
 * `{ entries }`, where each entry is
 * `{ path, method, crc32, compressedSize, uncompressedSize, localHeaderOffset }`.
 */
export function parseZip(bytes) {
    const eocdPos = findEndOfCentralDirectory(bytes);
    if (eocdPos === -1) {
        throw new Error("Not a ZIP file (no End Of Central Directory record found)");
    }
    const eocd = new DataView(bytes.buffer, bytes.byteOffset + eocdPos, bytes.byteLength - eocdPos);
    const totalEntries = eocd.getUint16(10, true);
    const centralDirOffset = eocd.getUint32(16, true);

    const entries = [];
    let pos = centralDirOffset;
    for (let i = 0; i < totalEntries; i++) {
        const view = new DataView(bytes.buffer, bytes.byteOffset + pos, 46);
        const sig = view.getUint32(0, true);
        if (sig !== CENTRAL_HEADER_SIG) {
            throw new Error(`Malformed ZIP central directory (bad signature for entry ${i} at ${pos})`);
        }
        const method = view.getUint16(10, true);
        const crc = view.getUint32(16, true);
        const compressedSize = view.getUint32(20, true);
        const uncompressedSize = view.getUint32(24, true);
        const nameLen = view.getUint16(28, true);
        const extraLen = view.getUint16(30, true);
        const commentLen = view.getUint16(32, true);
        const localHeaderOffset = view.getUint32(42, true);
        const nameStart = pos + 46;
        const path = textDecoder.decode(bytes.subarray(nameStart, nameStart + nameLen));
        entries.push({ path, method, crc32: crc, compressedSize, uncompressedSize, localHeaderOffset });
        pos = nameStart + nameLen + extraLen + commentLen;
    }
    return { entries };
}

/** Locates an entry's raw (still-compressed) data within the zip buffer. */
function entryDataSlice(bytes, entry) {
    const view = new DataView(bytes.buffer, bytes.byteOffset + entry.localHeaderOffset, 30);
    const sig = view.getUint32(0, true);
    if (sig !== LOCAL_HEADER_SIG) {
        throw new Error(`Malformed ZIP local header for "${entry.path}"`);
    }
    const nameLen = view.getUint16(26, true);
    const extraLen = view.getUint16(28, true);
    const dataStart = entry.localHeaderOffset + 30 + nameLen + extraLen;
    return bytes.subarray(dataStart, dataStart + entry.compressedSize);
}

/**
 * Async-iterates one entry's decompressed bytes in chunks. `verifyCrc`
 * (default true) checks the CRC-32 of the fully decompressed data against
 * the central directory's recorded value once the last chunk has been
 * yielded, throwing if it doesn't match (a corrupt download/zip).
 */
export async function* readZipEntry(bytes, entry, { verifyCrc = true, chunkSize = 1024 * 1024 } = {}) {
    const raw = entryDataSlice(bytes, entry);
    let crc = 0;
    let total = 0;

    if (entry.method === 0) {
        for (let off = 0; off < raw.length; off += chunkSize) {
            const chunk = raw.subarray(off, Math.min(off + chunkSize, raw.length));
            if (verifyCrc) crc = crc32Update(crc, chunk);
            total += chunk.length;
            yield chunk;
        }
    } else if (entry.method === 8) {
        if (typeof DecompressionStream === "undefined") {
            throw new Error(
                `"${entry.path}" is DEFLATE-compressed and this browser has no DecompressionStream ` +
                    "support to decode it. Use a recent Chrome/Edge/Firefox/Safari."
            );
        }
        const ds = new DecompressionStream("deflate-raw");
        const stream = new Blob([raw]).stream().pipeThrough(ds);
        for await (const chunk of stream) {
            const u8 = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
            if (verifyCrc) crc = crc32Update(crc, u8);
            total += u8.length;
            yield u8;
        }
    } else {
        throw new Error(`"${entry.path}" uses unsupported ZIP compression method ${entry.method}`);
    }

    if (total !== entry.uncompressedSize) {
        throw new Error(
            `"${entry.path}" decompressed to ${total} bytes, expected ${entry.uncompressedSize} -- corrupt archive?`
        );
    }
    if (verifyCrc && crc !== entry.crc32) {
        throw new Error(`"${entry.path}" failed its CRC-32 check -- corrupt archive?`);
    }
}
