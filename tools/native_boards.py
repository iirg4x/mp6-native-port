"""Explicit, lazy ownership of newly recovered native board overlays."""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:
    from native_namespace import namespace_flags as shared_namespace_flags


MODULES = {
    "w02Dll": ("text_A0", "prolog"),
    "w03Dll": ("board", "prolog"),
    "w04Dll": ("board", "prolog"),
    "w05Dll": ("board", "prolog"),
    "w06Dll": ("board_main", "text_0E3EC"),
    "w10Dll": ("board", "prolog"),
    "w11Dll": ("world11_board", "prolog"),
    "s01Dll": ("s01", "prolog"),
    "s02Dll": ("s02",),
    "s03Dll": ("s03",),
}


def namespace_flags(decomp, module):
    root = Path(decomp)
    files = [root / "src/REL" / module / (unit + ".c") for unit in MODULES[module]]
    files.extend(sorted((root / "include/REL" / module).glob("*.h")))
    return shared_namespace_flags(
        files, module.lower().removesuffix("dll"),
        extra_names={"_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"},
    )


def sources(decomp):
    for module, units in MODULES.items():
        flags = namespace_flags(decomp, module) + ["-D_FDLIBM_H", "-include", "math.h"]
        for unit in units:
            yield f"src/REL/{module}/{unit}.c", list(flags)
