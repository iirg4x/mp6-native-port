"""Compare same-save-state AO captures, reporting RGB and HUD differences."""
import argparse
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def frame(path):
    raw = path.read_bytes()
    assert raw[:8] == b'MP6FDUMP'
    width, height, bpp, fmt = struct.unpack_from('<4I', raw, 12)
    assert bpp == 4
    pixels = np.frombuffer(raw, np.uint8, offset=96).reshape(height, width, 4).copy()
    if fmt in (3, 4):
        pixels = pixels[:, :, [2, 1, 0, 3]]
    return struct.unpack_from('<Q', raw, 40)[0], pixels[:, :, :3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('off')
    parser.add_argument('on')
    args = parser.parse_args()
    off = ROOT / 'build/board-qa-runs' / args.off
    on = ROOT / 'build/board-qa-runs' / args.on
    before = sorted((off / 'frames').glob('*.mfd'))
    after = sorted((on / 'frames').glob('*.mfd'))
    assert before and len(before) == len(after)
    results = []
    for a, b in zip(before, after):
        ta, pa = frame(a)
        tb, pb = frame(b)
        assert ta == tb and pa.shape == pb.shape, (ta, tb)
        delta = pb.astype(np.int16) - pa.astype(np.int16)
        height, width = pa.shape[:2]
        # Only the opaque interiors of the four status panels. Background
        # scenery within each corner is deliberately excluded from this mask.
        hud = np.zeros((height, width), dtype=bool)
        for x0, x1 in ((.06, .26), (.67, .87)):
            for y0, y1 in ((.12, .19), (.80, .87)):
                hud[int(y0*height):int(y1*height), int(x0*width):int(x1*width)] = True
        # Status panel backgrounds are translucent: correctly shaded scenery
        # must show through them. Test opaque white glyphs/black outlines only.
        hud &= (pa.min(axis=2) >= 240) | (pa.max(axis=2) <= 10)
        results.append(dict(tick=ta, changed_pixels=int(np.any(delta != 0, axis=2).sum()),
                            darker_pixels=int(np.any(delta < -1, axis=2).sum()),
                            brighter_pixels=int(np.any(delta > 1, axis=2).sum()),
                            mean_absolute_rgb=float(np.abs(delta).mean()),
                            max_darkening=int(-delta.min()), hud_pixels=int(hud.sum()),
                            hud_max_delta=int(np.abs(delta[hud]).max()) if hud.any() else None))
    (on / 'comparison.json').write_text(json.dumps(results, indent=2)+'\n')
    _, pa = frame(before[0])
    _, pb = frame(after[0])
    Image.fromarray(np.concatenate((pa, pb), axis=1)).save(on / 'ao-before-after.png')
    # Difference visualization is measurement output, not a game asset.
    diff = np.clip((pa.astype(np.int16) - pb.astype(np.int16))*6, 0, 255).astype(np.uint8)
    Image.fromarray(diff).save(on / 'ao-difference-x6.png')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
