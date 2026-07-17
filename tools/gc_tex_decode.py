#!/usr/bin/env python3
"""gc_tex_decode.py -- decode raw GameCube-tiled texture dumps to PNG.

Companion to framescope's MP6_FRAMESCOPE_TEXDUMP: it writes
build/fs_tex_<ptr>_<w>x<h>_f<fmt>.bin (raw, still GC-tiled bytes); this tool
turns them into viewable PNGs so "is the texture content actually right?"
is a picture you look at, not a hypothesis. No third-party deps (own PNG
writer via zlib).

Usage:
  python tools/gc_tex_decode.py build/fs_tex_*.bin        # each -> .png next to it
  python tools/gc_tex_decode.py file.bin --w 640 --h 320 --fmt 6   # explicit

Formats: 0=I4 1=I8 2=IA4 3=IA8 4=RGB565 5=RGB5A3 6=RGBA8 8=C4 9=C8 14=CMPR.
C4/C8 decode as grayscale indices by default (no TLUT in the dump); pass
--tlut build/fs_tlut_*.bin (framescope's TLUT dump: raw BE u16 entries,
tlut format parsed from the _tfN suffix -- 0=IA8 1=RGB565 2=RGB5A3, or
override with --tlut-fmt) to decode them through the real palette instead,
exactly as the GX hardware would. CMPR is DXT1-style.
"""
import argparse, glob, os, re, struct, sys, zlib

def png_write(path, w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))

def expand5(v): return (v << 3) | (v >> 2)
def expand6(v): return (v << 2) | (v >> 4)
def expand4(v): return v * 17
def expand3(v): return (v << 5) | (v << 2) | (v >> 1)

def tlut_load(path, fmt_override=None):
    """Load a framescope fs_tlut_*.bin dump -> list of (r,g,b,a) tuples.
    Entry format (GXTlutFmt): 0=IA8, 1=RGB565, 2=RGB5A3 -- parsed from the
    filename's _tfN suffix unless overridden."""
    fmt = fmt_override
    if fmt is None:
        m = re.search(r"_tf(\d+)\.bin$", path)
        fmt = int(m.group(1)) if m else 2
    raw = open(path, "rb").read()
    pal = []
    for i in range(0, len(raw) - 1, 2):
        v = struct.unpack(">H", raw[i:i+2])[0]
        if fmt == 0:    # IA8: I in low byte, A in high byte (GX TLUT entry = A<<8|I)
            it, al = v & 0xFF, v >> 8
            pal.append((it, it, it, al))
        elif fmt == 1:  # RGB565
            pal.append((expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255))
        else:           # RGB5A3
            if v & 0x8000:
                pal.append((expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), 255))
            else:
                pal.append((expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15),
                            expand3((v >> 12) & 7)))
    return pal

