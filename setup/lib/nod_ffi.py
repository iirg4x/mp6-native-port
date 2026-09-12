"""setup/lib/nod_ffi.py -- minimal ctypes binding against nod's C FFI
(encounter/nod, MIT OR Apache-2.0), the same GC/Wii disc-image library the
port's own on-device importer uses (src/content/content_import.cpp).

Why a fresh ctypes binding instead of reusing content_import.cpp directly:
that code is compiled INTO mp6native.exe/.so -- it doesn't exist yet the
first time this setup tool runs (chicken-and-egg: we need the disc's
contents extracted before build.py has anything to compile). This module
talks to the SAME prebuilt nod.dll (port/toolchain/nod/, fetched by
tools/fetch_nod.py) through the same public C API (nod.h), replicating
content_import.cpp's proven shape:
  - nod_disc_open() directly from a file path (no stream shim needed --
    content_import.cpp needed one only for Android content:// URIs, which
    do not apply to this desktop setup tool).
  - nod_disc_header() for the GP6E01 game-ID check (same message wording
    as content_import.cpp's validate_game_id(), by design).
  - nod_partition_meta() for the sys/ blobs (boot.bin/bi2.bin/apploader.img/
    main.dol/fst.bin) -- straight in-memory slices, no per-file FST lookup
    needed for these.
  - nod_partition_iterate_fst() + nod_partition_open_file() + nod_read()
    for files/ -- the same directory-stack walk content_import.cpp's
    fst_callback() uses, ported to Python.

Struct layouts are transcribed field-for-field from
port/toolchain/nod/include/nod.h (fetched at NOD_VERSION pinned in
tools/fetch_nod.py); ctypes.Structure uses natural (C-compatible) alignment
by default, so matching field order + type is sufficient -- no manual
padding math needed. Options pointers are always passed as NULL (None):
nod.h documents this as "use defaults", so NodDiscOptions/NodPartitionOptions
layouts are intentionally never modeled here.
"""
import ctypes
import os
import platform

from . import common

NOD_RESULT_OK = 0
NOD_PARTITION_KIND_DATA = 0
NOD_NODE_KIND_FILE = 0
NOD_NODE_KIND_DIRECTORY = 1
NOD_FST_STOP = 0xFFFFFFFF

_RESULT_NAMES = {
    0: "OK",
    1: "ERR_IO",
    2: "ERR_FORMAT",
    3: "ERR_NOT_FOUND",
    4: "ERR_INVALID_HANDLE",
    5: "ERR_OTHER",
}


class NodError(Exception):
    pass


class NodBlob(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
    ]


class NodDiscHeader(ctypes.Structure):
    _fields_ = [
        ("game_id", ctypes.c_char * 6),
        ("disc_num", ctypes.c_uint8),
        ("disc_version", ctypes.c_uint8),
        ("audio_streaming", ctypes.c_uint8),
        ("audio_stream_buf_size", ctypes.c_uint8),
        ("_pad1", ctypes.c_uint8 * 14),
        ("wii_magic", ctypes.c_uint8 * 4),
        ("gcn_magic", ctypes.c_uint8 * 4),
        ("game_title", ctypes.c_char * 64),
        ("no_partition_hashes", ctypes.c_uint8),
        ("no_partition_encryption", ctypes.c_uint8),
        ("_pad2", ctypes.c_uint8 * 926),
    ]


class NodPartitionMeta(ctypes.Structure):
    _fields_ = [
        ("raw_boot", NodBlob),
        ("raw_bi2", NodBlob),
        ("raw_apploader", NodBlob),
        ("raw_dol", NodBlob),
        ("raw_fst", NodBlob),
        ("raw_ticket", NodBlob),
        ("raw_tmd", NodBlob),
        ("raw_cert_chain", NodBlob),
        ("raw_h3_table", NodBlob),
    ]


# uint32_t (*)(uint32_t index, enum NodNodeKind kind, const char *name, uint32_t size, void *user_data)
NodFstCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int32, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_void_p
)


