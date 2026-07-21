// LOCAL-ONLY test helper: a File/Blob-shaped ByteSource (byte-source.js)
// backed by a real fs file handle instead of an in-memory buffer, used
// only by web/test/integration/real-iso.test.js so a multi-GB real ISO
// can be exercised by the exact same js/iso-source.js code the browser
// runs, WITHOUT reading the whole file into memory -- every slice() reads
// only the requested byte range off disk. Not used by anything that ships.

import { open } from "node:fs/promises";

export class NodeFileShim {
    constructor(path, size, handle) {
        this._path = path;
        this.size = size;
        this._handle = handle;
    }

    static async open(path) {
        const handle = await open(path, "r");
        const stat = await handle.stat();
        return new NodeFileShim(path, stat.size, handle);
    }

    slice(start, end) {
        const length = (end === undefined ? this.size : end) - start;
        const handle = this._handle;
        return {
            async arrayBuffer() {
                if (length === 0) return new ArrayBuffer(0);
                const buf = Buffer.alloc(length);
                const { bytesRead } = await handle.read(buf, 0, length, start);
                if (bytesRead !== length) {
                    throw new Error(`short read at offset ${start}: wanted ${length} bytes, got ${bytesRead}`);
                }
                return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
            },
        };
    }

    async close() {
        await this._handle.close();
    }
}
