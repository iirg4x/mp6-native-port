// MP6 web packager -- minimal STORE-only streaming ZIP writer.
//
// Used by the fallback (non-Chromium) output tier: the packager already
// reads every input file in bounded chunks (byte-source.js), and this
// writer never buffers a whole entry either, so total memory stays
// O(chunk size) regardless of the ~400MB+ output. No compression is
// implemented on purpose -- the brief's content is already-compressed
// game data (audio containers, the game's own zlib/LZ-coded archives) and
// the fetched engine build, so DEFLATE would spend CPU for close to
// nothing. Every entry is written with the streaming "data descriptor"
// technique (general-purpose flag bit 3): the local header is emitted
// before the entry's size/CRC-32 are known (they aren't, until every chunk
// has streamed past), with the real values following the file's bytes in a
// data descriptor record, and again authoritatively in the central
// directory at the very end. This is a standard, widely-supported part of
// the ZIP spec (PKWARE APPNOTE.TXT 4.3.9) -- Windows Explorer, 7-Zip,
// PowerShell's Expand-Archive and `unzip` all read archives built this way.
//
// No zip64: every field here is a plain 32-bit ZIP. Fine for this project's
// sizes (~400-450MB total, largest single entry ~150MB, low hundreds of
// entries) -- all comfortably under the 4GB/65535-entry ZIP32 limits.

import { crc32Update } from "./crc32.js";

const LOCAL_HEADER_SIG = 0x04034b50;
const DATA_DESCRIPTOR_SIG = 0x08074b50;
const CENTRAL_HEADER_SIG = 0x02014b50;
const EOCD_SIG = 0x06054b50;

const FLAG_DATA_DESCRIPTOR = 0x0008;
const FLAG_UTF8 = 0x0800;
const VERSION_NEEDED = 20;

const textEncoder = new TextEncoder();

/** Encodes a Date as classic DOS date/time (used by every ZIP timestamp field). */
function dosDateTime(date) {
    const time =
        ((date.getHours() & 0x1f) << 11) | ((date.getMinutes() & 0x3f) << 5) | ((date.getSeconds() >> 1) & 0x1f);
    const day =
        (((date.getFullYear() - 1980) & 0x7f) << 9) | (((date.getMonth() + 1) & 0xf) << 5) | (date.getDate() & 0x1f);
    return { time, date: day };
}

function u16(n) {
    const b = new Uint8Array(2);
    new DataView(b.buffer).setUint16(0, n, true);
    return b;
}
function u32(n) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, n >>> 0, true);
    return b;
}
function concat(...parts) {
    const total = parts.reduce((s, p) => s + p.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

/**
 * `sink(chunk: Uint8Array) -> void | Promise<void>` receives every output
 * byte, in order, exactly once. It is the only place bytes leave this
 * class, so it is what makes both output tiers work: a DirectorySink-style
 * caller can also just use a plain concatenating sink for round-trip tests
 * (see web/test/zip-roundtrip.test.js).
 */
export class ZipWriter {
    constructor(sink) {
        this._sink = sink;
        this._offset = 0;
        this._records = [];
        this._current = null;
        this._finished = false;
    }

    async _write(bytes) {
        await this._sink(bytes);
        this._offset += bytes.length;
    }

    /** Begins a new entry. Must be followed by writeChunk() calls, then endFile(). */
    async beginFile(path, when = new Date()) {
        if (this._current) throw new Error("beginFile() called while a previous entry is still open");
        const nameBytes = textEncoder.encode(path.replace(/\\/g, "/"));
        const { time, date } = dosDateTime(when);
        const flags = FLAG_DATA_DESCRIPTOR | FLAG_UTF8;
        const header = concat(
            u32(LOCAL_HEADER_SIG),
            u16(VERSION_NEEDED),
            u16(flags),
            u16(0), // compression method: 0 = store
            u16(time),
            u16(date),
            u32(0), // crc-32: deferred (flag bit 3)
            u32(0), // compressed size: deferred
            u32(0), // uncompressed size: deferred
            u16(nameBytes.length),
            u16(0), // extra field length
            nameBytes
        );
        const localHeaderOffset = this._offset;
        await this._write(header);
        this._current = { path, nameBytes, crc: 0, size: 0, localHeaderOffset, flags, time, date };
    }

    /** Streams one chunk of the current entry's raw (uncompressed) bytes. */
    async writeChunk(chunk) {
        if (!this._current) throw new Error("writeChunk() called with no open entry");
        this._current.crc = crc32Update(this._current.crc, chunk);
        this._current.size += chunk.length;
        await this._write(chunk);
    }

    /** Convenience: streams an entire async-iterable of chunks as one entry. */
    async writeFile(path, chunks, when = new Date()) {
        await this.beginFile(path, when);
        for await (const chunk of chunks) {
            await this.writeChunk(chunk);
        }
        await this.endFile();
    }

    /** Closes the current entry: writes its data descriptor, records it for the central directory. */
    async endFile() {
        if (!this._current) throw new Error("endFile() called with no open entry");
        const c = this._current;
        const descriptor = concat(u32(DATA_DESCRIPTOR_SIG), u32(c.crc), u32(c.size), u32(c.size));
        await this._write(descriptor);
        this._records.push(c);
        this._current = null;
    }

    /** Writes the central directory + EOCD. Must be called exactly once, after every entry is closed. */
    async finish() {
        if (this._current) throw new Error("finish() called while an entry is still open");
        if (this._finished) throw new Error("finish() called twice");
        this._finished = true;

        const centralDirStart = this._offset;
        for (const r of this._records) {
            const header = concat(
                u32(CENTRAL_HEADER_SIG),
                u16(VERSION_NEEDED), // version made by
                u16(VERSION_NEEDED), // version needed to extract
                u16(r.flags),
                u16(0), // compression method: store
                u16(r.time),
                u16(r.date),
                u32(r.crc),
                u32(r.size), // compressed size == uncompressed size (store)
                u32(r.size),
                u16(r.nameBytes.length),
                u16(0), // extra field length
                u16(0), // comment length
                u16(0), // disk number start
                u16(0), // internal file attributes
                u32(0o100644 << 16), // external file attributes: regular file, rw-r--r--
                u32(r.localHeaderOffset),
                r.nameBytes
            );
            await this._write(header);
        }
        const centralDirSize = this._offset - centralDirStart;

        const eocd = concat(
            u32(EOCD_SIG),
            u16(0), // this disk
            u16(0), // disk with central dir start
            u16(this._records.length),
            u16(this._records.length),
            u32(centralDirSize),
            u32(centralDirStart),
            u16(0) // comment length
        );
        await this._write(eocd);
    }
}