class Nod:
    """Loads nod.dll and exposes the handful of entry points the setup tool
    needs, with Python-friendly wrappers (exceptions instead of NodResult
    codes)."""

    def __init__(self, dll_path):
        if not os.path.exists(dll_path):
            raise NodError(f"nod library not found: {dll_path}")
        self.lib = ctypes.CDLL(dll_path)
        self._bind()

    def _bind(self):
        lib = self.lib
        lib.nod_error_message.argtypes = []
        lib.nod_error_message.restype = ctypes.c_char_p

        lib.nod_disc_open.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.nod_disc_open.restype = ctypes.c_int32

        lib.nod_disc_header.argtypes = [ctypes.c_void_p, ctypes.POINTER(NodDiscHeader)]
        lib.nod_disc_header.restype = ctypes.c_int32

        lib.nod_disc_open_partition_kind.argtypes = [
            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
        ]
        lib.nod_disc_open_partition_kind.restype = ctypes.c_int32

        lib.nod_partition_meta.argtypes = [ctypes.c_void_p, ctypes.POINTER(NodPartitionMeta)]
        lib.nod_partition_meta.restype = ctypes.c_int32

        lib.nod_partition_iterate_fst.argtypes = [ctypes.c_void_p, NodFstCallback, ctypes.c_void_p]
        lib.nod_partition_iterate_fst.restype = None

        lib.nod_partition_open_file.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
        lib.nod_partition_open_file.restype = ctypes.c_int32

        lib.nod_read.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        lib.nod_read.restype = ctypes.c_int64

        lib.nod_free.argtypes = [ctypes.c_void_p]
        lib.nod_free.restype = None

    def last_error(self):
        msg = self.lib.nod_error_message()
        return msg.decode("utf-8", "replace") if msg else "(no nod error message)"

    def _check(self, result, what):
        if result != NOD_RESULT_OK:
            name = _RESULT_NAMES.get(result, str(result))
            raise NodError(f"{what} failed: {name} -- {self.last_error()}")

    def disc_open(self, path):
        handle = ctypes.c_void_p()
        rc = self.lib.nod_disc_open(path.encode("utf-8"), None, ctypes.byref(handle))
        self._check(rc, f"nod_disc_open({path!r})")
        if not handle.value:
            raise NodError(f"nod_disc_open({path!r}) returned OK but a null handle")
        return handle

    def disc_header(self, disc_handle):
        header = NodDiscHeader()
        rc = self.lib.nod_disc_header(disc_handle, ctypes.byref(header))
        self._check(rc, "nod_disc_header")
        return header

    def open_data_partition(self, disc_handle):
        handle = ctypes.c_void_p()
        rc = self.lib.nod_disc_open_partition_kind(
            disc_handle, NOD_PARTITION_KIND_DATA, None, ctypes.byref(handle)
        )
        self._check(rc, "nod_disc_open_partition_kind(DATA)")
        return handle

    def partition_meta(self, part_handle):
        meta = NodPartitionMeta()
        rc = self.lib.nod_partition_meta(part_handle, ctypes.byref(meta))
        self._check(rc, "nod_partition_meta")
        return meta

    def iterate_fst(self, part_handle, py_callback):
        """py_callback(index, kind, name_bytes_or_None, size) -> next index
        (or NOD_FST_STOP). Exceptions raised inside py_callback are caught,
        stored, and re-raised after the C call returns (never let a Python
        exception unwind through a ctypes callback boundary)."""
        pending = []

        def trampoline(index, kind, name, size, _user_data):
            try:
                return py_callback(index, kind, name, size)
            except Exception as exc:  # noqa: BLE001 -- must not escape the C boundary
                pending.append(exc)
                return NOD_FST_STOP

        cb = NodFstCallback(trampoline)
        self.lib.nod_partition_iterate_fst(part_handle, cb, None)
        if pending:
            raise pending[0]

    def open_file(self, part_handle, fst_index):
        handle = ctypes.c_void_p()
        rc = self.lib.nod_partition_open_file(part_handle, fst_index, ctypes.byref(handle))
        self._check(rc, f"nod_partition_open_file(index={fst_index})")
        return handle

    def read_all_to(self, file_handle, size, write_fn, chunk=1024 * 1024):
        buf = (ctypes.c_uint8 * chunk)()
        left = size
        while left > 0:
            want = min(left, chunk)
            got = self.lib.nod_read(file_handle, buf, want)
            if got < 0:
                raise NodError(f"nod_read error: {self.last_error()}")
            if got == 0:
                raise NodError("nod_read: unexpected end-of-stream before declared size")
            if got > want:
                raise NodError(f"nod_read returned {got} bytes for a {want}-byte request")
            write_fn(bytes(buf[:got]))
            left -= got

    def free(self, handle):
        if handle and handle.value:
            self.lib.nod_free(handle)


