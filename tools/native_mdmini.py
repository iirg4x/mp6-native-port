"""Native Minigame Mode entrance; upstream owns both code and typed data.

No disc-derived data mirror or import-time checkout access is needed. Native
reload resets and rendering adaptations remain in the compatibility patches.
"""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:
    from native_namespace import namespace_flags as shared_namespace_flags

UNITS = ("menu_setup", "particle_effects")


def namespace_flags(decomp):
    root = Path(decomp)
    files = [root / "src/REL/mdminidll" / f"{unit}.c" for unit in UNITS]
    files += sorted((root / "include/REL/mdminidll").glob("*.h"))
    return shared_namespace_flags(
        files, "mdmini", extra_names={"_prolog", "_epilog", "_ctors", "_dtors"},
    )


def sources(decomp):
    flags = namespace_flags(decomp) + [
        "-D_FDLIBM_H", "-include", "math.h", "-include", "stdlib.h",
        "-include", "string.h", "-include", "mp6_ppc_math.h",
        "-include", "game/hsfex.h",
    ]
    for unit in UNITS:
        yield f"src/REL/mdminidll/{unit}.c", list(flags)
