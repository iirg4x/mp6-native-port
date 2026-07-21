// MP6 web packager -- output sink for the fallback tier: wraps ZipWriter
// (zip-writer.js) around a downloadSink (stream-download.js), so the same
// writeFile()/finish() surface DirectorySink offers in the Chromium tier
// also works here -- js/packager.js does not need to know which tier it's
// driving.

import { ZipWriter } from "./zip-writer.js";

export class ZipStreamSink {
    constructor(downloadSink) {
        this._download = downloadSink;
        this._zip = new ZipWriter((chunk) => this._download.write(chunk));
    }

    async writeFile(path, chunks) {
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
