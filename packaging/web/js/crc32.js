// MP6 web packager -- CRC-32 (ISO 3309 / ZIP polygon 0xEDB88320), used by
// both the ZIP reader (verifying decompressed entries) and the ZIP writer
// (STORE entries still need a real CRC-32 in the data descriptor / central
// directory -- see zip-writer.js).

let table = null;

function buildTable() {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) {
            c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        }
        t[n] = c >>> 0;
    }
    return t;
}

/**
 * Incremental CRC-32. Call with `crc = 0` to start, then feed successive
 * Uint8Array chunks; the return value is the running CRC (already
 * finalized -- i.e. already XORed), safe to pass straight back in as the
 * next call's `crc` (init/update/finalize are folded into one function
 * because every call site here processes a stream chunk-by-chunk anyway).
 */
export function crc32Update(crc, chunk) {
    if (table === null) table = buildTable();
    let c = (crc ^ 0xffffffff) >>> 0;
    for (let i = 0; i < chunk.length; i++) {
        c = table[(c ^ chunk[i]) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
}

/** One-shot CRC-32 of a single buffer. */
export function crc32(bytes) {
    return crc32Update(0, bytes);
}
