"""Explicit, lazy source ownership for recovered non-minigame modes.

Upstream completeness is not native enablement: add a module only alongside
its ABI adaptation, reset owners, loader registration and executable tests.
Do not discover/enable modules by globbing the dependency checkout.
"""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:
    from native_namespace import namespace_flags as shared_namespace_flags


MODULES = {
    "ttwarsdll": ("game", "prolog"),
    "mgmrenshodll": ("camera", "course", "main", "messages", "player", "preview", "scenery"),
    "mgmtournamentdll": ("bracket", "camera", "characters", "host", "math", "preview", "scene", "window"),
    "mdmicdll": ("effects", "mic_mode"),
    "mdsingdll": ("mdsing", "stage"),
    "mgmbattledll": ("gameplay", "main", "preview", "stage", "window"),
    "mgmbingodll": ("text_00000", "text_00E9C", "text_014B8", "text_03B6C", "text_0BF3C", "text_10548", "text_11410"),
    "mgmdecathlondll": ("camera", "main", "players", "player_intro", "results", "scenery", "windows"),
}


def namespace_flags(decomp, module):
    root = Path(decomp)
    files = [root / "src/REL" / module / (unit + ".c") for unit in MODULES[module]]
    files.extend(sorted((root / "include/REL" / module).glob("*.h")))
    return shared_namespace_flags(
        files, module.removesuffix("dll"),
        extra_names={"_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"},
    )


def sources(decomp):
    for module, units in MODULES.items():
        flags = namespace_flags(decomp, module) + ["-D_FDLIBM_H", "-include", "math.h"]
        for unit in units:
            yield f"src/REL/{module}/{unit}.c", list(flags)
