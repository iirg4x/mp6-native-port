#!/usr/bin/env python3
"""Apply the Port's per-file adaptations to a local source staging tree.

The pinned decomp checkout is read-only. compat/decomp/ mirrors its source
paths with one zero-context unified diff per changed file. Removed lines and
hunk counts are verified; source drift is an error, never a fuzzy match.
Non-UTF8 source comments round-trip losslessly using surrogateescape.

Run python tools/apply_patches.py, or import apply_all() from the build.
Generated files go to build/patched-src/ and are never authoritative inputs.
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
    canonical = os.path.join(NATIVE_ROOT, "build", "deps", "marioparty6")
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
DEFAULT_PATCHES_DIR = os.path.join(NATIVE_ROOT, "compat", "decomp")
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
    # true last line -- e.g. compat/decomp/src/game/frand.c.patch's
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


REL_STATE_DIR = os.path.join(NATIVE_ROOT, "src", "rel_state")


def discover_rel_states(rel_state_dir=REL_STATE_DIR):
    """Decomp-relative paths ('src/REL/...c') that have a state-reset block
    in src/rel_state (<path>.inc)."""
    out = []
    for root, _dirs, files in os.walk(rel_state_dir):
        for f in files:
            if f.endswith(".c.inc"):
                rel = os.path.relpath(os.path.join(root, f), rel_state_dir)[:-len(".inc")]
                out.append("src/" + rel.replace(os.sep, "/"))
    return sorted(out)


def with_rel_state(source_text, relative):
    """Append a recovered REL source's state-reset block (src/rel_state/
    <path>.inc), as apply_all does for the generated sources the build compiles. Sources
    without one are returned unchanged."""
    relative = relative.replace("\\", "/")
    if not relative.startswith("src/REL/"):
        return source_text
    inc = os.path.join(NATIVE_ROOT, "src", "rel_state", *relative[len("src/"):].split("/")) + ".inc"
    if not os.path.exists(inc):
        return source_text
    with open(inc, encoding="utf-8") as f:
        return source_text.rstrip("\n") + "\n\n" + f.read()


def generated_source(decomp_root, relative):
    """The text apply_all generates for one decomp source: the pinned file,
    its compat/decomp patch when one exists, then its rel_state block."""
    relative = relative.replace("\\", "/")
    with open(os.path.join(decomp_root, *relative.split("/")), encoding="utf-8") as f:
        text = f.read()
    patch = os.path.join(NATIVE_ROOT, "compat", "decomp", *relative.split("/")) + ".patch"
    if os.path.exists(patch):
        with open(patch, encoding="utf-8") as f:
            text = apply_unified_diff(text, f.read(), label=relative)
    return with_rel_state(text, relative)


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
        # (compat/decomp's own -U0 output, or any future patch a real
        # diff tool produces) these are always equal by construction, so
        # this changes nothing for that case. But both old_count AND
        # new_count are, in practice, pure documentation that hand-edited
        # patches let go stale (verified empirically: 7 of the pre-U0
        # compat/decomp/*.patch hunks and 4 of
        # compat/aurora/base/*.patch's had a header old_count
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


def apply_all(decomp_root=DEFAULT_DECOMP, patches_dir=DEFAULT_PATCHES_DIR, out_dir=DEFAULT_OUT_DIR,
              rel_state_dir=None):
    results = []
    # Recovered REL sources also get their state-reset block appended (port
    # code kept in src/rel_state, not in the decomp patches), so they have a
    # generated copy even when no patch remains for them. A caller-supplied
    # patch tree (tests) brings no rel_state blocks unless it names a directory.
    if rel_state_dir is None and os.path.realpath(patches_dir) == os.path.realpath(DEFAULT_PATCHES_DIR):
        rel_state_dir = REL_STATE_DIR
    rel_states = discover_rel_states(rel_state_dir) if rel_state_dir else []
    active = sorted(set(discover_patches(patches_dir)) | set(rel_states))
    # Unlike source TUs, headers are chosen by include search order. A removed
    # patch must not leave an obsolete shadow silently ahead of upstream.
    header_root = os.path.realpath(os.path.join(out_dir, "include"))
    if header_root == os.path.realpath(os.path.join(decomp_root, "include")):
        raise ValueError("generated header output must not be the decomp input")
    active_headers = {rel for rel in active if rel.startswith("include/")}
    for directory, _dirs, files in os.walk(header_root, followlinks=False):
        for name in files:
            candidate = os.path.join(directory, name)
            if os.path.commonpath([header_root, os.path.realpath(candidate)]) != header_root:
                raise ValueError("generated header escapes the output tree: " + candidate)
            rel = "include/" + os.path.relpath(candidate, header_root).replace(os.sep, "/")
            if name.endswith(".h") and rel not in active_headers:
                os.remove(candidate)
    for rel in active:
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
        patched = with_rel_state(patched, rel)
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
        print("[WARN] apply_patches: no *.patch files found under compat/decomp/")
        return 1
    for rel, out_path, changed in results:
        print(f"{'patched' if changed else 'up-to-date'}: {rel} -> {os.path.relpath(out_path, NATIVE_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