def blob_bytes(blob):
    if not blob.data or blob.size == 0:
        return b""
    return ctypes.string_at(blob.data, blob.size)


# ---------------------------------------------------------------------------
# Game-ID validation -- same two error messages as
# src/content/content_import.cpp's validate_game_id(), on purpose (this
# is the same product-facing check, just running before the game binary
# exists).
# ---------------------------------------------------------------------------

EXPECTED_GAME_ID = b"GP6E01"
MAX_FST_BYTES = 8 * 1024 * 1024
MAX_FST_ENTRIES = 65_536
MAX_WANTED_BYTES = 1536 * 1024 * 1024  # 1.5 GiB; real GP6E01 stays below this.
BOOT_BIN_BYTES = 0x440
DISC_MAX_BYTES = 1_459_978_240


def validate_game_id(game_id6):
    """game_id6: 6 raw bytes. Returns (ok, message). message is None on ok."""
    if game_id6 == EXPECTED_GAME_ID:
        return True, None
    shown = "".join(chr(b) if 0x20 <= b <= 0x7E else "?" for b in game_id6)
    if game_id6[:3] == b"GP6":
        return False, f"wrong region: this disc is {shown} -- the port needs the USA release (GP6E01)"
    return False, f"not Mario Party 6: game ID {shown} (need GP6E01, Mario Party 6 USA)"


def validate_boot_bytes(data):
    if not isinstance(data, (bytes, bytearray)) or len(data) != BOOT_BIN_BYTES:
        raise common.SetupError(
            f"boot.bin must be exactly {BOOT_BIN_BYTES} bytes (the GameCube boot header)"
        )
    ok, msg = validate_game_id(bytes(data[:6]))
    if not ok:
        raise common.SetupError(msg)
    return True


# ---------------------------------------------------------------------------
# The wanted file set for the decomp's orig/GP6E01 tree. This is a full
# "Extract Entire Disc" EXCEPT files/movie/ (336MB of FMVs; the port's own
# on-device importer defers these for the same reason -- see
# content_import.h's header comment -- neither the build nor the boot-to-
# menu slice this tool verifies ever reads them; --include-movies opts back
# in for full fidelity/cutscenes at the cost of a much longer extraction).
# ---------------------------------------------------------------------------

def is_safe_rel(rel):
    """SECURITY: mirror of src/content/content_path_safe.h's
    mp6_content_path_is_safe_rel(). An FST entry name is attacker-controlled
    (raw bytes from the disc's string table), and extract_disc_image() joins
    it onto dest_root to form a write path -- so a crafted name like
    'data/../../evil', '/abs/evil', '..\\win' or 'C:\\x' must be rejected or
    the extraction escapes the chosen root.

    True iff `rel` is non-empty, not absolute, and every '/'-separated
    component is a plain name: never '', '.', '..', and free of '\\', ':' or
    any control byte.
    """
    if not rel or rel[0] == "/":
        return False
    reserved = {"CON", "PRN", "AUX", "NUL", "CLOCK$"}
    for comp in rel.split("/"):
        if comp in ("", ".", ".."):
            return False
        if comp[-1] in (".", " "):
            return False
        if "\\" in comp or ":" in comp or any(ch in '<>"|?*' for ch in comp):
            return False
        if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in comp):
            return False
        base = comp.split(".", 1)[0].upper()
        if base in reserved or (len(base) == 4 and base[:3] in ("COM", "LPT")
                                and base[3] in "123456789"):
            return False
    return True


