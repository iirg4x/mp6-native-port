"""Focused unit test for nod_ffi.is_safe_rel (SECURITY: FST-name traversal
gate). Exercises the validator directly -- no ISO is forged. Run from the
mp6-native repo root:

    python setup/lib/test_path_safe.py

Mirrors platform/content/test_path_safe.cpp so the C++ and Python gates stay
in lockstep.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))   # setup/lib
sys.path.insert(0, os.path.dirname(_HERE))            # setup/ on path -> `lib` package
from lib.nod_ffi import is_safe_rel  # noqa: E402

# (rel, expected_safe)
CASES = [
    # --- the traversal / escape attempts: all must be REJECTED ---
    ("data/../../evil", False),
    ("/abs/evil", False),
    ("..\\win", False),          # backslash separator + ".." component
    ("..", False),
    (".", False),
    ("data/..", False),
    ("data/./x", False),         # "." component
    ("data//x", False),          # empty component (doubled slash)
    ("data/", False),            # trailing slash -> empty last component
    ("C:/x", False),             # drive-qualified
    ("C:\\x", False),
    ("data\\sub\\evil", False),  # windows separators inside a component
    ("data/sub/../../../x", False),
    ("data/e\x00vil", False),    # control byte (NUL) inside a name
    ("data/e\nvil", False),      # control byte (LF)
    ("", False),
    # --- the legitimate wanted-set paths: all must be ACCEPTED ---
    ("data/x.bin", True),
    ("data/sub/file.bin", True),
    ("opening.bnr", True),
    ("sound/MP6_SND.msm", True),
    ("mess/e/message.bin", True),
    ("data/...oddbutlegal", True),  # "..." is a real name, not traversal
    ("data/.hidden", True),         # leading dot is a legal name
]


def _resolves_under_root(rel, root):
    root_n = os.path.normpath(root)
    joined = os.path.normpath(os.path.join(root, *rel.split("/")))
    return joined == root_n or joined.startswith(root_n + os.sep)


def main():
    root = os.path.normpath("/tmp/mp6dest/files")
    failures = []
    for rel, expected in CASES:
        got = bool(is_safe_rel(rel))
        if got != expected:
            failures.append(f"  is_safe_rel({rel!r}) = {got}, expected {expected}")
            continue
        # An accepted path MUST lexically resolve under the extraction root;
        # a rejected one must NOT (that is the whole point of rejecting it).
        under = _resolves_under_root(rel, root) if rel else False
        if expected and not under:
            failures.append(f"  {rel!r} accepted but resolves OUTSIDE root ({root})")
        if (not expected) and rel and under and ".." not in rel and not rel.startswith("/"):
            # Sanity: rejected purely-lexical cases are fine to also be "under";
            # only assert the escape cases actually escape.
            pass

    # Explicit escape assertions: the traversal cases must land outside root.
    for rel in ("data/../../evil", "/abs/evil", "data/sub/../../../x"):
        assert not is_safe_rel(rel), rel
        if not rel.startswith("/"):
            outside = not _resolves_under_root(rel, root)
            if not outside:
                failures.append(f"  {rel!r} was expected to escape root but did not")

    if failures:
        print("[test_path_safe.py] FAIL:")
        print("\n".join(failures))
        return 1
    print(f"[test_path_safe.py] PASS: {len(CASES)} cases -- traversal/absolute/drive/"
          "control names rejected, wanted-set paths accepted and rooted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
