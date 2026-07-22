// MP6 web packager -- orchestration. Combines a validated disc source
// (iso-source.js or folder-source.js), an optional engine zip
// (github-release.js), and an output sink (directory-sink.js or
// zip-stream-sink.js) into one progress-reporting, cancellable run.
//
// Output layout mirrors mp6-native's own first-run importer exactly
// (platform/content/content_import.cpp): engine files land at the output
// root (mp6native.exe, its DLLs, res/...) and disc content lands under
// content/GP6E01/{sys/boot.bin, sys/fst.bin, files/...}. The write order
// -- every wanted file, then sys/boot.bin, then sys/fst.bin dead last --
// reproduces content_import.cpp's "torn-import safety" contract (its
// comment: fst.bin is the single load-bearing probe file every content
// check looks for, so it is written last, meaning an interrupted run never
// presents as bootable content). That matters here too: a directory-picker
// output can be interrupted by a revoked permission, a closed tab, or an
// explicit cancel, and the same invariant keeps a partial folder honestly
// non-bootable.

import { readZipEntry } from "./zip-reader.js";

export const STATE = {
    IDLE: "idle",
    RUNNING: "running",
    DONE: "done",
    FAILED: "failed",
    CANCELLED: "cancelled",
};

export class CancelledError extends Error {
    constructor() {
        super("cancelled");
        this.name = "CancelledError";
    }
}

const CONTENT_ROOT = "content/GP6E01";

async function* oneShotChunks(bytes) {
    yield bytes;
}

/**
 * Builds the ordered list of `{ path, size, open() }` output items for one
 * run. `open()` returns a fresh async-iterable of Uint8Array chunks.
 * `engineZip` is `{ bytes, entries }` (parseZip()'s result plus the raw
 * bytes it parsed) or null for a content-only run.
 */
export function buildOutputItems(discSource, engineZip) {
    const items = [];

    if (engineZip) {
        for (const entry of engineZip.entries) {
            if (entry.path.endsWith("/")) continue; // directory entries are implied by file paths
            items.push({
                path: entry.path,
                size: entry.uncompressedSize,
                open: () => readZipEntry(engineZip.bytes, entry),
            });
        }
    }

    for (const entry of discSource.wantedFiles) {
        items.push({
            path: `${CONTENT_ROOT}/files/${entry.path}`,
            size: entry.size,
            open: () => discSource.readWantedFile(entry),
        });
    }
    if (discSource.bootBytes) {
        items.push({
            path: `${CONTENT_ROOT}/sys/boot.bin`,
            size: discSource.bootBytes.length,
            open: () => oneShotChunks(discSource.bootBytes),
        });
    }
    // fst.bin: always the very last item written. See file header comment.
    items.push({
        path: `${CONTENT_ROOT}/sys/fst.bin`,
        size: discSource.fstBytes.length,
        open: () => oneShotChunks(discSource.fstBytes),
    });

    return items;
}

/** A small mutable flag object the UI's Cancel button flips; runPackage() polls it between chunks. */
export function createCancelToken() {
    return { cancelled: false };
}

/**
 * Runs one packaging pass. `onProgress(status)` is called after every
 * state change and periodically while copying, with a snapshot shaped like
 * mp6-native's own Mp6ImportStatus (content_import.h) for a direct
 * conceptual parallel -- that struct is polled once/frame by the native UI;
 * this is pushed via callback instead, the natural idiom for async JS, but
 * carries the same fields: state/bytesDone/bytesTotal/filesDone/
 * filesTotal/currentFile/error.
 *
 * Returns the final status. Never throws for expected failures (a write
 * error, a cancel) -- those come back as status.state === FAILED/CANCELLED
 * with status.error set; only a programming error throws.
 */
export async function runPackage({ discSource, engineZip, sink, cancelToken, onProgress }) {
    const items = buildOutputItems(discSource, engineZip);
    const bytesTotal = items.reduce((sum, i) => sum + i.size, 0);
    const filesTotal = items.length;

    const status = {
        state: STATE.RUNNING,
        bytesDone: 0,
        bytesTotal,
        filesDone: 0,
        filesTotal,
        currentFile: "",
        error: "",
        skippedCount: 0, // entries the sink rejected as unsafe (see directory-sink.js's isSafeRelPath gate)
    };
    const emit = () => onProgress && onProgress({ ...status });
    // Sinks that validate paths (currently DirectorySink) expose the
    // rejected ones on `skippedPaths`; sinks that don't (ZipStreamSink)
    // simply leave the count at 0.
    const syncSkippedCount = () => {
        if (Array.isArray(sink.skippedPaths)) status.skippedCount = sink.skippedPaths.length;
    };
    emit();

    try {
        for (const item of items) {
            if (cancelToken && cancelToken.cancelled) throw new CancelledError();
            status.currentFile = item.path;
            emit();

            await sink.writeFile(
                item.path,
                (async function* () {
                    for await (const chunk of item.open()) {
                        if (cancelToken && cancelToken.cancelled) throw new CancelledError();
                        status.bytesDone += chunk.length;
                        emit();
                        yield chunk;
                    }
                })()
            );

            status.filesDone += 1;
            emit();
        }

        await sink.finish();
        status.state = STATE.DONE;
        status.currentFile = "";
        syncSkippedCount();
        emit();
        return status;
    } catch (e) {
        if (sink.abort) {
            try {
                await sink.abort(e && e.message ? e.message : String(e));
            } catch {
                /* best-effort cleanup only */
            }
        }
        if (e instanceof CancelledError) {
            status.state = STATE.CANCELLED;
        } else {
            status.state = STATE.FAILED;
            status.error = e && e.message ? e.message : String(e);
        }
        syncSkippedCount();
        emit();
        return status;
    }
}