def validate_fst_bytes(data):
    """Reject an FST before nod/setup/runtime can traverse unchecked offsets."""
    if not isinstance(data, (bytes, bytearray)) or len(data) < 12:
        raise common.SetupError("sys/fst.bin is too small")
    if len(data) > MAX_FST_BYTES:
        raise common.SetupError("sys/fst.bin exceeds the 8 MiB safety limit")

    def be32(off):
        return int.from_bytes(data[off:off + 4], "big")

    root_word = be32(0)
    if (root_word >> 24) != 1 or (root_word & 0x00FFFFFF) != 0:
        raise common.SetupError("sys/fst.bin root entry is not the canonical directory entry")
    if be32(4) != 0:
        raise common.SetupError("sys/fst.bin root directory has a nonzero parent index")
    count = be32(8)
    if count <= 0 or count > MAX_FST_ENTRIES or count * 12 > len(data):
        raise common.SetupError(
            f"sys/fst.bin entry count {count} does not fit in the {len(data)}-byte file"
        )
    strings_at = count * 12
    stack = [(0, count, "")]  # index, subtree end, full path
    files = []
    seen_host_paths = set()
    for index in range(1, count):
        while stack and index >= stack[-1][1]:
            stack.pop()
        if not stack:
            raise common.SetupError(f"sys/fst.bin entry {index} lies outside the root subtree")
        off = index * 12
        word0 = be32(off)
        entry_type = word0 >> 24
        if entry_type not in (0, 1):
            raise common.SetupError(f"sys/fst.bin entry {index} has invalid type {entry_type}")
        name_off = word0 & 0x00FFFFFF
        name_at = strings_at + name_off
        if name_at < strings_at or name_at >= len(data):
            raise common.SetupError(f"sys/fst.bin entry {index} has an out-of-range name offset")
        nul = data.find(b"\0", name_at)
        if nul < 0 or nul == name_at:
            raise common.SetupError(f"sys/fst.bin entry {index} has an empty/unterminated name")
        name = bytes(data[name_at:nul]).decode("latin-1")
        if "/" in name or not is_safe_rel(name):
            raise common.SetupError(f"sys/fst.bin entry {index} has an unsafe host filename: {name!r}")
        full = f"{stack[-1][2]}/{name}" if stack[-1][2] else name
        if not is_safe_rel(full):
            raise common.SetupError(f"sys/fst.bin entry {index} has an unsafe host path: {full!r}")
        if len(full) >= 1024:
            raise common.SetupError(f"sys/fst.bin entry {index} path exceeds 1023 bytes")
        folded = full.upper()
        if folded in seen_host_paths:
            raise common.SetupError(
                f"sys/fst.bin entry {index} collides with another host path: {full!r}"
            )
        seen_host_paths.add(folded)
        if entry_type == 1:
            parent, end = be32(off + 4), be32(off + 8)
            if parent != stack[-1][0] or end <= index or end > stack[-1][1]:
                raise common.SetupError(f"sys/fst.bin directory {index} has an invalid parent/end")
            if len(stack) >= 256:
                raise common.SetupError("sys/fst.bin directory nesting exceeds 256 levels")
            stack.append((index, end, full))
        else:
            position, size = be32(off + 4), be32(off + 8)
            if position + size > DISC_MAX_BYTES:
                raise common.SetupError(
                    f"sys/fst.bin file entry {index} exceeds the GameCube disc range"
                )
            files.append((full, size))
    return files


def _runtime_wanted(rel):
    return (rel in ("opening.bnr", "sound/MP6_SND.msm", "sound/MP6_Str.pdt")
            or rel == "data" or rel.startswith("data/")
            or rel == "mess" or rel.startswith("mess/")
            or rel == "mic" or rel.startswith("mic/"))


