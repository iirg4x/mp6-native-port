// MP6 web packager -- game ID validation.
//
// Transcribed from mp6-native's platform/content/content_import.cpp:172-187
// (validate_game_id): GP6E01 is Mario Party 6 (USA), the only disc this
// port (and therefore this packager) supports -- its data layout, byte
// endianness and save-file contracts are all verified against that one
// release. A same-series disc (GP6, any other region/version byte) gets a
// precise "wrong region" message; anything else gets a generic mismatch
// message. Both sanitize non-printable bytes to "?" before display, exactly
// like the C++ version, so a garbage read never corrupts the terminal/DOM.

export const EXPECTED_GAME_ID = "GP6E01";

function sanitizeForDisplay(id) {
    let out = "";
    for (let i = 0; i < id.length; i++) {
        const code = id.charCodeAt(i);
        out += code < 0x20 || code > 0x7e ? "?" : id[i];
    }
    return out;
}

/**
 * `id` is a 6-character string (see fst.js's parseBootBin -> header.gameId,
 * or the folder-source's own 6-byte read of sys/boot.bin). Returns
 * { ok: true } or { ok: false, message }.
 */
export function validateGameId(id) {
    if (id === EXPECTED_GAME_ID) return { ok: true };
    const shown = sanitizeForDisplay(id);
    if (id.slice(0, 3) === "GP6") {
        return {
            ok: false,
            message: `Wrong region: this disc is ${shown} -- the packager needs the USA release (GP6E01).`,
        };
    }
    return {
        ok: false,
        message: `Not Mario Party 6: game ID ${shown} (need GP6E01, Mario Party 6 USA).`,
    };
}
