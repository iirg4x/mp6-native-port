// Unit tests for js/format-detect.js's magic-byte sniffing.

import { test, assertEqual } from "./tiny-test.js";
import { detectDiscFormat } from "../js/format-detect.js";

function head(bytes) {
    const buf = new Uint8Array(32);
    buf.set(bytes, 0);
    return buf;
}

test("detectDiscFormat: RVZ magic", () => {
    assertEqual(detectDiscFormat(head([0x52, 0x56, 0x5a, 0x01])).format, "rvz");
});
test("detectDiscFormat: WIA magic", () => {
    assertEqual(detectDiscFormat(head([0x57, 0x49, 0x41, 0x01])).format, "wia");
});
test("detectDiscFormat: GCZ magic", () => {
    assertEqual(detectDiscFormat(head([0xb1, 0x0b, 0xc0, 0x01])).format, "gcz");
});
test("detectDiscFormat: CISO magic", () => {
    assertEqual(detectDiscFormat(head(new TextEncoder().encode("CISO"))).format, "ciso");
});

test("detectDiscFormat: plain GC image (magic word at 0x1C)", () => {
    const bytes = head([]);
    new DataView(bytes.buffer).setUint32(0x1c, 0xc2339f3d, false);
    assertEqual(detectDiscFormat(bytes).kind, "gc");
});

test("detectDiscFormat: Wii disc (magic word at 0x18) is reported distinctly", () => {
    const bytes = head([]);
    new DataView(bytes.buffer).setUint32(0x18, 0x5d1c9ea3, false);
    assertEqual(detectDiscFormat(bytes).kind, "wii");
});

test("detectDiscFormat: unrecognized bytes are 'unknown', not misidentified", () => {
    const bytes = head(new TextEncoder().encode("NOPE"));
    assertEqual(detectDiscFormat(bytes).kind, "unknown");
});
