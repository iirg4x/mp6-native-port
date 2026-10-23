"""The decode-ahead decoders match the game's on generated streams.

src/os/decode_ahead.c re-implements the game's LZ, SLIDE, FSLIDE and RLE
decoders (src/game/decode.c in the decomp) with bounds checks. This compiles
both, feeds them the same pseudo-random streams, and requires that whenever
the bounded decoder accepts a stream it produced exactly the game's bytes
from exactly the game's input. (zlib is the library's own inflate in both.)
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(build.DECOMP) / 'src/game/decode.c'
PORT = ROOT / 'src/os/decode_ahead.c'

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#include "game_decoders.inc"
#include "port_decoders.inc"

static uint32_t rng = 12345;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(void)
{
    static u8 in[1 << 16], gameOut[1 << 15], portOut[1 << 15];
    int type, accepted[4] = {0};
    for (type = 0; type < 4; type++) {
        int round;
        for (round = 0; round < 6000; round++) {
            u32 size = 1 + next() % 1500, i;
            const u32 pre = 4096; /* game FSLIDE may read before its output */
            DECODE d;
            Bounded b;
            int ok;
            for (i = 0; i < sizeof(in); i++) in[i] = (next() & 1) ? 0xFF : (u8)next();
            /* FSLIDE rejects early back-references: favour literals. */
            if (type == 2) {
                size = 1 + size % 300;
                for (i = 0; i < sizeof(in); i++) in[i] = (next() % 4) ? 0xFF : (u8)next();
            }
            if (type == 3) for (i = 0; i < sizeof(in); i++) in[i] = (u8)(next() % 3 == 0 ? 0x81 : next());
            for (i = 0; i < sizeof(gameOut); i++) gameOut[i] = (u8)i;
            memset(portOut, 0xEE, sizeof(portOut));
            d.src = in; d.dst = gameOut + pre; d.size = size;
            b.srcStart = b.src = in; b.srcEnd = in + 4096;
            b.dstStart = b.dst = portOut + pre; b.dstEnd = portOut + pre + size;
            /* Streams the bounded decoder rejects can make the game's decoder
             * run past its output, so only accepted ones reach the game's. */
            switch (type) {
            case 0: ok = decode_lz(&b); if (ok) HuDecodeLz(&d); break;
            case 1: ok = decode_slide(&b, 1); if (ok) HuDecodeSlide(&d); break;
            case 2: ok = decode_slide(&b, 0); if (ok) HuDecodeFslide(&d); break;
            default: ok = decode_rle(&b); if (ok) HuDecodeRle(&d); break;
            }
            if (!ok) continue;
            accepted[type]++;
            if (d.dst != gameOut + pre + size || d.size != 0) {
                printf("type %d round %d: accepted a stream the game decodes differently\n", type, round);
                return 1;
            }
            if (memcmp(gameOut + pre, portOut + pre, size) != 0) {
                printf("type %d round %d: output differs\n", type, round);
                return 1;
            }
            if (d.src != in + (b.src - b.srcStart)) {
                printf("type %d round %d: consumed %ld, game read %ld\n", type, round,
                       (long)(b.src - b.srcStart), (long)(d.src - in));
                return 1;
            }
        }
    }
    printf("accepted lz=%d slide=%d fslide=%d rle=%d\n", accepted[0], accepted[1], accepted[2], accepted[3]);
    for (type = 0; type < 4; type++) if (accepted[type] < 50) return 2;
    return 0;
}
'''


class DecodeAhead(unittest.TestCase):
    def test_bounded_decoders_match_the_game(self):
        if not GAME.is_file():
            self.skipTest('decomp source not available')
        game = GAME.read_text(encoding='utf-8').replace('\r\n', '\n')
        game = game[game.index('typedef struct Decode_s'):game.index('// Resource loaders call this')]
        game = game.replace('static int HuDecodeZlib(DECODE *decode);', '')
        port = PORT.read_text(encoding='utf-8')
        port = ('static uint32_t be32(const uint8_t *p)\n{\n    return ((uint32_t)p[0] << 24) | '
                '((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];\n}\n'
                + port[port.index('typedef struct {\n    const uint8_t *src, *srcEnd'):
                       port.index('static voidpf zlib_alloc')])
        self.assertIsNotNone(re.search(r'static int decode_slide\(Bounded \*b, int zeroBeforeStart\)', port))
        with tempfile.TemporaryDirectory(prefix='decode-ahead-', dir=ROOT / 'build') as temporary:
            folder = Path(temporary)
            (folder / 'game_decoders.inc').write_text(game, encoding='utf-8')
            (folder / 'port_decoders.inc').write_text(port, encoding='utf-8')
            (folder / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = folder / 'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', '-w', '-I', str(folder),
                                     str(folder / 'test.c'), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
