"""Package image-generated menu art; never draw or regenerate its content."""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('picture', 'sound', 'controls', 'game', 'extras', 'day-night', 'star')
OPTION_NAMES = ('option-preset', 'option-shadow', 'option-edges', 'option-occlusion',
                'option-effects', 'option-camera')
SIZE = 256


def main():
    destination = ROOT / 'res/menu'
    destination.mkdir(parents=True, exist_ok=True)
    total_bytes = 0
    for name in (*NAMES, *OPTION_NAMES):
        revision = 'v1' if name in OPTION_NAMES else 'v3'
        source = ROOT / 'docs/design/settings-icons' / f'{name}-{revision}.png'
        with Image.open(source) as original:
            if original.width != original.height or original.width < SIZE:
                raise ValueError(f'{source}: expected square artwork at least {SIZE}px')
            if 'A' not in original.getbands():
                raise ValueError(f'{source}: generated alpha channel is missing')
            if original.getchannel('A').getextrema() != (0, 255):
                raise ValueError(f'{source}: expected transparent and opaque pixels')
            # Mechanical deployment resize only. Keep the generated colors,
            # composition and transparency; retain the unmodified source PNG.
            icon = original.convert('RGBA').resize((SIZE, SIZE), Image.Resampling.LANCZOS)
            output = destination / f'{name}.png'
            icon.save(output, optimize=True)
            total_bytes += output.stat().st_size
            print(f'{name}: {original.width}px -> {SIZE}px RGBA')
    print(f'{total_bytes / 1024:.1f} KiB PNG; {(len(NAMES) + len(OPTION_NAMES)) * SIZE * SIZE * 4 / 1048576:.2f} MiB RGBA')


if __name__ == '__main__':
    main()
