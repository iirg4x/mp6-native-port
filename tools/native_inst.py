"""Explicit native closure of the recovered minigame instruction overlay.

Namespace discovery is lazy: importing the build driver before the decomp
checkout has been fetched must remain supported. The PPC runtime wrapper is
provided by the host compiler and is not a game source unit.
"""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:
    from native_namespace import namespace_flags as shared_namespace_flags


UNITS = ("prolog", "setup", "stage", "display", "name")


def namespace_flags(decomp):
    files = [Path(decomp) / "src/REL/instDll" / f"{unit}.c" for unit in UNITS]
    return shared_namespace_flags(
        files, "inst",
        extra_names={"_prolog", "_epilog", "_ctors", "_dtors"},
    )


def sources(decomp):
    flags = namespace_flags(decomp) + [
        "-D_FDLIBM_H", "-include", "math.h", "-include", "stdlib.h",
        "-include", "string.h", "-include", "stdint.h",
        "-include", "game/hsfex.h",
    ]
    for unit in UNITS:
        extra = ["-include", "game/saveload.h"] if unit == "setup" else []
        yield f"src/REL/instDll/{unit}.c", flags + extra
