// MP6 web packager -- chunked reads over an abstract "byte source".
//
// A ByteSource is anything shaped like a browser Blob/File: a `.size`
// number and a `.slice(start, end)` method returning an object with an
// `.arrayBuffer()` promise. Browser File objects (from <input type=file> or
// a File System Access API handle) already satisfy this exactly, so the
// packager code that walks a disc image never has to know whether it is
// running in the browser or (for the local-only integration test) under
// Node against a fs-backed shim with the same three members -- see
// web/test/node-file-shim.js.
//
// This is the layer that guarantees the brief's "never load 1.4GB into
// memory" rule: every read goes through readRange()/iterateChunks() below,
// which only ever materializes one chunk (default 8MB) at a time.

export const DEFAULT_CHUNK_SIZE = 8 * 1024 * 1024; // 8MB

/** Reads exactly `length` bytes starting at `start` as one Uint8Array. */
export async function readRange(source, start, length) {
    if (start < 0 || length < 0 || start + length > source.size) {
        throw new Error(
            `readRange(${start}, ${length}) is out of bounds for a ${source.size}-byte source`
        );
    }
    if (length === 0) return new Uint8Array(0);
    const blob = source.slice(start, start + length);
    const buf = await blob.arrayBuffer();
    return new Uint8Array(buf);
}

/**
 * Async-iterates [start, start+length) in `chunkSize`-sized Uint8Array
 * pieces (the last piece may be shorter). Used for every large-file copy in
 * the packager so memory use stays bounded regardless of file size.
 */
export async function* iterateChunks(source, start, length, chunkSize = DEFAULT_CHUNK_SIZE) {
    if (start < 0 || length < 0 || start + length > source.size) {
        throw new Error(
            `iterateChunks(${start}, ${length}) is out of bounds for a ${source.size}-byte source`
        );
    }
    let offset = 0;
    while (offset < length) {
        const thisLen = Math.min(chunkSize, length - offset);
        const blob = source.slice(start + offset, start + offset + thisLen);
        const buf = await blob.arrayBuffer();
        yield new Uint8Array(buf);
        offset += thisLen;
    }
}
