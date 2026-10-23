"""Exercise the real JPEG adapter, including complete original-disc playback."""
import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body

ROOT = Path(__file__).resolve().parents[1]


class ThpVideo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        output = ROOT / "build/thp-video-tests"
        output.mkdir(exist_ok=True)
        library = output / "decoder.dll"
        result = subprocess.run(
            [build.ZIG, "cc", "-O2", "-shared", "-Iinclude",
             "src/video/thp_jpeg.c", "-o", str(library)],
            cwd=ROOT, capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.library = ctypes.CDLL(str(library))
        cls.decode = cls.library.mp6_thp_decode_video
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                              ctypes.c_void_p, ctypes.c_size_t,
                              ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                              ctypes.c_uint, ctypes.c_uint]
        cls.decode.restype = ctypes.c_int

    def test_bad_payloads_leave_output_untouched(self):
        scratch = ctypes.create_string_buffer(4096)
        y = ctypes.create_string_buffer(b"\xA5" * 256)
        u = ctypes.create_string_buffer(b"\xA5" * 64)
        v = ctypes.create_string_buffer(b"\xA5" * 64)
        for payload in (b"", b"not JPEG", b"\xff\xd8\xff\xda\x00\xff", b"\xff\xd8\xff\xd9"):
            self.assertEqual(self.decode(payload, len(payload), scratch, len(scratch),
                                         y, u, v, 16, 16), 0)
            self.assertEqual(y.raw[:256], b"\xA5" * 256)
            self.assertEqual(u.raw[:64], b"\xA5" * 64)
            self.assertEqual(v.raw[:64], b"\xA5" * 64)

    def test_every_original_preview_frame_decodes_and_changes(self):
        movie = ROOT / "build/disc-cache/orig/GP6E01/files/movie/m601.thp"
        if not movie.is_file():
            self.skipTest("original disc preview is not extracted")
        data = movie.read_bytes()
        be32 = lambda at: struct.unpack_from(">I", data, at)[0]
        self.assertEqual(data[:4], b"THP\0")
        count, size, offset = be32(20), be32(24), be32(40)
        component_info = be32(32)
        components = be32(component_info)
        width, height = struct.unpack_from(">II", data, component_info + 20)
        scratch = ctypes.create_string_buffer(be32(8) * 2)
        y = ctypes.create_string_buffer(width * height)
        u = ctypes.create_string_buffer(width * height // 4)
        v = ctypes.create_string_buffer(width * height // 4)
        hashes = set()
        for frame_number in range(count):
            length = be32(offset + 8)
            start = offset + 8 + components * 4
            payload = data[start:start + length]
            self.assertEqual(self.decode(payload, length, scratch, len(scratch),
                                         y, u, v, width, height), 1, frame_number)
            hashes.add(hashlib.sha256(y.raw).digest())
            next_size = be32(offset)
            offset += size
            size = next_size
        self.assertEqual(len(hashes), count)

    def test_rgba8_copy_packs_tiles_and_respects_frame_cache(self):
        source = (ROOT / "src/video/thp_player.c").read_text(encoding="utf-8")
        copy_frame = function_body(source, "mp6_thp_copy_frame_rgba8")
        probe = r'''#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    unsigned width;
    unsigned height;
} VIDEO_INFO;

typedef struct {
    int frameIndex;
} VIDEO_BUFFER;

typedef struct {
    VIDEO_INFO videoInfo;
    VIDEO_BUFFER videoBuffers[2];
} SIMPLE_CONTROL;

SIMPLE_CONTROL SimpleControl;
static uint8_t *rgbFrame;
static uint32_t generation;
static uint32_t copiedGeneration;
static void *copiedDestination;
static unsigned copiedWidth;
static unsigned copiedHeight;
static unsigned flushCalls;
static unsigned invalidateCalls;
static void *lastFlushDestination;
static unsigned lastFlushBytes;

void DCFlushRange(void *destination, unsigned bytes)
{
    ++flushCalls;
    lastFlushDestination = destination;
    lastFlushBytes = bytes;
}

void GXInvalidateTexAll(void)
{
    ++invalidateCalls;
}

''' + copy_frame + r'''

static void fill_rgb(uint8_t *pixels, unsigned base)
{
    unsigned y;
    unsigned x;

    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            uint8_t *pixel = pixels + ((size_t)y * 8 + x) * 3;
            pixel[0] = (uint8_t)(base + x * 3 + y);
            pixel[1] = (uint8_t)(base + 40 + x + y * 2);
            pixel[2] = (uint8_t)(base + 80 + x * 2 + y * 3);
        }
    }
}

static void expected_pixel(unsigned base, unsigned x, unsigned y,
                           unsigned *red, unsigned *green, unsigned *blue)
{
    unsigned sx;
    unsigned sy;
    unsigned r = 0;
    unsigned g = 0;
    unsigned b = 0;

    /* A 4x4 destination samples each 2x2 block of the 8x8 source. */
    for (sy = y * 2; sy < y * 2 + 2; ++sy) {
        for (sx = x * 2; sx < x * 2 + 2; ++sx) {
            r += base + sx * 3 + sy;
            g += base + 40 + sx + sy * 2;
            b += base + 80 + sx * 2 + sy * 3;
        }
    }
    *red = (r + 2) / 4;
    *green = (g + 2) / 4;
    *blue = (b + 2) / 4;
}

static void assert_4x4_tile(const uint8_t *destination, unsigned base)
{
    unsigned y;
    unsigned x;

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            unsigned red;
            unsigned green;
            unsigned blue;
            size_t at = ((size_t)(y / 4) * (4 / 4) + x / 4) * 64 +
                        ((y % 4) * 4 + x % 4) * 2;
            expected_pixel(base, x, y, &red, &green, &blue);
            assert(destination[at] == 255);
            assert(destination[at + 1] == red);
            assert(destination[at + 32] == green);
            assert(destination[at + 33] == blue);
        }
    }
}

static void assert_8x4_tiles(const uint8_t *destination, unsigned base)
{
    unsigned y;
    unsigned x;

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 8; ++x) {
            unsigned sy;
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            size_t at = ((size_t)(y / 4) * (8 / 4) + x / 4) * 64 +
                        ((y % 4) * 4 + x % 4) * 2;
            for (sy = y * 2; sy < y * 2 + 2; ++sy) {
                red += base + x * 3 + sy;
                green += base + 40 + x + sy * 2;
                blue += base + 80 + x * 2 + sy * 3;
            }
            assert(destination[at] == 255);
            assert(destination[at + 1] == (red + 1) / 2);
            assert(destination[at + 32] == (green + 1) / 2);
            assert(destination[at + 33] == (blue + 1) / 2);
        }
    }
}

int main(void)
{
    uint8_t source[8 * 8 * 3];
    uint8_t destination[4 * 4 * 4];
    uint8_t otherDestination[4 * 4 * 4];
    uint8_t resizedDestination[8 * 4 * 4];
    uint8_t before[sizeof(destination)];
    unsigned beforeFlush;
    unsigned beforeInvalidate;
    unsigned invalidWidth;

    memset(&SimpleControl, 0, sizeof(SimpleControl));
    SimpleControl.videoInfo.width = 8;
    SimpleControl.videoInfo.height = 8;
    SimpleControl.videoBuffers[0].frameIndex = 4;
    fill_rgb(source, 10);
    rgbFrame = source;
    generation = 7;
    copiedGeneration = 0;
    copiedDestination = NULL;
    copiedWidth = copiedHeight = 0;
    memset(destination, 0xCD, sizeof(destination));
    assert(mp6_thp_copy_frame_rgba8(destination, 4, 4) == 1);
    assert_4x4_tile(destination, 10);
    assert(flushCalls == 1 && invalidateCalls == 1);
    assert(lastFlushDestination == destination && lastFlushBytes == sizeof(destination));

    /* Same frame/target/size is a true cache hit: even a caller mutation is
     * preserved and no cache maintenance runs again. */
    destination[0] = 0x5A;
    beforeFlush = flushCalls;
    beforeInvalidate = invalidateCalls;
    assert(mp6_thp_copy_frame_rgba8(destination, 4, 4) == 1);
    assert(destination[0] == 0x5A);
    assert(flushCalls == beforeFlush && invalidateCalls == beforeInvalidate);

    /* A decoded generation invalidates the cache and refreshes the same
     * destination from the new RGB frame. */
    fill_rgb(source, 90);
    ++generation;
    assert(mp6_thp_copy_frame_rgba8(destination, 4, 4) == 1);
    assert_4x4_tile(destination, 90);
    assert(flushCalls == 2 && invalidateCalls == 2);

    /* A distinct destination is copied even at the same generation. */
    memset(otherDestination, 0xCD, sizeof(otherDestination));
    assert(mp6_thp_copy_frame_rgba8(otherDestination, 4, 4) == 1);
    assert_4x4_tile(otherDestination, 90);
    assert(flushCalls == 3 && invalidateCalls == 3);

    /* Changing the requested size also bypasses the cache and exercises two
     * horizontal RGBA8 tiles with vertical 2:1 downsampling. */
    memset(resizedDestination, 0xCD, sizeof(resizedDestination));
    assert(mp6_thp_copy_frame_rgba8(resizedDestination, 8, 4) == 1);
    assert_8x4_tiles(resizedDestination, 90);
    assert(flushCalls == 4 && invalidateCalls == 4);

    /* No decoded frame leaves the last displayed target byte-for-byte intact. */
    memset(destination, 0xA7, sizeof(destination));
    memcpy(before, destination, sizeof(before));
    SimpleControl.videoBuffers[0].frameIndex = -1;
    assert(mp6_thp_copy_frame_rgba8(destination, 4, 4) == 1);
    assert(memcmp(destination, before, sizeof(destination)) == 0);
    assert(flushCalls == 4 && invalidateCalls == 4);

    /* Invalid dimensions and null destinations fail before touching output or
     * issuing cache maintenance. */
    SimpleControl.videoBuffers[0].frameIndex = 4;
    for (invalidWidth = 0; invalidWidth < 5; ++invalidWidth) {
        unsigned width = invalidWidth == 0 ? 0 : invalidWidth == 1 ? 2 :
                         invalidWidth == 2 ? 1028 : 4;
        unsigned height = invalidWidth == 3 ? 2 : invalidWidth == 4 ? 1028 : 4;
        unsigned beforeByte;

        memset(destination, 0xB6, sizeof(destination));
        beforeByte = destination[0];
        beforeFlush = flushCalls;
        beforeInvalidate = invalidateCalls;
        if (invalidWidth == 0) {
            assert(mp6_thp_copy_frame_rgba8(NULL, 4, 4) == 0);
        } else {
            assert(mp6_thp_copy_frame_rgba8(destination, width, height) == 0);
        }
        assert(destination[0] == beforeByte);
        assert(flushCalls == beforeFlush && invalidateCalls == beforeInvalidate);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source_path = temporary / "thp_copy_frame_probe.c"
            executable = temporary / "thp_copy_frame_probe.exe"
            source_path.write_text(probe, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "cc", "-O2", "-std=c11", str(source_path),
                 "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(
                result.returncode, 0,
                f"THP RGBA8 probe compile failed:\n{result.stderr}",
            )
            result = subprocess.run(
                [str(executable)], cwd=ROOT,
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(
                result.returncode, 0,
                f"THP RGBA8 probe failed:\n{result.stdout}\n{result.stderr}",
            )

    def test_unavailable_movie_leaves_its_sprite_to_the_owner(self):
        # Callers put the movie sprite in a group. Killing it when the file
        # is missing freed a slot the group still drew (the instruction
        # screen's red box); only a requested close may release it.
        path = "src/game/thpmain.c"
        original = (Path(build.DECOMP) / path).read_text(encoding="utf-8")
        current = apply_patches.apply_unified_diff(
            original, (ROOT / "compat/decomp" / (path + ".patch")).read_text())
        body = function_body(current, "THPTestProc")
        self.assertIn("goto cleanup;", body)
        self.assertLess(body.index("HuSprKill(SimpleControl.drawObjectId)"),
                        body.index("cleanup:"))
        self.assertLess(body.index("THP_STATE_CLOSE"), body.index("HuSprKill"))


if __name__ == "__main__":
    unittest.main()
