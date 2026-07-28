"""Focused regression tests for setup's hostile/torn FST and tree validator."""
import os
import struct
import sys
import tempfile

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))
    from setup.lib import common, nod_ffi
else:
    from . import common, nod_ffi


FILES = {
    "data/title.bin": b"dat",
    "mess/e.bin": b"message",
    "mic/voice.bin": b"voice",
    "sound/MP6_SND.msm": b"msm!",
    "sound/MP6_Str.pdt": b"pdt!!",
    "opening.bnr": b"banner",
    "dll/bootDll.rel": b"rel-data",
}


def build_fst():
    # Preorder GC FST: root, three one-file dirs, sound/two files,
    # opening.bnr, then dll/bootDll.rel.
    spec = [
        (True, "", 0, 13),
        (True, "data", 0, 3), (False, "title.bin", 0, len(FILES["data/title.bin"])),
        (True, "mess", 0, 5), (False, "e.bin", 0, len(FILES["mess/e.bin"])),
        (True, "mic", 0, 7), (False, "voice.bin", 0, len(FILES["mic/voice.bin"])),
        (True, "sound", 0, 10),
        (False, "MP6_SND.msm", 0, len(FILES["sound/MP6_SND.msm"])),
        (False, "MP6_Str.pdt", 0, len(FILES["sound/MP6_Str.pdt"])),
        (False, "opening.bnr", 0, len(FILES["opening.bnr"])),
        (True, "dll", 0, 13), (False, "bootDll.rel", 0, len(FILES["dll/bootDll.rel"])),
    ]
    strings = bytearray(b"\0")
    offsets = [0]
    for _is_dir, name, _field1, _field2 in spec[1:]:
        offsets.append(len(strings))
        strings.extend(name.encode("ascii") + b"\0")
    entries = bytearray()
    for index, (is_dir, _name, field1, field2) in enumerate(spec):
        word0 = ((1 if is_dir else 0) << 24) | offsets[index]
        entries.extend(struct.pack(">III", word0, field1, field2))
    return bytes(entries + strings)


def expect_error(fn, needle):
    try:
        fn()
    except common.SetupError as exc:
        assert needle.lower() in str(exc).lower(), (needle, str(exc))
    else:
        raise AssertionError(f"expected SetupError containing {needle!r}")


def main():
    fst = build_fst()
    manifest = dict(nod_ffi.validate_fst_bytes(fst))
    assert manifest == {path: len(data) for path, data in FILES.items()}

    # Retail GP6E01 does not reserve an empty string for the implicit root:
    # its first child (CVS) legitimately uses string-table offset zero.
    retail_style = (struct.pack(">III", 1 << 24, 0, 2) +
                    struct.pack(">III", 0, 0x1000, 1) + b"CVS\0")
    assert nod_ffi.validate_fst_bytes(retail_style) == [("CVS", 1)]

    bad = bytearray(fst)
    bad[8:12] = struct.pack(">I", 0x100001)
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "entry count")
    bad = bytearray(fst)
    bad[4:8] = struct.pack(">I", 1)
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "root directory")
    bad = bytearray(fst)
    bad[0:4] = struct.pack(">I", 2 << 24)
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "canonical directory")
    bad = bytearray(fst)
    bad[0:4] = struct.pack(">I", (1 << 24) | 1)
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "canonical directory")
    bad = bytearray(fst)
    word = int.from_bytes(bad[2 * 12:2 * 12 + 4], "big")
    bad[2 * 12:2 * 12 + 4] = struct.pack(">I", (2 << 24) | (word & 0x00FFFFFF))
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "invalid type")
    bad = bytearray(fst)
    strings_at = 13 * 12
    mic_at = bad.find(b"mic\0", strings_at)
    bad[mic_at:mic_at + 3] = b"NUL"
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "unsafe")
    bad = bytearray(fst)
    voice_at = bad.find(b"voice.bin\0", strings_at)
    bad[voice_at:voice_at + 9] = b"voi/e.bin"
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "unsafe")
    expect_error(lambda: nod_ffi.validate_fst_bytes(fst[:-1]), "unterminated")
    bad = bytearray(fst)
    mess_at = bad.find(b"mess\0", strings_at)
    bad[mess_at:mess_at + 4] = b"DATA"
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "collides")
    bad = bytearray(fst)
    bad[2 * 12 + 4:2 * 12 + 8] = struct.pack(">I", nod_ffi.DISC_MAX_BYTES)
    expect_error(lambda: nod_ffi.validate_fst_bytes(bad), "disc range")

    with tempfile.TemporaryDirectory(prefix="mp6-fst-") as root:
        os.makedirs(os.path.join(root, "sys"))
        for rel, data in FILES.items():
            path = os.path.join(root, "files", *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as out:
                out.write(data)
        with open(os.path.join(root, "sys", "boot.bin"), "wb") as out:
            out.write(b"GP6E01" + bytes(nod_ffi.BOOT_BIN_BYTES - 6))
        with open(os.path.join(root, "sys", "fst.bin"), "wb") as out:
            out.write(fst)
        with open(os.path.join(root, "sys", "main.dol"), "wb") as out:
            out.write(b"dol")
        assert nod_ffi.validate_extracted_root(root, require_build_files=True)
        with open(os.path.join(root, "files", "mic", "voice.bin"), "wb") as out:
            out.write(b"bad")
        expect_error(lambda: nod_ffi.validate_extracted_root(root), "missing/truncated")

    print("[test_fst_validate.py] PASS: structure, manifest completeness, and exact sizes enforced")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
