"""Native integration for the recovered MGMfree menu overlay.

Only the two recovered game units are compiled.  The overlay's embedded
PowerPC runtime is supplied by the native executable and must never be linked
as a second copy.  Every REL-local function and storage owner is namespaced
with the full module name because several other recovered overlays use the
same address-derived identifiers.
"""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:  # build.py imports tools modules as top-level modules.
    from native_namespace import namespace_flags as shared_namespace_flags


UNITS = ("menu", "mode_flow")


def namespace_flags(decomp):
    root = Path(decomp)
    files = [root / "src/REL/mgmfreedll" / f"{unit}.c" for unit in UNITS]
    return shared_namespace_flags(
        files, "mgmfree",
        extra_names={"_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"},
    )


def sources(decomp):
    flags = namespace_flags(decomp) + [
        "-D_FDLIBM_H", "-include", "math.h", "-include", "stdlib.h",
        "-include", "stdio.h", "-include", "game/audio.h", "-include", "game/hsfex.h",
    ]
    for unit in UNITS:
        yield f"src/REL/mgmfreedll/{unit}.c", list(flags)
