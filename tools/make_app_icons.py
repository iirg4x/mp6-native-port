#!/usr/bin/env python3
"""Build every app icon from packaging/icon-src/mp6-app-icon.png (a full-bleed
square painting): the PWA icons, the Android launcher mipmaps (legacy, round
and adaptive layers) and the Windows .ico. Needs Pillow; rerun only when the
source art changes."""
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "packaging", "icon-src", "mp6-app-icon.png")
PWA = os.path.join(ROOT, "packaging", "pwa", "icons")
ANDROID_RES = os.path.join(ROOT, "packaging", "android", "app", "src", "main", "res")
WINDOWS = os.path.join(ROOT, "packaging", "windows")

# Android density buckets: legacy icon is 48dp, adaptive layers are 108dp.
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}


def fit(img, size):
    return img.resize((size, size), Image.LANCZOS)


def padded(art, size, art_fraction):
    """The art centered at art_fraction of the tile, framed by a blurred,
    enlarged copy of itself so masks and parallax never show a hard edge."""
    back = fit(art, size).filter(ImageFilter.GaussianBlur(size / 24))
    inner = round(size * art_fraction)
    off = (size - inner) // 2
    back.paste(fit(art, inner), (off, off))
    return back


def round_crop(img):
    size = img.width
    s = 4
    mask = Image.new("L", (size * s, size * s), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size * s - 1, size * s - 1), fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask.resize((size, size), Image.LANCZOS))
    return out


def pwa(art):
    os.makedirs(PWA, exist_ok=True)
    for size in (180, 192, 512):  # iOS and browsers draw these full-bleed
        fit(art, size).save(os.path.join(PWA, f"icon-{size}.png"), optimize=True)
    # Maskable icons may be cut to the inner 80% circle; keep the sun, moon
    # and star inside it.
    padded(art, 512, 0.84).save(os.path.join(PWA, "icon-maskable-512.png"), optimize=True)


def android(art):
    for bucket, scale in DENSITIES.items():
        d = os.path.join(ANDROID_RES, f"mipmap-{bucket}")
        os.makedirs(d, exist_ok=True)
        legacy = round(48 * scale)
        fit(art, legacy).save(os.path.join(d, "ic_launcher.png"), optimize=True)
        round_crop(fit(art, legacy)).save(os.path.join(d, "ic_launcher_round.png"), optimize=True)
        # Adaptive: the launcher shows the inner 72dp of the 108dp layer, so
        # the art spans 74dp (just past the mask, never showing an edge)
        # over a blurred copy that only appears while the layers shift.
        layer = round(108 * scale)
        padded(art, layer, 74 / 108).save(os.path.join(d, "ic_launcher_foreground.png"), optimize=True)
    anydpi = os.path.join(ANDROID_RES, "mipmap-anydpi-v26")
    os.makedirs(anydpi, exist_ok=True)
    xml = ('<?xml version="1.0" encoding="utf-8"?>\n'
           '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
           '    <background android:drawable="@color/ic_launcher_background" />\n'
           '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
           '</adaptive-icon>\n')
    for name in ("ic_launcher.xml", "ic_launcher_round.xml"):
        with open(os.path.join(anydpi, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(xml)
    values = os.path.join(ANDROID_RES, "values")
    os.makedirs(values, exist_ok=True)
    with open(os.path.join(values, "ic_launcher_background.xml"), "w", encoding="utf-8", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                '    <color name="ic_launcher_background">#1B2A8C</color>\n</resources>\n')


def windows(art):
    os.makedirs(WINDOWS, exist_ok=True)
    sizes = [(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)]
    fit(art, 256).save(os.path.join(WINDOWS, "mp6.ico"), sizes=sizes)


def main():
    art = Image.open(SRC).convert("RGB")
    if art.width != art.height:
        raise SystemExit(f"{SRC} must be square, got {art.size}")
    pwa(art)
    android(art)
    windows(art)
    print("wrote", PWA, ANDROID_RES, WINDOWS)


if __name__ == "__main__":
    main()
