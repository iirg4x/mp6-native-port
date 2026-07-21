// Unit tests for js/crc32.js against well-known CRC-32 (zip/zlib polynomial) test vectors.

import { test, assertEqual } from "./tiny-test.js";
import { crc32, crc32Update } from "../js/crc32.js";

test("crc32: empty input is 0", () => {
    assertEqual(crc32(new Uint8Array(0)), 0);
});

test('crc32: "123456789" is the standard check value 0xCBF43926', () => {
    const bytes = new TextEncoder().encode("123456789");
    assertEqual(crc32(bytes) >>> 0, 0xcbf43926);
});

test('crc32: "The quick brown fox jumps over the lazy dog" is 0x414FA339', () => {
    const bytes = new TextEncoder().encode("The quick brown fox jumps over the lazy dog");
    assertEqual(crc32(bytes) >>> 0, 0x414fa339);
});

test("crc32Update: incremental feeding matches one-shot over the same bytes", () => {
    const bytes = new TextEncoder().encode("The quick brown fox jumps over the lazy dog");
    let crc = 0;
    for (let i = 0; i < bytes.length; i += 7) {
        crc = crc32Update(crc, bytes.subarray(i, i + 7));
    }
    assertEqual(crc >>> 0, crc32(bytes) >>> 0);
});
