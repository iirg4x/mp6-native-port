"""Explicit, lazy ownership of recovered system and support overlays."""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:
    from native_namespace import namespace_flags as shared_namespace_flags


MODULES = {
    "actmanDLL": ("actman",),
    "endingdll": ("ending", "effects"),
    "meschkdll": ("meschkdll",),
    "micquizdll": ("quiz", "effects"),
    "micquizseldll": ("quiz",),
    "miraclebookdll": ("setup", "book"),
    "motchkDll": ("motchk",),
    "openingDll": ("opening",),
    "optionDll": ("main", "option", "records", "sound_test"),
    "resultDll": ("battle_results", "particle_renderer", "player_results", "prolog", "team_results"),
    "safdll": ("saf", "saf_data"),
    "sequencedll": ("sequence",),
    "staffdll": ("staff",),
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
