// MP6 web packager -- opens a plain .iso/.gcm ByteSource (see byte-source.js)
// as a validated Mario Party 6 disc: format sniff -> game ID check -> parse
// boot.bin + fst.bin -> filter to the wanted set (wanted.js). Every large
// read after that goes through iterateChunks(), so opening even a 1.4GB
// image only ever materializes boot.bin (1088 bytes) and fst.bin
// (~24KB on the real disc) up front.
//
// This intentionally duplicates none of mp6-native's C++ (there is no JS
// build of `nod` for the browser) but every field it reads is the same
// field content_import.cpp reads through nod, cross-referenced in fst.js's
// header comment.

import { readRange, iterateChunks, DEFAULT_CHUNK_SIZE } from "./byte-source.js";
import { parseBootBin, parseFst, BOOT_BIN_LENGTH } from "./fst.js";
import { detectDiscFormat, formatProblemMessage } from "./format-detect.js";
import { validateGameId } from "./validate.js";
import { filterWanted } from "./wanted.js";

/**
 * Opens and validates a disc image ByteSource. Returns
 * `{ ok: true, source: DiscSource }` or `{ ok: false, error: string }` --
 * never throws for expected "wrong file" cases, so callers can show the
 * message directly in the UI.
 */
export async function openIsoSource(byteSource) {
    if (byteSource.size < BOOT_BIN_LENGTH) {
        return { ok: false, error: "This file is too small to be a GameCube disc image." };
    }

    const bootBytes = await readRange(byteSource, 0, BOOT_BIN_LENGTH);

    const detected = detectDiscFormat(bootBytes.subarray(0, 32));
    if (detected.kind === "compressed" || detected.kind === "wii") {
        return { ok: false, error: formatProblemMessage(detected) };
    }

    const header = parseBootBin(bootBytes);
    const idCheck = validateGameId(header.gameId);
    if (!idCheck.ok) {
        return { ok: false, error: idCheck.message };
    }

    if (header.fstOffset === 0 || header.fstSize === 0 || header.fstOffset + header.fstSize > byteSource.size) {
        return { ok: false, error: "The disc's FST offset/size in boot.bin look invalid -- this image may be truncated or corrupt." };
    }

    let fstBytes;
    try {
        fstBytes = await readRange(byteSource, header.fstOffset, header.fstSize);
    } catch (e) {
        return { ok: false, error: `Could not read the file system table: ${e.message}` };
    }

    let parsed;
    try {
        parsed = parseFst(fstBytes);
    } catch (e) {
        return { ok: false, error: `Could not parse the disc's file system table: ${e.message}` };
    }

    for (const f of parsed.files) {
        if (f.offset + f.size > byteSource.size) {
            return {
                ok: false,
                error: `The disc's file table references data past the end of the image (at "${f.path}") -- this image is likely truncated.`,
            };
        }
    }

    const wantedFiles = filterWanted(parsed.files)
        .slice()
        .sort((a, b) => a.offset - b.offset);

    const totalWantedBytes =
        wantedFiles.reduce((sum, f) => sum + f.size, 0) + fstBytes.length + bootBytes.length;

    return {
        ok: true,
        source: {
            kind: "iso",
            header,
            numEntries: parsed.numEntries,
            allFiles: parsed.files,
            wantedFiles,
            totalWantedBytes,
            bootBytes,
            fstBytes,
            /** Async-iterates one wanted file's bytes in bounded chunks. */
            readWantedFile(entry, chunkSize = DEFAULT_CHUNK_SIZE) {
                return iterateChunks(byteSource, entry.offset, entry.size, chunkSize);
            },
        },
    };
}
