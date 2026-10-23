"""Compare settled native captures after a renderer-only optimization.

Pass a preserved baseline capture directory and the new capture directory.
Motion-off frames cover all chapters, painter ordering, AA, and window scales.
"""
from pathlib import Path
import sys
from PIL import Image, ImageChops, ImageDraw


def check(before, after):
    names = ['native-picture', 'native-picture-720p', 'native-picture-hidpi',
             'native-picture-4x3', 'native-picture-ultrawide', 'native-picture-page2',
             'native-sound', 'native-sound-selected', 'native-inline-choice',
             'native-game', 'native-extras',
             'launcher-linked', 'launcher-hidpi', 'launcher-4x3', 'launcher-ultrawide']
    for name in names:
        with Image.open(before / f'{name}.png') as a, Image.open(after / f'{name}.png') as b:
            assert a.size == b.size, f'{name}: viewport changed'
            delta = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
            if name.startswith('launcher-'):
                # Overlay's 200ms notice transition uses wall time even when
                # storybook motion is off. Exclude only its known CSS bounds;
                # the paper, frame, logo, hosts and rest of the sky stay exact.
                w, h = a.size
                if name == 'launcher-hidpi' and (w, h) == (2560, 1440):
                    # `toast.controller-warning` (overlay.rcss: width 440dp,
                    # bottom 13vh, scale 1, plus its 1dp shadow spread) has a
                    # 2560x1440 union of [1102,1057,1986,1261). Keep the
                    # exclusion to the changed toast pixels; the book remains
                    # part of the parity assertion.
                    ImageDraw.Draw(delta).rectangle((1102, 1057, 1985, 1260), fill=0)
                else:
                    dpi = 1
                    ImageDraw.Draw(delta).rectangle((int(w/2-225*dpi), int(h*.87-125*dpi),
                                                    int(w/2+300*dpi), int(h*.87+8*dpi)), fill=0)
            error = max(high for low, high in delta.getextrema())
            assert error == 0, f'{name}: rendering changed (max channel error {error}, bounds {delta.getbbox()})'
        suffix = ' (independent toast transition excluded)' if name.startswith('launcher-') else ''
        print(f'[STORYBOOK-PARITY] {name}: pixel-identical{suffix}')


if __name__ == '__main__':
    check(Path(sys.argv[1]), Path(sys.argv[2]))