def validate_extracted_root(root, require_build_files=False):
    boot = os.path.join(root, "sys", "boot.bin")
    fst = os.path.join(root, "sys", "fst.bin")
    try:
        with open(boot, "rb") as f:
            boot_bytes = f.read(BOOT_BIN_BYTES + 1)
    except OSError as exc:
        raise common.SetupError(f"missing or unreadable {boot}: {exc}") from exc
    validate_boot_bytes(boot_bytes)
    try:
        with open(fst, "rb") as f:
            manifest = validate_fst_bytes(f.read(MAX_FST_BYTES + 1))
    except OSError as exc:
        raise common.SetupError(f"missing or unreadable {fst}: {exc}") from exc
    found = set()
    wanted_bytes = 0
    for rel, expected_size in manifest:
        if not _runtime_wanted(rel):
            continue
        wanted_bytes += expected_size
        if wanted_bytes > MAX_WANTED_BYTES:
            raise common.SetupError("GP6E01 runtime file set exceeds the 1.5 GiB safety limit")
        path = os.path.join(root, "files", *rel.split("/"))
        if not os.path.isfile(path) or os.path.getsize(path) != expected_size:
            raise common.SetupError(
                f"missing/truncated runtime file from fst.bin: {path} "
                f"(expected {expected_size} bytes)"
            )
        if expected_size > 0 and rel in ("opening.bnr", "sound/MP6_SND.msm", "sound/MP6_Str.pdt"):
            found.add(rel)
        for prefix in ("data/", "mess/", "mic/"):
            if expected_size > 0 and rel.startswith(prefix):
                found.add(prefix[:-1])
    required = {"opening.bnr", "sound/MP6_SND.msm", "sound/MP6_Str.pdt", "data", "mess", "mic"}
    if not required.issubset(found):
        missing = ", ".join(sorted(required - found))
        raise common.SetupError(f"incomplete GP6E01 fst.bin/runtime tree; missing: {missing}")
    if require_build_files:
        dol = os.path.join(root, "sys", "main.dol")
        dll_entries = [(rel, size) for rel, size in manifest
                       if rel.startswith("dll/") and rel.lower().endswith(".rel")]
        if not os.path.isfile(dol) or os.path.getsize(dol) <= 0 or not dll_entries:
            raise common.SetupError(
                f"incomplete build extraction under {root}: need sys/main.dol and files/dll/*.rel"
            )
        for rel, expected_size in dll_entries:
            path = os.path.join(root, "files", *rel.split("/"))
            if not os.path.isfile(path) or os.path.getsize(path) != expected_size:
                raise common.SetupError(
                    f"missing/truncated build REL from fst.bin: {path} (expected {expected_size} bytes)"
                )
    return True


def _skip_subtree(rel, include_movies):
    if include_movies:
        return False
    return rel == "movie" or rel.startswith("movie/")


def find_nod_dll(nod_dir):
    system = platform.system()
    machine = platform.machine().lower()
    if machine in ("amd64", "x86_64"):
        machine = "x86_64"
    elif machine in ("arm64", "aarch64"):
        machine = "aarch64"
    if (system, machine) == ("Windows", "x86_64"):
        return os.path.join(nod_dir, "windows-x86_64", "bin", "nod.dll")
    if (system, machine) == ("Linux", "x86_64"):
        return os.path.join(nod_dir, "linux-x86_64", "lib", "libnod.so")
    if (system, machine) == ("Darwin", "aarch64"):
        return os.path.join(nod_dir, "macos-aarch64", "lib", "libnod.dylib")
    raise common.SetupError(
        f"nod has no pinned setup-tool binary for {system}/{machine}; "
        "supported hosts are Windows/x86_64, Linux/x86_64, and macOS/aarch64"
    )


