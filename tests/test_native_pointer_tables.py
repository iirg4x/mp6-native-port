"""Recovered code must not walk pointer tables in GameCube 4-byte steps."""
from pathlib import Path
import re
import unittest

from tools import apply_patches

ROOT = Path(__file__).resolve().parents[1]

# `(u8 *)&table + i * 4` (or a fixed +4/+8/...) is one element only while
# pointers are 4 bytes. E. Gadd's Garage started its ? space events from such
# a table, so on 64-bit the second event jumped to half of two pointers.
BYTE_OFFSET = re.compile(
    r"\((?:u8|s8|char)\s*\*\)\s*\(?\s*&?\s*(\w+)\s*\)?\s*\+\s*"
    r"(?:\(?\s*\w+\s*\*\s*4\b|(?:4|8|12|16)\b)"
)


def pointer_array(source, name):
    """True when `name` is declared at file scope as an array of pointers."""
    declaration = re.search(rf"(?m)^[A-Za-z_][^\n;=]*\b{re.escape(name)}\s*\[", source)
    return bool(declaration and "*" in declaration.group(0))


def gamecube_pointer_offsets(source):
    for match in BYTE_OFFSET.finditer(source):
        if pointer_array(source, match.group(1)):
            yield source.count("\n", 0, match.start()) + 1, match.group(0)


class NativePointerTables(unittest.TestCase):
    def test_detector_catches_the_garage_event_table(self):
        source = (
            "HUPROCESS *lbl_1_bss_18[5];\n"
            "void (*lbl_1_data_874[5])(void);\n"
            "s32 lbl_1_bss_3C[4];\n"
            "void f(s32 slot) {\n"
            "    *(s32 *) ((u8 *) &lbl_1_bss_3C + (slot * 4)) = -1;\n"
            "    HuPrcChildCreate(*(void (**)(void))((u8 *) &lbl_1_data_874 + (slot * 4)));\n"
            "    if (*(void **)((u8 *)&lbl_1_bss_18 + 4) != NULL) {}\n"
            "}\n"
        )
        self.assertEqual([line for line, _ in gamecube_pointer_offsets(source)], [6, 7])

    def test_no_patched_source_indexes_pointers_by_gamecube_offsets(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        for path in sorted((decomp / "src").rglob("*.c")):
            relative = path.relative_to(decomp).as_posix()
            source = path.read_text(encoding="utf-8", errors="replace")
            patch = ROOT / "compat/decomp" / (relative + ".patch")
            if patch.exists():
                source = apply_patches.apply_unified_diff(
                    source, patch.read_text(encoding="utf-8"), relative
                )
            hits = list(gamecube_pointer_offsets(source))
            with self.subTest(source=relative):
                self.assertEqual(hits, [], "index pointer tables, never step them by 4 bytes")


if __name__ == "__main__":
    unittest.main()