def decode(data, w, h, fmt, tlut=None):
    """returns rgba bytes (w*h*4), decoding the GC tile layout"""
    out = bytearray(w * h * 4)
    def put(x, y, r, g, b, a):
        if x < w and y < h:
            i = (y * w + x) * 4
            out[i:i+4] = bytes((r, g, b, a))
    def putpal(x, y, idx):
        if tlut and idx < len(tlut):
            put(x, y, *tlut[idx])
        else:  # gray-index fallback (or out-of-range index -> magenta flag)
            if tlut:
                put(x, y, 255, 0, 255, 255)
            else:
                v = idx if fmt == 9 else expand4(idx)
                put(x, y, v, v, v, 255)
    if fmt == 6:      # RGBA8: 4x4 tiles, 64B = 32B AR + 32B GB
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                ar = data[p:p+32]; gb = data[p+32:p+64]; p += 64
                for i in range(16):
                    x, y = tx + (i & 3), ty + (i >> 2)
                    put(x, y, ar[i*2+1], gb[i*2], gb[i*2+1], ar[i*2])
    elif fmt in (4, 5):  # RGB565 / RGB5A3: 4x4 tiles, 32B of BE u16
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for i in range(16):
                    v = struct.unpack(">H", data[p:p+2])[0]; p += 2
                    x, y = tx + (i & 3), ty + (i >> 2)
                    if fmt == 4:
                        put(x, y, expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255)
                    elif v & 0x8000:
                        put(x, y, expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), 255)
                    else:
                        put(x, y, expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15),
                            expand3((v >> 12) & 7))
    elif fmt == 9:    # C8: 8x4 tiles, 32B -- palette indices (see putpal)
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    putpal(tx + (i & 7), ty + (i >> 3), v)
    elif fmt == 1:    # I8: 8x4 tiles, 32B
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    put(tx + (i & 7), ty + (i >> 3), v, v, v, 255)
    elif fmt == 8:    # C4: 8x8 tiles, 32B, 2 texels/byte -- palette indices
        p = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    x, y = tx + ((i & 3) * 2), ty + (i >> 2)
                    putpal(x,   y, v >> 4)
                    putpal(x+1, y, v & 15)
    elif fmt == 0:    # I4: 8x8 tiles, 32B, 2 texels/byte
        p = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    x, y = tx + ((i & 3) * 2), ty + (i >> 2)
                    put(x,   y, expand4(v >> 4), expand4(v >> 4), expand4(v >> 4), 255)
                    put(x+1, y, expand4(v & 15), expand4(v & 15), expand4(v & 15), 255)
    elif fmt == 2:    # IA4: 8x4 tiles, 32B, byte = A:I nibbles
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    it, al = expand4(v & 15), expand4(v >> 4)
                    put(tx + (i & 7), ty + (i >> 3), it, it, it, al)
    elif fmt == 3:    # IA8: 4x4 tiles, BE u16 = A<<8 | I
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for i in range(16):
                    a, it = data[p], data[p+1]; p += 2
                    put(tx + (i & 3), ty + (i >> 2), it, it, it, a)
    elif fmt == 14:   # CMPR: 8x8 tiles of four 4x4 DXT1 blocks (2x2 order)
        p = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sub in range(4):
                    sx, sy = tx + (sub & 1) * 4, ty + (sub >> 1) * 4
                    c0, c1, bits = struct.unpack(">HHI", data[p:p+8]); p += 8
                    pal = []
                    r0, g0, b0 = expand5(c0 >> 11), expand6((c0 >> 5) & 63), expand5(c0 & 31)
                    r1, g1, b1 = expand5(c1 >> 11), expand6((c1 >> 5) & 63), expand5(c1 & 31)
                    pal.append((r0, g0, b0, 255)); pal.append((r1, g1, b1, 255))
                    if c0 > c1:
                        pal.append(((2*r0+r1)//3, (2*g0+g1)//3, (2*b0+b1)//3, 255))
                        pal.append(((r0+2*r1)//3, (g0+2*g1)//3, (b0+2*b1)//3, 255))
                    else:
                        pal.append(((r0+r1)//2, (g0+g1)//2, (b0+b1)//2, 255))
                        pal.append((0, 0, 0, 0))
                    for i in range(16):
                        sel = (bits >> (30 - i * 2)) & 3
                        put(sx + (i & 3), sy + (i >> 2), *pal[sel])
    else:
        raise SystemExit(f"unsupported fmt {fmt}")
    return bytes(out)

def stats(rgba, w, h):
    """tiny content summary so you can triage without opening an image viewer"""
    n = w * h
    alphas = rgba[3::4]
    opaque = sum(1 for a in alphas if a == 255)
    clear = sum(1 for a in alphas if a == 0)
    rs, gs, bs = rgba[0::4], rgba[1::4], rgba[2::4]
    avg = (sum(rs)//n, sum(gs)//n, sum(bs)//n)
    distinct = len(set(zip(rs[::37], gs[::37], bs[::37])))  # sampled
    return f"avg_rgb={avg} opaque={opaque*100//n}% transparent={clear*100//n}% distinct~{distinct}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--w", type=int); ap.add_argument("--h", type=int)
    ap.add_argument("--fmt", type=int)
    ap.add_argument("--tlut", help="fs_tlut_*.bin palette dump to decode C4/C8 through")
    ap.add_argument("--tlut-fmt", type=int, choices=(0, 1, 2),
                    help="TLUT entry format override: 0=IA8 1=RGB565 2=RGB5A3 (default: from _tfN filename suffix)")
    a = ap.parse_args()
    tlut = tlut_load(a.tlut, a.tlut_fmt) if a.tlut else None
    paths = []
    for f in a.files: paths.extend(glob.glob(f) or [f])
    for path in paths:
        m = re.search(r"_(\d+)x(\d+)_f(\d+)\.bin$", path)
        w = a.w or (m and int(m.group(1)))
        h = a.h or (m and int(m.group(2)))
        fmt = a.fmt if a.fmt is not None else (m and int(m.group(3)))
        if not (w and h and fmt is not None):
            print(f"{path}: can't infer w/h/fmt (use --w --h --fmt)"); continue
        data = open(path, "rb").read()
        rgba = decode(data, w, h, fmt, tlut)
        suffix = "_tlut" if (tlut and fmt in (8, 9)) else ""
        out = os.path.splitext(path)[0] + suffix + ".png"
        png_write(out, w, h, rgba)
        print(f"{out}  {w}x{h} fmt={fmt}{' +tlut' if (tlut and fmt in (8,9)) else ''}  {stats(rgba, w, h)}")

if __name__ == "__main__":
    main()
