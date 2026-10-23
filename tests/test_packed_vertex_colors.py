"""Vertex colours packed as a u32 over GXColor bytes must not reach the GPU natively.

Retail stored RGBA bytes big-endian, so a union's u32 view (or *(u32 *)&color)
read as 0xRRGGBBAA. On little-endian hosts the same bytes read as 0xAABBGGRR:
Tally Me Banana's white ripple rings faded their red channel instead of alpha
and stayed opaque cyan, and the other minigame's blue ripples drew with alpha 0.
"""
import os
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKED_READ = re.compile(r'=\s*(\*\s*\(\s*u32\s*\*\s*\)\s*&\s*[\w>.\-\[\]]*colou?r\w*'
                         r'|[\w>.\-\[\]]*colou?r\.(?:packed|value))\s*;')


def patch_lines(rel, sign):
    path = ROOT / 'compat/decomp' / (rel + '.patch')
    if not path.exists():
        return ''
    text = path.read_text(encoding='utf-8')
    return '\n'.join(l[1:] for l in text.splitlines() if l.startswith(sign) and not l.startswith(sign * 3))


def decomp_dir():
    env = os.environ.get('MP6_DECOMP_DIR')
    if env:
        return Path(env)
    pins = sorted((ROOT / 'build/deps').glob('marioparty6-*'), key=lambda p: p.stat().st_mtime)
    return pins[-1] if pins else None


class PackedVertexColors(unittest.TestCase):
    def test_ripple_rings_draw_from_their_channels(self):
        m674 = patch_lines('src/REL/m674Dll/effects.c', '+')
        self.assertIn('GXColor color = sprite->color.channels;', m674)
        self.assertEqual(m674.count('GXColor4u8(color.r, color.g, color.b, color.a);'), 4)
        self.assertIn('slot->color.channels.a = alpha;', m674)
        self.assertNotIn('GXColor1u32(color);', m674)
        m626 = patch_lines('src/REL/m626Dll/effects.c', '+')
        self.assertIn('record->color.channels.blue = 255;', m626)
        self.assertIn('color.a = (u8)record->color.channels.alpha;', m626)
        self.assertEqual(m626.count('GXColor4u8(color.r, color.g, color.b, color.a);'), 3)

    def test_every_packed_colour_read_in_a_vertex_emitter_is_patched_out(self):
        root = decomp_dir()
        if root is None or not (root / 'src').exists():
            self.skipTest('pinned decomp checkout not available')
        missed = []
        for path in sorted((root / 'src').rglob('*.c')):
            rel = path.relative_to(root).as_posix()
            if rel.startswith('src/dolphin/'):
                continue  # The SDK is replaced by Aurora natively.
            text = path.read_text(encoding='utf-8', errors='replace')
            if 'GXColor1u32' not in text and 'GXWGFifo.u32' not in text:
                continue
            removed = patch_lines(rel, '-')
            for line in text.splitlines():
                if PACKED_READ.search(line) and line not in removed.splitlines():
                    missed.append(f'{rel}: {line.strip()}')
        self.assertEqual(missed, [])


if __name__ == '__main__':
    unittest.main()
