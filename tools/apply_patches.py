#!/usr/bin/env python3
"""apply_patches.py -- surgical build-time patch queue.

The decomp repo (external_refs/repos/marioparty6) stays completely
untouched (docs/DECOMP_DEPENDENCY.md) -- it is consumed read-only. For the
small set of central data/container-parsing files that need big-endian
accessor fixes (docs/ARCHITECTURE.md's "Data endianness discipline"), this
script copies the NAMED original source into
build/patched-src/<same relative path>, applying the matching .patch file
from patches/decomp/<same relative path>.patch on the way -- and
tools/build.py compiles that patched copy instead of the original for
exactly those files (see build.py's PATCHED_SOURCES).

Patches are plain unified diffs (`git diff`/`diff -u` format, `a/...`/
`b/...` headers) -- kept that way so every hunk is readable/reviewable on
its own. Applying them does NOT shell out to
`patch`/`git apply` (neither is reliably on PATH from a plain PowerShell
invocation of this driver -- verified empirically; only `git.exe` is, and
`git apply` itself needs a `--directory`/cwd dance to patch-into-a-
different-tree that isn't worth fighting for 4 files). Instead this is a
small, dependency-free unified-diff applier: it locates each hunk's
context+removed lines as a contiguous block anywhere in the current text
(tolerating minor line-number drift the same way real patch tools do)
and fails LOUDLY (raises) if that block isn't found verbatim -- e.g. if
the decomp repo is ever un-pinned from the commit these patches were
authored against -- rather than silently applying to the wrong spot.

Usage:
    python tools/apply_patches.py            # apply all, print a summary
Also importable: apply_all(decomp_root, patches_dir, out_dir) -> list of
(rel_path, out_abs_path) for every patched file, called directly from
tools/build.py before compilation.
"""
import os
import sys

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # .../mp6-native
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
DEFAULT_DECOMP = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos", "marioparty6"))
DEFAULT_PATCHES_DIR = os.path.join(NATIVE_ROOT, "patches", "decomp")
DEFAULT_OUT_DIR = os.path.join(NATIVE_ROOT, "build", "patched-src")


def _parse_hunks(patch_text):
    """Yields (old_lines, new_lines) for each @@ hunk in a unified diff.
    Lines include their trailing '\n' (or none, for a real last line)."""
    hunks = []
    cur_old, cur_new = None, None
    for line in patch_text.splitlines(keepends=True):
        if line.startswith("@@"):
            if cur_old is not None:
                hunks.append((cur_old, cur_new))
            cur_old, cur_new = [], []
        elif line.startswith("--- ") or line.startswith("+++ "):
            continue
        elif cur_old is None:
            continue  # anything before the first @@ (shouldn't happen for our patches)
        elif line.startswith("-"):
            cur_old.append(line[1:])
        elif line.startswith("+"):
            cur_new.append(line[1:])
        elif line.startswith(" "):
            cur_old.append(line[1:])
            cur_new.append(line[1:])
        elif line.startswith("\\"):
            continue  # "\ No newline at end of file" -- not emitted by our generator, but harmless to skip
        else:
            # Blank line in the hunk body (difflib emits a bare '\n' for a
            # context/no-prefix blank line in some edge cases) -- treat as context.
            cur_old.append(line)
            cur_new.append(line)
    if cur_old is not None:
        hunks.append((cur_old, cur_new))
    return hunks


def apply_unified_diff(original_text, patch_text, label=""):
    hunks = _parse_hunks(patch_text)
    text_lines = original_text.splitlines(keepends=True)
    for old_block, new_block in hunks:
        n = len(old_block)
        found = -1
        for i in range(0, len(text_lines) - n + 1):
            if text_lines[i:i + n] == old_block:
                found = i
                break
        if found < 0:
            raise ValueError(
                f"apply_patches: hunk context not found in {label!r} -- the decomp source "
                f"may have drifted from the commit this patch was authored against. "
                f"Expected to find:\n{''.join(old_block)}"
            )
        text_lines = text_lines[:found] + new_block + text_lines[found + n:]
    return "".join(text_lines)


def _write_if_changed(path, content):
    if os.path.exists(path):
        # surrogateescape (not errors="replace") so the up-to-date comparison
        # sees the same lone-surrogate representation apply_all() produces for
        # non-utf-8 source bytes -- with "replace" those bytes decode to
        # U+FFFD, never compare equal, and every run would spuriously rewrite
        # (and mtime-dirty) the file, defeating incremental rebuilds.
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            if f.read() == content:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(content)
    return True


def discover_patches(patches_dir):
    """Returns a sorted list of relative-to-decomp paths (e.g.
    'src/game/decode.c') for every *.patch file under patches_dir."""
    out = []
    for root, _dirs, files in os.walk(patches_dir):
        for f in files:
            if not f.endswith(".patch"):
                continue
            abs_patch = os.path.join(root, f)
            rel = os.path.relpath(abs_patch, patches_dir)
            rel = rel[:-len(".patch")]
            out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def apply_all(decomp_root=DEFAULT_DECOMP, patches_dir=DEFAULT_PATCHES_DIR, out_dir=DEFAULT_OUT_DIR):
    results = []
    for rel in discover_patches(patches_dir):
        src_path = os.path.join(decomp_root, rel.replace("/", os.sep))
        patch_path = os.path.join(patches_dir, rel.replace("/", os.sep) + ".patch")
        out_path = os.path.join(out_dir, rel.replace("/", os.sep))
        # errors="surrogateescape", not strict: the decomp is READ-ONLY and a
        # few of its files carry stray single-byte cp1252 punctuation inside
        # comments (first hit: src/REL/fileseldll/filesel.c line ~2308, a 0x97
        # em-dash) -- strict utf-8 made merely ADDING such a file to the patch
        # set crash the whole applier. surrogateescape round-trips any such
        # byte losslessly (decode -> lone surrogate -> encode restores the
        # exact original byte), so untouched lines stay byte-identical instead
        # of being silently rewritten the way errors="replace" would.
        with open(src_path, "r", encoding="utf-8", errors="surrogateescape") as f:
            original = f.read()
        with open(patch_path, "r", encoding="utf-8", errors="surrogateescape") as f:
            patch_text = f.read()
        patched = apply_unified_diff(original, patch_text, label=rel)
        changed = _write_if_changed(out_path, patched)
        results.append((rel, out_path, changed))
    return results


def main():
    results = apply_all()
    if not results:
        print("[WARN] apply_patches: no *.patch files found under patches/decomp/")
        return 1
    for rel, out_path, changed in results:
        print(f"{'patched' if changed else 'up-to-date'}: {rel} -> {os.path.relpath(out_path, NATIVE_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
