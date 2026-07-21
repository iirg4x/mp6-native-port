// MP6 web packager -- output sink for the Chromium tier: writes straight
// into a user-chosen folder via the File System Access API
// (showDirectoryPicker() + FileSystemWritableFileStream), so the whole
// engine+content tree lands on disk incrementally with no in-page
// buffering and no intermediate ZIP at all -- the same end state
// mp6native.exe's own first-run importer produces
// (content_import.cpp:220-241's dest_disc_root(), "content/GP6E01" next to
// the exe), just written to a folder the user picked up front instead of
// discovered by the native launcher afterward.

export class DirectorySink {
    constructor(rootHandle) {
        this.root = rootHandle;
        this._dirs = new Map(); // "a/b" -> DirectoryHandle, memoized
    }

    async _getDirectory(pathParts) {
        let handle = this.root;
        let key = "";
        for (const part of pathParts) {
            key = key ? `${key}/${part}` : part;
            const cached = this._dirs.get(key);
            if (cached) {
                handle = cached;
                continue;
            }
            handle = await handle.getDirectoryHandle(part, { create: true });
            this._dirs.set(key, handle);
        }
        return handle;
    }

    /** Writes one file (forward-slash path, relative to the chosen root) from an async-iterable of chunks. */
    async writeFile(path, chunks) {
        const parts = path.split("/").filter(Boolean);
        const fileName = parts.pop();
        const dir = await this._getDirectory(parts);
        const fileHandle = await dir.getFileHandle(fileName, { create: true });
        const writable = await fileHandle.createWritable();
        try {
            for await (const chunk of chunks) {
                await writable.write(chunk);
            }
        } catch (e) {
            await writable.abort(String(e && e.message ? e.message : e));
            throw e;
        }
        await writable.close();
    }

    // Nothing to finalize -- every file is already durably written by its
    // own writable.close() above.
    async finish() {}
}

/** True when the File System Access API's directory picker is usable here. */
export function directoryPickerAvailable() {
    return typeof window !== "undefined" && typeof window.showDirectoryPicker === "function";
}