def extract_disc_image(image_path, dest_root, nod_dll_path, include_movies=False, progress=None):
    """Extracts a GC disc image (.iso/.gcm/.ciso/.rvz/.gcz/.wia/...) into
    dest_root (which will directly contain sys/ and files/). Raises NodError
    on any nod-level failure; raises common.SetupError with a friendly,
    content_import.cpp-matching message on a game-ID mismatch.

    Returns a dict summary: {"files": N, "bytes": N, "game_id": "GP6E01"}.
    """
    nod = Nod(nod_dll_path)
    disc = nod.disc_open(image_path)
    try:
        header = nod.disc_header(disc)
        game_id = bytes(header.game_id)
        ok, msg = validate_game_id(game_id)
        if not ok:
            raise common.SetupError(msg, hint="point --disc at a real, legally-owned Mario Party 6 (USA) disc image")

        part = nod.open_data_partition(disc)
        try:
            meta = nod.partition_meta(part)
            if meta.raw_boot.size != BOOT_BIN_BYTES:
                raise common.SetupError(
                    f"boot.bin must be exactly {BOOT_BIN_BYTES} bytes (the GameCube boot header)"
                )
            if meta.raw_fst.size > MAX_FST_BYTES:
                raise common.SetupError("sys/fst.bin exceeds the 8 MiB safety limit")
            boot_bytes = blob_bytes(meta.raw_boot)
            validate_boot_bytes(boot_bytes)
            fst_bytes = blob_bytes(meta.raw_fst)
            validate_fst_bytes(fst_bytes)
            sys_dir = os.path.join(dest_root, "sys")
            common.ensure_dir(sys_dir)
            sys_files = [
                ("boot.bin", meta.raw_boot),
                ("bi2.bin", meta.raw_bi2),
                ("apploader.img", meta.raw_apploader),
                ("main.dol", meta.raw_dol),
                # fst.bin is written LAST (below) -- torn-import safety, same
                # contract as content_import.cpp: it's the one file every
                # "is this content present" probe in the port checks for.
            ]
            total_sys_bytes = 0
            for name, blob in sys_files:
                data = blob_bytes(blob)
                with open(os.path.join(sys_dir, name), "wb") as f:
                    f.write(data)
                total_sys_bytes += len(data)

            # --- Walk the FST, collecting wanted files -------------------
            # NOD CONTRACT: nod_partition_iterate_fst() never delivers the
            # root node -- its loop starts at node index 1 (nod-ffi/src/
            # lib.rs:680, "let mut idx: usize = 1; // skip root node").  The
            # root frame is therefore seeded here from the same raw fst.bin
            # nod parses: the root node's length field (big-endian at byte
            # offset 8) is the node count, i.e. the root's child-end index,
            # and validate_fst_bytes() above has already range-checked it.
            # content_import.cpp's fst_callback() seeds the identical frame.
            root_end = int.from_bytes(fst_bytes[8:12], "big")
            walk_dirs = [(root_end, "")]  # stack of (end_index, prefix)
            wanted_files = []  # (index, size, rel)
            skipped_dirs = []
            unsafe_skipped = []  # SECURITY: FST names that would escape dest_root

            def cb(index, kind, name, size):
                while walk_dirs and index >= walk_dirs[-1][0]:
                    walk_dirs.pop()
                prefix = walk_dirs[-1][1] if walk_dirs else ""
                name_s = name.decode("utf-8", "replace") if name else ""
                rel = f"{prefix}/{name_s}" if prefix else name_s
                if kind == NOD_NODE_KIND_DIRECTORY:
                    if not is_safe_rel(rel):
                        # A traversal/absolute/drive-qualified directory name:
                        # prune the whole subtree so no child is ever written.
                        unsafe_skipped.append(rel)
                        return size
                    if _skip_subtree(rel, include_movies):
                        skipped_dirs.append(rel)
                        return size  # child-end index: skip the whole subtree
                    walk_dirs.append((size, rel))
                    return index + 1
                if not is_safe_rel(rel):
                    unsafe_skipped.append(rel)
                    return index + 1
                wanted_files.append((index, size, rel))
                return index + 1

            nod.iterate_fst(part, cb)

            if unsafe_skipped:
                common.warn(f"ignored {len(unsafe_skipped)} disc entry name(s) that would escape "
                            f"the extraction root (e.g. {unsafe_skipped[0]!r}) -- possible tampered image")

            if not wanted_files:
                raise common.SetupError(
                    "the disc's file table had no files after filtering -- this doesn't look like a "
                    "real Mario Party 6 data partition"
                )

            total_bytes = sum(sz for _, sz, _ in wanted_files) + total_sys_bytes + len(fst_bytes)
            if total_bytes > MAX_WANTED_BYTES:
                raise common.SetupError("selected disc content exceeds the 1.5 GiB safety limit")
            done_bytes = total_sys_bytes
            for n, (index, size, rel) in enumerate(wanted_files):
                dest_path = os.path.join(dest_root, "files", *rel.split("/"))
                common.ensure_dir(os.path.dirname(dest_path))
                fh = nod.open_file(part, index)
                try:
                    with open(dest_path, "wb") as out:
                        nod.read_all_to(fh, size, out.write)
                finally:
                    nod.free(fh)
                done_bytes += size
                if progress:
                    progress(done_bytes, total_bytes, rel)

            # fst.bin last (torn-import safety).
            with open(os.path.join(sys_dir, "fst.bin"), "wb") as f:
                f.write(fst_bytes)
            done_bytes += len(fst_bytes)
            if progress:
                progress(done_bytes, total_bytes, "sys/fst.bin")

            return {
                "files": len(wanted_files) + len(sys_files) + 1,
                "bytes": done_bytes,
                "game_id": game_id.decode("ascii", "replace"),
                "skipped_dirs": skipped_dirs,
            }
        finally:
            nod.free(part)
    finally:
        nod.free(disc)


