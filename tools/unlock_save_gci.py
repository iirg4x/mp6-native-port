"""Unlock a selected MP6 save box without replacing progress or adding currency.

Dry-run by default. --apply makes an exclusive, byte-for-byte backup first.
The game must be closed so its in-memory save cannot overwrite these changes.
Only the canonical GWSaveDebugSet unlock bits and box checksums are changed;
its 1000-star grant is deliberately not applied.
"""
import argparse
from datetime import datetime, timezone
import hashlib
from pathlib import Path
import struct

GCI_HEADER = 0x40
BOX_SIZE = 0xCB2
BOX_BODY = BOX_SIZE - 2
BOX_BASES = (0x2042, 0x6000)


def checksum(data):
    return (~sum(data)) & 0xFFFF


def validate(raw):
    if len(raw) != 0xA040 or raw[:6] != b"GP6E01":
        raise ValueError("Expected a GP6E01 Mario Party 6 GCI (0xA040 bytes)")
    data = memoryview(raw)[GCI_HEADER:]
    if checksum(data[:0x2040]) != struct.unpack_from(">H", data, 0x2040)[0]:
        raise ValueError("Invalid banner checksum; refusing to change save")
    for base in BOX_BASES:
        for box in range(3):
            start = base + box * BOX_SIZE
            if checksum(data[start:start + BOX_BODY]) != struct.unpack_from(
                    ">H", data, start + BOX_BODY)[0]:
                raise ValueError(f"Invalid box checksum at {start:#x}")


def unlock(raw, box):
    if box not in range(3):
        raise ValueError("Box must be 0, 1 or 2")
    validate(raw)
    changed = bytearray(raw)
    for base in BOX_BASES:
        start = GCI_HEADER + base + box * BOX_SIZE
        if raw[start:start + 4] != b"SAVE":
            raise ValueError("Selected primary and backup must both be SAVE boxes")
        changed[start + 0x24:start + 0x34] = b"\xff" * 16
        # Console bit order: extra board and the two extra difficulties.
        changed[start + 0x428] |= 0x70
        struct.pack_into(">H", changed, start + BOX_BODY,
                         checksum(changed[start:start + BOX_BODY]))
    validate(changed)
    return bytes(changed)


def apply(path, box):
    original = path.read_bytes()
    updated = unlock(original, box)
    if updated == original:
        return None
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    backup = path.with_name(path.name + ".before-unlock-" + stamp + ".bak")
    with backup.open("xb") as file:
        file.write(original)
    # Recheck after backup; never overwrite a save changed by a running game.
    if path.read_bytes() != original:
        raise RuntimeError(f"Save changed during backup; no changes applied. Backup: {backup}")
    temporary = path.with_name(path.name + ".unlock-" + stamp + ".tmp")
    with temporary.open("xb") as file:
        file.write(updated)
    temporary.replace(path)
    if path.read_bytes() != updated:
        raise RuntimeError(f"Save verification failed. Restore {backup}")
    return backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--box", type=int, choices=range(3), default=0)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    original = args.save.read_bytes()
    updated = unlock(original, args.box)
    print(f"Box {args.box}: {sum(a != b for a, b in zip(original, updated))} bytes change; "
          "records, stars, settings and other boxes preserved")
    print("Original SHA256:", hashlib.sha256(original).hexdigest())
    if args.apply:
        backup = apply(args.save, args.box)
        print("Backup:", backup if backup else "not needed (already unlocked)")
        print("Unlock applied; both box checksums verified")
    else:
        print("Dry run only; close the game before using --apply")


if __name__ == "__main__":
    main()
