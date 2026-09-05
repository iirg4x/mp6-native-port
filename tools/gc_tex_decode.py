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

THE INTENSITY FORMATS CARRY ALPHA.  For I4 and I8 the GX texture unit
replicates the intensity into ALL FOUR channels, so A == I -- an intensity
texture is a MASK, not a greyscale image, and its zero texels are fully
transparent.  See the fmt==0 / fmt==1 arms for the derivation and citations.
IA4/IA8 carry an explicit alpha nibble/byte; the CI_* formats take alpha from
their TLUT entry.

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

# Why the I4/I8 arms below write A = I and not A = 255.
#
# THE HARDWARE.  The GX texture unit expands an intensity texel into all four
# channels, so a fetched I4/I8 texel is (I, I, I, I) and its ALPHA equals its
# intensity.  Cross-checked against aurora -- the GX implementation the native
# port actually renders through, calibrated against real captures --
# `lib/gfx/texture_convert.cpp:215-221` (TextureDecoderI4) and `:232-238`
# (TextureDecoderI8) both end with `target[x].a = intensity`.  Its
# `ExpandTo8<4>(n)` is `(n << 4) | n`, i.e. exactly this file's `expand4`.
#
# THAT ALPHA IS LOAD-BEARING IN MP6, not an incidental channel.  In the decomp
# (external_refs/repos/marioparty6/src/game/hsfdraw.c):
#
#   * hsfdraw.c:2039-2055  Hu3DTexSet uploads HSF_BMPFMT_I4/I8 as GX_TF_I4 /
#     GX_TF_I8, so the fetch is the intensity fetch above.
#   * hsfdraw.c:1103       the intensity first TEV stage is
#     `GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST,
#     GX_CA_ZERO)` -> `alpha = TEXA * KONST_A`, and hsfdraw.c:561-563 forces
#     every stage's `GXSetTevKAlphaSel(i, GX_TEV_KASEL_1)`, so KONST_A == 1.0
#     and the pipeline alpha IS the texel's alpha.  (The non-intensity arm at
#     hsfdraw.c:1130 reads GX_CA_TEXA too -- alpha always comes from the
#     texture.)
#   * hsfdraw.c:536        `GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_AND,
#     GX_GEQUAL, 1)` DISCARDS every fragment whose alpha is 0, and
#     hsfdraw.c:533-534 raises that threshold to 128 for
#     HSF_MATERIAL_DISABLE_ZWRITE / _NEAR materials.
#   * hsfdraw.c:503        the surviving fragments blend
#     `GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, ..)`.
#
# So an MP6 intensity texture is a MASK: I == 0 means "do not draw this texel"
# and the game relies on it. Emitting A = 255 turned every such mask into an
# opaque rectangle -- measured on Towering Treetop, that was the tree's eyes,
# ~20 shadow decals, the water sheet and the pond ripple all rendering as solid
# blocks. The KONST in that stage 0 modulates COLOUR ONLY (`SetKColorRGB` is
# handed `color.a = 255`, hsfdraw.c:1098-1101), which is why the consumer-side
# tint fold must stay RGB-only and leave this alpha alone.
_INTENSITY_ALPHA = "A == I for GX_TF_I4 / GX_TF_I8"


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
                # DELIBERATELY A = 255 here, unlike the I4/I8 arms.  C4/C8 are
                # PALETTE formats: the hardware's alpha comes from the TLUT
                # entry (see tlut_load), never from the index, so an index is
                # not an intensity and A = index would be a fiction.  This arm
                # only runs when no TLUT was supplied -- a raw framescope dump
                # being eyeballed -- and there a visible index map is the point.
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
    elif fmt == 1:    # I8: 8x4 tiles, 32B -- A == I, see _INTENSITY_ALPHA
        p = 0
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    put(tx + (i & 7), ty + (i >> 3), v, v, v, v)
    elif fmt == 8:    # C4: 8x8 tiles, 32B, 2 texels/byte -- palette indices
        p = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    x, y = tx + ((i & 3) * 2), ty + (i >> 2)
                    putpal(x,   y, v >> 4)
                    putpal(x+1, y, v & 15)
    elif fmt == 0:    # I4: 8x8 tiles, 32B, 2 texels/byte -- A == I
        p = 0
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for i in range(32):
                    v = data[p]; p += 1
                    x, y = tx + ((i & 3) * 2), ty + (i >> 2)
                    hi, lo = expand4(v >> 4), expand4(v & 15)
                    put(x,   y, hi, hi, hi, hi)
                    put(x+1, y, lo, lo, lo, lo)
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
    """tiny content summary so a text-only agent can triage without eyes"""
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
