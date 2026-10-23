"""Shared deterministic namespacing for recovered REL source units.

Each recovered overlay was originally linked as its own REL, so address-based
``fn_1_*``/``lbl_1_*`` names are local to that overlay.  The native build
links those units together and supplies a per-overlay ``-D`` namespace.  Keep
the scanner and flag ordering here so source/header aliases are handled the
same way by every native overlay generator.
"""

from pathlib import Path
import re


LOCAL_SYMBOL_RE = re.compile(r"\b(?:fn_1_|lbl_1_)[A-Za-z0-9_]+")


def symbols_from_files(paths):
    """Return all address-derived local symbols found in *paths*.

    The result is a set intentionally: declarations shared by several source
    units must produce one compiler definition, not one definition per file.
    ``paths`` is consumed in caller order for straightforward diagnostics,
    while flag emission below is sorted and therefore deterministic.
    """
    names = set()
    for path in paths:
        names.update(LOCAL_SYMBOL_RE.findall(Path(path).read_text(encoding="utf-8")))
    return names


def namespace_flags(paths, prefix, extra_names=()):
    """Emit sorted ``-D`` aliases for local symbols and lifecycle names.

    ``extra_names`` carries REL lifecycle symbols that need the same namespace
    even when a particular source set only references their native definitions.
    Sorting the original names before formatting preserves the historical flag
    order while making additions from any source/header order equivalent.
    """
    names = set(extra_names)
    names.update(symbols_from_files(paths))
    return ["-D" + name + "=" + prefix + "_" + name.lstrip("_")
            for name in sorted(names)]
