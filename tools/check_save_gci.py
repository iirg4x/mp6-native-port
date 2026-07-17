#!/usr/bin/env python3
"""check_save_gci.py -- byte-level console-format checker for a Mario Party 6
.gci (docs/TESTING.md's save-integrity gate).

Verifies, straight off the file bytes (no game code involved), that a save is
big-endian console format:
  - icon/banner area checksum (byte-wise ~sum over 0..0x2040, stored as two
    BE bytes at 0x2040),
  - all 6 box tails (3 primary + 3 backup) satisfy the same byte-wise ~sum
    over the 0xCB0 box body,
  - for every SAVE box: decodes magic/name/lastBoard plus the BE u16
    bankStar and BE u64 time (rendered as a calendar date -- must match the
    game's own comment-date string at 0x20, which the game writes byte-wise
    at every save and is therefore an in-file ground truth).

Exit 0 iff every checksum is valid. Usage:
    python tools/check_save_gci.py "saves/USA/Card A/01-GP6E-MARIPA6.gci"
"""
import datetime
import struct
import sys

BOX = 0xCB2
CLK = 40500000  # OS_TIMER_CLOCK


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    raw = open(sys.argv[1], "rb").read()
    if len(raw) != 0xA040:
        print(f"unexpected .gci size {len(raw):#x} (want 0xA040 = 0x40 header + 0xA000 data)")
        return 2
    data = raw[0x40:]
    bad = 0

    print("comment  :", data[0:32].rstrip(b"\x00").decode("ascii", "replace"))
    print("date str :", data[32:64].rstrip(b"\x00").decode("ascii", "replace"))

    s = (~sum(data[:0x2040])) & 0xFFFF
    stored = (data[0x2040] << 8) | data[0x2041]
    ok = s == stored
    bad += not ok
    print(f"iconbanner cksum: stored {stored:04X} computed {s:04X} {'OK' if ok else 'BAD'}")

    offs = [(i, 0x2042 + i * BOX) for i in range(3)] + [(i + 3, 0x6000 + i * BOX) for i in range(3)]
    for n, o in offs:
        magic = data[o:o + 4]
        ck = (~sum(data[o:o + BOX - 2])) & 0xFFFF
        st = (data[o + BOX - 2] << 8) | data[o + BOX - 1]
        ok = ck == st
        bad += not ok
        verdict = "cksumOK" if ok else f"cksumBAD stored={st:04X} computed={ck:04X}"
        if magic == b"SAVE":
            t = struct.unpack(">Q", data[o + 8:o + 16])[0]
            cal = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=t / CLK)
            name = data[o + 0x10:o + 0x21].split(b"\x00")[0].decode("ascii", "replace")
            bank = struct.unpack(">H", data[o + 0x59A:o + 0x59C])[0]
            last = struct.unpack("b", data[o + 0x42E:o + 0x42F])[0]
            print(f"box{n} @0x{o:04X}: SAVE name={name!r} bankStar(BE)={bank} "
                  f"lastBoard={last} time(BE)={t:#x} -> {cal.isoformat()} {verdict}")
        else:
            print(f"box{n} @0x{o:04X}: {magic!r} {verdict}")

    print("RESULT:", "all checksums valid" if bad == 0 else f"{bad} INVALID checksum(s)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
