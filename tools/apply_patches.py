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
`b/...` headers, though the header text itself is never parsed -- only the
`@@` hunk lines are) -- kept that way so every hunk is readable/reviewable
on its own. As of the public-repo source-exposure cleanup, patches/decomp/
is authored at ZERO context (`diff -U0`): each hunk carries only the
changed (`-`/`+`) lines, none of the surrounding decomp source, to keep as
little of the decompiled game's own code out of the public repo as
possible. Applying them does NOT shell out to `patch`/`git apply` (neither
is reliably on PATH from a plain PowerShell invocation of this driver --
verified empirically; only `git.exe` is, and `git apply` itself needs a
`--directory`/cwd dance to patch-into-a-different-tree that isn't worth
fighting for two dozen files). Instead this is a small, dependency-free
unified-diff applier -- but with zero-context hunks, the OLD design (find
each hunk's context+removed block as a contiguous run of text anywhere in
the file) breaks: a pure-addition hunk has an EMPTY old block, which
matches vacuously at index 0 and inserts in the wrong place. So this
applier instead trusts the `@@ -oldStart,oldCount +newStart,newCount @@`
line numbers in each hunk header directly -- correct because the decomp
this project patches is PINNED to an exact commit (docs/
DECOMP_DEPENDENCY.md), so a patch's line numbers are exact against it, not
approximate. Hunks are applied bottom-to-top (descending oldStart) so an
earlier (lower-numbered) edit's line-count change never shifts a later
(higher-numbered) hunk's own coordinates. The old design's loud-failure
drift guarantee is kept, just keyed by position instead of free substring
search: before replacing the original lines starting at oldStart with the
hunk's added lines, this applier VERIFIES the old-file lines at that
position equal the hunk's own removed/context lines, and raises
immediately if they don't -- e.g. if the decomp repo is ever un-pinned
from the commit a patch was authored against -- rather than silently
applying to the wrong spot. Only oldStart itself is trusted from the
header, though -- the actual span length compared/replaced is
len(old_lines), the hunk BODY's own removed/context line count, not the
header's own "oldCount" number (oldCount==0 is still how a pure-insertion
hunk, with no removed/context lines at all, is recognized, since that's
exactly when len(old_lines) is 0 too). A fresh diff always has these
equal by construction, so this changes nothing for patches/decomp's own
-U0 output or any future patch a real diff tool produces -- but hand-
edited patches let the header's own oldCount/newCount go stale over time
without anyone noticing (verified empirically while regenerating
patches/decomp at -U0: 7 of the 23 pre-existing hunks, and separately 4 of
platform/gx/aurora-patches/*.patch's, had a header oldCount that no
longer matched their own body's actual line count -- both invisible under
the OLD context-search engine, which never read the @@ numbers at all).
Trusting the body's own count here tracks that same tolerance instead of
newly breaking on it, while oldStart plus the content-equality check
still fully verify the decomp source hasn't ACTUALLY drifted at that
position (a genuine mismatch still fails just as loudly). A hunk with
real context lines applies exactly the same way (context lines simply
appear on both the removed-side check and the replacement, unchanged) --
so this applies patches/decomp's own zero-context hunks and any higher-
context patch (e.g. platform/gx/aurora-patches's, applied via this same
module's apply_unified_diff() from setup/lib/step_aurora.py) equally
correctly.

Usage:
    python tools/apply_patches.py            # apply all, print a summary
Also importable: apply_all(decomp_root, patches_dir, out_dir) -> list of
(rel_path, out_abs_path, changed) for every patched file, called directly
from tools/build.py before compilation.
"""
import os
import re
import subprocess
import sys

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # .../mp6-native
PORT_ROOT = os.path.dirname(NATIVE_ROOT)


def _default_decomp():
    """The decomp checkout to patch FROM, honouring MP6_DECOMP_DIR.

    tools/build.py always passes decomp_root= explicitly (it resolves the
    same override through setup/lib/common.py's DECOMP_DIR), but this module
    is also run standalone (`python tools/apply_patches.py`) and imported by
    tests -- those used to hard-code the sibling `marioparty6` checkout and
    would read a DIFFERENT tree than the build, silently patching (or, when
    that tree lacks a patched file such as src/REL/w01Dll/world01.c,
    crashing with FileNotFoundError). Resolution mirrors
    setup/lib/common.py::_workspace_path_override exactly -- relative values
    are rooted at NATIVE_ROOT, never at the process cwd -- so both entry
    points can never select different trees.
    """
    repos = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos"))
    canonical = os.path.join(repos, "marioparty6")
    value = os.environ.get("MP6_DECOMP_DIR", "").strip()
    if not value:
        # Match setup/lib/common.py exactly: standalone patch application and
        # tools/build.py must consume the same canonical checkout.
        value = canonical
    elif not os.path.isabs(value):
        value = os.path.join(NATIVE_ROOT, value)
    return os.path.normpath(os.path.abspath(os.path.expanduser(value)))


DEFAULT_DECOMP = _default_decomp()


def require_pinned_decomp(root=None):
    """Fail loudly when the resolved decomp tree is not at the documented pin.

    Decomp-reading tests call this before asserting on tree CONTENT, so a
    stale or wrong checkout produces one clear error instead of a page of
    baffling content-assertion failures. Returns the verified root.
    """
    root = os.path.normpath(root or DEFAULT_DECOMP)
    dep_doc = os.path.join(NATIVE_ROOT, "docs", "DECOMP_DEPENDENCY.md")
    with open(dep_doc, "r", encoding="utf-8") as fh:
        pin = next((ln.strip() for ln in fh if re.fullmatch(r"[0-9a-f]{40}", ln.strip())), None)
    if pin is None:
        raise RuntimeError(f"no 40-hex pin SHA found in {dep_doc}")
    try:
        head = subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(
            f"decomp tree at {root} is not a readable git checkout ({error}) -- set MP6_DECOMP_DIR to the pinned worktree"
        ) from error
    if head != pin:
        raise RuntimeError(
            f"decomp tree at {root} is stale/mismatched (HEAD {head[:12]}, "
            f"pin {pin[:12]}) -- set MP6_DECOMP_DIR to the clean pinned checkout"
        )
    status = subprocess.run(
        ["git", "-C", root, "status", "--porcelain=v1", "--untracked-files=all"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if status.returncode != 0:
        raise RuntimeError(f"could not inspect decomp tree cleanliness at {root}")
    if status.stdout.strip():
        raise RuntimeError(
            f"decomp tree at {root} has modified/untracked build inputs -- "
            "standalone patch application requires the clean pinned checkout"
        )
    return root
DEFAULT_PATCHES_DIR = os.path.join(NATIVE_ROOT, "patches", "decomp")
DEFAULT_PATCH_FRAGMENTS_DIR = os.path.join(NATIVE_ROOT, "patches", "decomp-fragments")
DEFAULT_OUT_DIR = os.path.join(NATIVE_ROOT, "build", "patched-src")

_HUNK_HEADER_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def _parse_hunks(patch_text):
    """Yields one dict per `@@` hunk in a unified diff:
        {"old_start", "old_count", "new_start", "new_count"} straight from
        the hunk header (old_count/new_count default to 1 when the header
        omits them, per the unified-diff format), plus "old_lines"/
        "new_lines" -- the hunk body's removed-or-context / added-or-
        context lines respectively, each including its trailing '\\n' (or
        none, for a real last line). Lines are collected by prefix
        (' ' -> both, '-' -> old only, '+' -> new only), NOT by counting
        against the header -- apply_unified_diff() cross-checks the two
        agree before trusting either."""
    hunks = []
    cur = None
    last_touched = None  # which side(s) the most recent body line went to --
    # needed so a following "\ No newline at end of file" marker (real
    # unified-diff hunks emit one per side, immediately after that side's
    # true last line -- e.g. patches/decomp/src/game/frand.c.patch's
    # hunk ends ON the file's actual last line, which has no trailing
    # newline) strips the trailing '\n' splitlines() attached from the
    # PATCH TEXT (irrelevant) off the correct collected line(s), instead
    # of leaving a phantom '\n' that would make the byte-for-byte
    # old-block verification below fail against the real (newline-less)
    # source line.
    for line in patch_text.splitlines(keepends=True):
        if line.startswith("@@"):
            m = _HUNK_HEADER_RE.match(line)
            if not m:
                raise ValueError(f"apply_patches: unparseable hunk header: {line!r}")
            cur = {
                "old_start": int(m.group(1)),
                "old_count": int(m.group(2)) if m.group(2) is not None else 1,
                "new_start": int(m.group(3)),
                "new_count": int(m.group(4)) if m.group(4) is not None else 1,
                "old_lines": [],
                "new_lines": [],
            }
            hunks.append(cur)
            last_touched = None
        elif line.startswith("--- ") or line.startswith("+++ "):
            continue
        elif cur is None:
            continue  # anything before the first @@ (shouldn't happen for our patches)
        elif line.startswith("-"):
            cur["old_lines"].append(line[1:])
            last_touched = "old"
        elif line.startswith("+"):
            cur["new_lines"].append(line[1:])
            last_touched = "new"
        elif line.startswith(" "):
            cur["old_lines"].append(line[1:])
            cur["new_lines"].append(line[1:])
            last_touched = "both"
        elif line.startswith("\\"):
            # "\ No newline at end of file" -- strip the trailing '\n'
            # off whichever side's last line this marker follows (see
            # last_touched's comment above). Not emitted by our own
            # generator's ordinary case (most hunks don't end ON the
            # file's true last line), but frand.c.patch's does, so this
            # must be handled, not just skipped.
            if last_touched in ("old", "both") and cur["old_lines"] and cur["old_lines"][-1].endswith("\n"):
                cur["old_lines"][-1] = cur["old_lines"][-1][:-1]
            if last_touched in ("new", "both") and cur["new_lines"] and cur["new_lines"][-1].endswith("\n"):
                cur["new_lines"][-1] = cur["new_lines"][-1][:-1]
            continue
        else:
            # Blank line in the hunk body (difflib emits a bare '\n' for a
            # context/no-prefix blank line in some edge cases) -- treat as context.
            cur["old_lines"].append(line)
            cur["new_lines"].append(line)
            last_touched = "both"
    return hunks


def apply_unified_diff(original_text, patch_text, label=""):
    hunks = _parse_hunks(patch_text)
    # Bottom-to-top: apply the hunk with the highest oldStart first, so
    # splicing it in never shifts the line numbers a lower (not-yet-
    # applied) hunk still needs to trust.
    hunks.sort(key=lambda h: h["old_start"], reverse=True)
    text_lines = original_text.splitlines(keepends=True)
    for h in hunks:
        old_start = h["old_start"]
        old_lines, new_lines = h["old_lines"], h["new_lines"]
        # The old-side SPAN used below is len(old_lines) -- the hunk
        # body's own actual removed/context line count -- not the
        # header's "oldCount" number. In a freshly-generated diff
        # (patches/decomp's own -U0 output, or any future patch a real
        # diff tool produces) these are always equal by construction, so
        # this changes nothing for that case. But both old_count AND
        # new_count are, in practice, pure documentation that hand-edited
        # patches let go stale (verified empirically: 7 of the pre-U0
        # patches/decomp/*.patch hunks and 4 of
        # platform/gx/aurora-patches/*.patch's had a header old_count
        # that didn't match their own body's actual line count, both
        # invisible under the OLD context-search engine because it never
        # read @@ numbers at all) -- trusting the body's own count here
        # instead of the header's tracks that same old engine's
        # tolerance for this specific kind of staleness, while the
        # content-equality check right below still fully verifies the
        # decomp source hasn't ACTUALLY drifted at that position (a
        # genuine mismatch fails just as loudly either way).
        old_count = len(old_lines)
        if old_count == 0:
            # Pure insertion: unified-diff convention is "insert after
            # original line oldStart" -- i.e. at 0-indexed position
            # oldStart (oldStart==0 means "before the first line"). No
            # removed/context lines exist to verify.
            pos = old_start
            text_lines = text_lines[:pos] + new_lines + text_lines[pos:]
        else:
            begin = old_start - 1  # 1-indexed hunk header -> 0-indexed list
            end = begin + old_count
            actual = text_lines[begin:end]
            if actual != old_lines:
                raise ValueError(
                    f"apply_patches: hunk context/removed-lines mismatch in {label!r} "
                    f"at original line {old_start} -- the decomp source may have drifted "
                    f"from the commit this patch was authored against (docs/"
                    f"DECOMP_DEPENDENCY.md pins it). Expected:\n{''.join(old_lines)}\n"
                    f"Found:\n{''.join(actual)}"
                )
            text_lines = text_lines[:begin] + new_lines + text_lines[end:]
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


def discover_patch_fragments(fragments_dir):
    """Returns {decomp-relative path: [ordered fragment patch paths]}.

    Fragment names mirror the target and end in ``.patch.<order>``.  They
    apply after the ordinary patch and are authored against that patched
    output.  This lets a small UTF-8 review fix extend a legacy patch that
    itself losslessly carries a stray non-UTF-8 source byte, without rewriting
    the whole historical patch merely to change its encoding.
    """
    out = {}
    if not fragments_dir or not os.path.isdir(fragments_dir):
        return out
    for root, _dirs, files in os.walk(fragments_dir):
        for filename in files:
            marker = ".patch."
            if marker not in filename:
                continue
            absolute = os.path.join(root, filename)
            relative = os.path.relpath(absolute, fragments_dir).replace(os.sep, "/")
            target, _order = relative.rsplit(marker, 1)
            out.setdefault(target, []).append(absolute)
    for paths in out.values():
        paths.sort()
    return out


def apply_all(decomp_root=DEFAULT_DECOMP, patches_dir=DEFAULT_PATCHES_DIR, out_dir=DEFAULT_OUT_DIR,
              fragments_dir=None):
    if fragments_dir is None and os.path.normcase(os.path.abspath(patches_dir)) == os.path.normcase(
            os.path.abspath(DEFAULT_PATCHES_DIR)):
        fragments_dir = DEFAULT_PATCH_FRAGMENTS_DIR
    fragments = discover_patch_fragments(fragments_dir)
    results = []
    for rel in sorted(set(discover_patches(patches_dir)) | set(fragments)):
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
        patched = original
        if os.path.isfile(patch_path):
            with open(patch_path, "r", encoding="utf-8", errors="surrogateescape") as f:
                patch_text = f.read()
            patched = apply_unified_diff(patched, patch_text, label=rel)
        for fragment_path in fragments.get(rel, ()):
            with open(fragment_path, "r", encoding="utf-8") as f:
                fragment_text = f.read()
            patched = apply_unified_diff(
                patched,
                fragment_text,
                label=f"{rel} fragment {os.path.basename(fragment_path)}",
            )
        changed = _write_if_changed(out_path, patched)
        results.append((rel, out_path, changed))
    return results


def main():
    try:
        decomp_root = require_pinned_decomp()
    except RuntimeError as error:
        print(f"[FATAL] apply_patches: {error}", file=sys.stderr)
        return 1
    results = apply_all(decomp_root=decomp_root)
    if not results:
        print("[WARN] apply_patches: no *.patch files found under patches/decomp/")
        return 1
    for rel, out_path, changed in results:
        print(f"{'patched' if changed else 'up-to-date'}: {rel} -> {os.path.relpath(out_path, NATIVE_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
