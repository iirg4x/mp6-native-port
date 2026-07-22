// MP6 web packager -- output sink for the fallback tier: wraps ZipWriter
// (zip-writer.js) around a downloadSink (stream-download.js), so the same
// writeFile()/finish() surface DirectorySink offers in the Chromium tier
// also works here -- js/packager.js does not need to know which tier it's
// driving.

import { ZipWriter } from "./zip-writer.js";
import { isSafeRelPath } from "./path-safe.js";

export class ZipStreamSink {
    constructor(downloadSink) {
        this._download = downloadSink;
        this._zip = new ZipWriter((chunk) => this._download.write(chunk));
        this.skippedPaths = []; // SECURITY: paths isSafeRelPath() rejected, never added to the zip -- see writeFile()
    }

    /**
     * SECURITY: `path` is ultimately built from attacker-controlled disc/
     * folder entry names (fst.js / folder-source.js) -- the same untrusted
     * input DirectorySink guards against (see its writeFile() comment).
     * Unlike the File System Access API tier, ZipWriter.beginFile() does
     * NOT reject an unsafe name itself: it would happily emit a
     * "../"-carrying entry into the archive, which is a classic zip-slip
     * waiting to happen the moment the user's own unzip tool extracts it --
     * a worse failure mode than the FS Access tier's thrown error, since it
     * fails silently instead of loudly. isSafeRelPath() is the same single
     * gate DirectorySink uses: a rejected path is skipped and logged
     * instead of ever reaching the archive, and the run continues.
     */
    async writeFile(path, chunks) {
        if (!isSafeRelPath(path)) {
            this.skippedPaths.push(path);
            console.warn(`[packager] skipping unsafe output path (possible disc path traversal): ${JSON.stringify(path)}`);
            return;
        }
        await this._zip.beginFile(path);
        for await (const chunk of chunks) {
            await this._zip.writeChunk(chunk);
        }
        await this._zip.endFile();
    }

    async finish() {
        await this._zip.finish();
        await this._download.close();
    }

    async abort(message) {
        await this._download.abort(message);
    }
}
