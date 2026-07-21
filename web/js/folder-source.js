// MP6 web packager -- opens an already-extracted disc folder (picked via
// <input type="file" webkitdirectory>, which is how Dolphin's "Extract
// Entire Disc..." output gets in here -- there is no showDirectoryPicker()
// equivalent input for a FileList, so this is the one supported shape).
//
// Mirrors mp6-native's platform/content/content_import.cpp:366-509
// (folder_disc_root / import_folder_worker):
//   - the disc root is the picked folder itself, "<picked>/GP6E01", or
//     "<picked>/DATA" -- whichever one directly contains both sys/fst.bin
//     and a files/ directory (folder_disc_root's `candidates` array,
//     content_import.cpp:370);
//   - the game ID is read from sys/boot.bin's first 6 bytes if that file
//     exists, and tolerated (not fatal) if it doesn't, exactly like
//     import_folder_worker's comment at content_import.cpp:443-445;
//   - the wanted-set filter (wanted.js) is applied to every file under
//     "<discRoot>/files/", walking only into subtrees that could contain a
//     wanted file (content_import.cpp:415-421's folder_collect), though
//     here that pruning is just a minor optimization since a webkitdirectory
//     pick has already enumerated the whole flat FileList up front.
//
// A browser `File` object from webkitdirectory already satisfies the
// ByteSource shape (byte-source.js) directly -- it has `.size` and
// `.slice()` -- so wanted files here are read with the exact same
// iterateChunks() helper the ISO path uses.

import { readRange, iterateChunks, DEFAULT_CHUNK_SIZE } from "./byte-source.js";
import { GAME_ID_LENGTH } from "./fst.js";
import { validateGameId } from "./validate.js";
import { wantedFile } from "./wanted.js";

const ROOT_CANDIDATES = ["", "GP6E01", "DATA"];

/**
 * Strips the top-level picked-folder name off a webkitRelativePath, e.g.
 * "MyDump/GP6E01/sys/fst.bin" -> "GP6E01/sys/fst.bin". Falls back to `.name`
 * for File-like objects that don't carry webkitRelativePath (e.g. the
 * Node test shim), which is only meaningful for single-file lookups.
 */
function relPath(file) {
    const p = file.webkitRelativePath || file.name || "";
    const slash = p.indexOf("/");
    return slash === -1 ? p : p.slice(slash + 1);
}

/**
 * `files` is a FileList or plain array of File-like objects, as produced by
 * an `<input type="file" webkitdirectory>` pick. Returns
 * `{ ok: true, source: DiscSource }` or `{ ok: false, error }`.
 */
export async function openFolderSource(files) {
    const list = Array.from(files);
    if (list.length === 0) {
        return { ok: false, error: "That folder appears to be empty." };
    }

    const byPath = new Map();
    for (const f of list) byPath.set(relPath(f), f);

    let discRootPrefix = null;
    for (const candidate of ROOT_CANDIDATES) {
        const fstPath = candidate ? `${candidate}/sys/fst.bin` : "sys/fst.bin";
        const filesPrefix = candidate ? `${candidate}/files/` : "files/";
        const hasFst = byPath.has(fstPath);
        const hasFilesDir = list.some((f) => relPath(f).startsWith(filesPrefix));
        if (hasFst && hasFilesDir) {
            discRootPrefix = candidate;
            break;
        }
    }

    if (discRootPrefix === null) {
        return {
            ok: false,
            error:
                "That doesn't look like an extracted GameCube disc (need sys/fst.bin and a files/ folder " +
                "inside it, or a GP6E01 folder containing them). In Dolphin, right-click the game and use " +
                '"Extract Entire Disc..." to produce one.',
        };
    }

    const fstFile = byPath.get(discRootPrefix ? `${discRootPrefix}/sys/fst.bin` : "sys/fst.bin");
    const bootPath = discRootPrefix ? `${discRootPrefix}/sys/boot.bin` : "sys/boot.bin";
    const bootFile = byPath.get(bootPath) || null;

    if (bootFile) {
        const idBytes = await readRange(bootFile, 0, GAME_ID_LENGTH);
        let id = "";
        for (let i = 0; i < idBytes.length; i++) id += String.fromCharCode(idBytes[i]);
        const idCheck = validateGameId(id);
        if (!idCheck.ok) return { ok: false, error: idCheck.message };
    }
    // No boot.bin: tolerated, matching content_import.cpp's comment -- the
    // fst.bin + files/ shape already gives reasonable confidence, and every
    // real file read downstream still honestly reports its own errors.

    const filesPrefix = discRootPrefix ? `${discRootPrefix}/files/` : "files/";
    const wantedFiles = [];
    for (const f of list) {
        const p = relPath(f);
        if (!p.startsWith(filesPrefix)) continue;
        const inFilesRel = p.slice(filesPrefix.length);
        if (wantedFile(inFilesRel)) {
            wantedFiles.push({ path: inFilesRel, size: f.size, file: f });
        }
    }

    if (wantedFiles.length === 0) {
        return { ok: false, error: `No Mario Party 6 files found under ${filesPrefix}.` };
    }

    const fstBytes = new Uint8Array(await fstFile.arrayBuffer());
    const bootBytes = bootFile ? new Uint8Array(await bootFile.arrayBuffer()) : null;
    const totalWantedBytes =
        wantedFiles.reduce((sum, f) => sum + f.size, 0) + fstBytes.length + (bootBytes ? bootBytes.length : 0);

    return {
        ok: true,
        source: {
            kind: "folder",
            wantedFiles,
            totalWantedBytes,
            bootBytes,
            fstBytes,
            /** Async-iterates one wanted file's bytes in bounded chunks. */
            readWantedFile(entry, chunkSize = DEFAULT_CHUNK_SIZE) {
                return iterateChunks(entry.file, 0, entry.size, chunkSize);
            },
        },
    };
}
