"""Render native frame dumps into inspectable PNGs and labelled contact sheets."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw
from tests.integration.compare_ao_captures import frame


def render(directory):
    paths = sorted(directory.glob("*.mfd"))
    if not paths:
        raise ValueError(f"no captures: {directory}")
    thumbs = []
    for path in paths:
        _, pixels = frame(path)
        picture = Image.fromarray(pixels)
        picture.save(path.with_suffix(".png"))
        picture.thumbnail((480, 360))
        thumbs.append((path.stem, picture.copy()))
    for start in range(0, len(thumbs), 8):
        group = thumbs[start:start + 8]
        height = max(picture.height for _, picture in group) + 24
        sheet = Image.new("RGB", (960, height * ((len(group) + 1) // 2)), "#202020")
        draw = ImageDraw.Draw(sheet)
        for index, (label, picture) in enumerate(group):
            x, y = (index % 2) * 480, (index // 2) * height
            sheet.paste(picture, (x, y + 24))
            draw.text((x + 8, y + 5), label, fill="white")
        sheet.save(directory / f"contact-{start // 8:02}.png")
    print(f"Rendered {len(paths)} frames in {directory}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    render(parser.parse_args().directory)
