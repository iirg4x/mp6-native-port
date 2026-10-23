"""Check the actual gold paper ornament remains visible beside the logo mount."""
from pathlib import Path
from PIL import Image


def check(captures):
    for name in ['launcher-linked', 'launcher-4x3', 'launcher-ultrawide',
                 'launcher-hidpi', 'launcher-landscape']:
        picture = Image.open(captures / f'{name}.png').convert('RGB')
        scale = min(picture.width / 1672, picture.height / 941)
        ox, oy = (picture.width - 1672 * scale) / 2, (picture.height - 941 * scale) / 2
        # These bands cross the logo mount, not the corner pockets. Sample a
        # short vertical band because the fine ornament is subpixel at 900x420;
        # an individual raster row can miss it even without any occlusion.
        for y in [280, 305, 330, 355, 380]:
            colors = [picture.getpixel((x, py))
                      for py in range(round(oy + y * scale) - 3, round(oy + y * scale) + 4)
                      for x in range(round(ox + 1510 * scale), round(ox + 1540 * scale) + 1)]
            assert any(190 < r < 253 and r-g > 5 and g-b > 15 for r, g, b in colors), \
                f'{name}: logo mount obscures page ornament near y={y}'
        print(f'{name}: uninterrupted page ornament beside logo mount')


if __name__ == '__main__':
    check(Path(__file__).resolve().parents[2] / 'build/settings-qa/captures')
