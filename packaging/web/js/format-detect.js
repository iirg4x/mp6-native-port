// MP6 web packager -- disc image format sniffing.
//
// mp6scene's README (tools/mp6scene/README.md, "Which image formats work")
// documents exactly which container formats this whole toolchain can and
// cannot read directly, for the same reason this browser tool can't: only a
// plain, uncompressed .iso/.gcm is a flat byte-addressable disc image.
// Everything else is a compressed/scrubbed archive *of* one and needs a
// real codec (or NKit) to restore first. This module recognizes the common
// ones by magic bytes so the packager can name the problem instead of
// failing with a generic parse error, and tell the user to convert in
// Dolphin (Right-click game -> Properties -> "Verify Integrity" isn't it --
// it's the disc list's right-click -> Convert File... -> ISO/GCM, no
// compression).
//
// Magic offsets/values (all well-documented, public GameCube/Wii disc and
// Dolphin-container facts -- e.g. Dolphin's own DiscIO/*Blob.cpp headers):
//   RVZ   bytes 0-3   "RVZ\x01"        (0x52 0x56 0x5A 0x01)
//   WIA   bytes 0-3   "WIA\x01"        (0x57 0x49 0x41 0x01)
//   GCZ   bytes 0-3   0xB10BC001 (BE)
//   CISO  bytes 0-3   "CISO"           (0x43 0x49 0x53 0x4F)
//   plain GC/Wii disc: no format magic at offset 0 (that's the 6-byte game
//     ID instead -- see fst.js) but a fixed magic word distinguishes GC
//     from Wii: GC word 0xC2339F3D at 0x1C, Wii word 0x5D1C9EA3 at 0x18.

const MAGIC_CHECKS = [
    { format: "rvz", name: "RVZ (Dolphin's compressed format)", bytes: [0x52, 0x56, 0x5a, 0x01] },
    { format: "wia", name: "WIA (Wii ISO Archive, compressed)", bytes: [0x57, 0x49, 0x41, 0x01] },
    { format: "gcz", name: "GCZ (older Dolphin compressed format)", bytes: [0xb1, 0x0b, 0xc0, 0x01] },
    { format: "ciso", name: "CISO (compressed ISO)", bytes: [0x43, 0x49, 0x53, 0x4f] },
];

const GC_MAGIC_OFFSET = 0x1c;
const GC_MAGIC_VALUE = 0xc2339f3d;
const WII_MAGIC_OFFSET = 0x18;
const WII_MAGIC_VALUE = 0x5d1c9ea3;

function bytesMatch(bytes, offset, pattern) {
    for (let i = 0; i < pattern.length; i++) {
        if (bytes[offset + i] !== pattern[i]) return false;
    }
    return true;
}

/**
 * Sniffs the first ~32 bytes of a candidate disc image. `head` must contain
 * at least offset 0x1C..0x20 (32 bytes is enough for every check here).
 *
 * Returns one of:
 *   { kind: "compressed", format, name }   -- a known archive-of-a-disc format
 *   { kind: "wii" }                        -- a real Wii disc, not GameCube
 *   { kind: "gc" }                         -- looks like a plain GC/GCM image
 *   { kind: "unknown" }                    -- none of the above; probably not a disc image at all
 */
export function detectDiscFormat(head) {
    for (const check of MAGIC_CHECKS) {
        if (bytesMatch(head, 0, check.bytes)) {
            return { kind: "compressed", format: check.format, name: check.name };
        }
    }
    if (head.length >= GC_MAGIC_OFFSET + 4) {
        const view = new DataView(head.buffer, head.byteOffset, head.byteLength);
        if (view.getUint32(GC_MAGIC_OFFSET, false) === GC_MAGIC_VALUE) {
            return { kind: "gc" };
        }
        if (head.length >= WII_MAGIC_OFFSET + 4 && view.getUint32(WII_MAGIC_OFFSET, false) === WII_MAGIC_VALUE) {
            return { kind: "wii" };
        }
    }
    return { kind: "unknown" };
}

/** Human-readable guidance for a detectDiscFormat() result that isn't a usable GC image. */
export function formatProblemMessage(detected) {
    if (detected.kind === "compressed") {
        return (
            `This looks like a ${detected.name} file, not a plain disc image. ` +
            `Open it in Dolphin, right-click the game in your game list, choose ` +
            `"Convert File...", and pick format ISO/GCM with compression None -- ` +
            `then point the packager at the converted file. ` +
            `(Or use Dolphin's "Extract Entire Disc..." instead and pick the extracted folder here.)`
        );
    }
    if (detected.kind === "wii") {
        return "This is a Wii disc image. Mario Party 6 is a GameCube game -- double-check you picked the right disc.";
    }
    return "This doesn't look like a GameCube disc image at all (no recognized header). Make sure you picked the right file.";
}