# ---------------------------------------------------------------------------
# Already-extracted-folder source (no nod needed) -- mirrors
# content_import.cpp's folder_disc_root()/folder_collect(), but grabs the
# FULL set (minus movie/ by default) since this feeds the decomp's own
# orig/GP6E01, not just the port's runtime asset subset.
# ---------------------------------------------------------------------------

def find_folder_disc_root(picked):
    for candidate in ("", "GP6E01", "DATA"):
        root = os.path.join(picked, candidate) if candidate else picked
        if os.path.isfile(os.path.join(root, "sys", "fst.bin")) and os.path.isdir(os.path.join(root, "files")):
            return root
    return None


def extract_disc_folder(folder_path, dest_root, include_movies=False, progress=None):
    import shutil

    root = find_folder_disc_root(folder_path)
    if root is None:
        raise common.SetupError(
            f"{folder_path} doesn't look like an extracted GameCube disc "
            "(need sys/fst.bin + files/ inside it, or a GP6E01/DATA folder containing them)"
        )

    validate_extracted_root(root)
    with open(os.path.join(root, "sys", "boot.bin"), "rb") as f:
        game_id = f.read(6)

    files_root = os.path.join(root, "files")
    to_copy = []
    with open(os.path.join(root, "sys", "fst.bin"), "rb") as f:
        manifest = validate_fst_bytes(f.read(MAX_FST_BYTES + 1))
    # The validated FST is authoritative. Never enumerate the host folder:
    # an extra wanted-looking file not named by the disc manifest must not be
    # smuggled into a prepared GP6E01 tree.
    for rel, expected_size in manifest:
        if _skip_subtree(rel, include_movies):
            continue
        src = os.path.join(files_root, *rel.split("/"))
        if not os.path.isfile(src) or os.path.getsize(src) != expected_size:
            raise common.SetupError(
                f"missing/truncated file required by fst.bin: {src} (expected {expected_size} bytes)"
            )
        to_copy.append(rel)

    total = len(to_copy)
    done_bytes = 0
    total_bytes = sum(os.path.getsize(os.path.join(files_root, *r.split("/"))) for r in to_copy)
    if total_bytes > MAX_WANTED_BYTES:
        raise common.SetupError("selected folder content exceeds the 1.5 GiB safety limit")
    for i, rel in enumerate(to_copy):
        src = os.path.join(files_root, *rel.split("/"))
        dst = os.path.join(dest_root, "files", *rel.split("/"))
        common.ensure_dir(os.path.dirname(dst))
        shutil.copy2(src, dst)
        done_bytes += os.path.getsize(src)
        if progress:
            progress(done_bytes, total_bytes, rel)

    sys_dst = os.path.join(dest_root, "sys")
    common.ensure_dir(sys_dst)
    sys_src = os.path.join(root, "sys")
    fst_last = None
    for name in os.listdir(sys_src):
        s, d = os.path.join(sys_src, name), os.path.join(sys_dst, name)
        if name.lower() == "fst.bin":
            fst_last = (s, d)
            continue
        shutil.copy2(s, d)
    if fst_last:
        shutil.copy2(*fst_last)  # fst.bin last -- torn-import safety

    return {"files": total + len(os.listdir(sys_src)), "bytes": done_bytes,
            "game_id": game_id.decode("ascii", "replace")}
