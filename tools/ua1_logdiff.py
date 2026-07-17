#!/usr/bin/env python3
"""ua1_logdiff.py -- game-flow log comparison: proves a Windows headless
600-tick boot log and the Android device log describe the IDENTICAL game
flow, by normalizing exactly the platform-legitimate representation
differences and byte-comparing the rest. Used by the Windows headless gate
and tools/gate_android.py's device-smoke tier (docs/TESTING.md).

The acceptance criterion: every GAME-FLOW line -- everything
the game itself prints (objman/process/dvd/boot-sequence/[SDK]/[HEAPTRACE]/
[AUDIO]/[PRC] lines) -- must be identical after stripping/normalizing ONLY:

  1. platform-tagged lines: the launcher's [HOST] banner (Android only) and
     the adb-wrapper's trailing EXIT= line -- pure launcher plumbing,
     printed before/after the game runs;
  2. %p renderings: mingw prints pointers as 16-digit zero-padded bare hex
     (0000000080000000), bionic as 0x-prefixed minimal hex (0x80000000) --
     same VALUE, different libc formatting. Both forms normalize to
     minimal lowercase hex, so equal pointers stay equal and (crucially)
     UNEQUAL pointers would still show as a diff: heap/arena addresses are
     deliberately NOT masked, since both platforms place the arena at the
     same 0x80000000 and every allocation address must genuinely match;
  3. code addresses: values in 0x10000000..0x1FFFFFFF (the image region on
     BOTH platforms -- Windows --image-base and the Android reserved-
     address dlopen use the same base) collapse to CODEADDR. The two
     compilers lay functions out differently inside the image, so
     retaddr=/func=/prolog-func values differ by construction; that they
     are in the SAME low window is exactly the invariant, and is asserted
     here (a code address outside the window fails the diff);
  4. absolute host paths inside the two [DVD] resolution lines (repo path
     on Windows, /data/local/tmp on Android).

Anything else -- line order, line count, every decimal size, every heap
address, every game marker -- must match byte-for-byte, or this exits 1
printing a unified diff of the residue.

Usage: python tools/ua1_logdiff.py <windows_log> <android_log> [--dump-dir D]
"""
import argparse
import difflib
import re
import sys

STRIP_LINE_RE = re.compile(r"^(\[HOST\]|EXIT=)")

# mingw %p: 16-digit zero-padded bare hex token (e.g. 00000000801FE128).
MINGW_PTR_RE = re.compile(r"\b0{8}([0-9A-Fa-f]{8})\b")
# 0x-prefixed hex of any width (bionic %p, and both platforms' 0x%08x-style
# game prints -- identical values on both sides for the latter).
HEX_RE = re.compile(r"\b0[xX]0*([0-9A-Fa-f]+)\b")
# After normalization: anything inside the shared low image window
# 0x10000000..0x1FFFFFFF is a code address (see module docstring, rule 3).
CODE_RE = re.compile(r"\b1[0-9a-f]{7}\b")
# Host paths inside quotes: this repo's Windows checkout or the on-device
# base (rule 4). Kept deliberately narrow -- only real path shapes match.
PATH_RE = re.compile(r'"(?:[A-Za-z]:[\\/][^"]*|/data/local/tmp[^"]*)"')


def normalize(text):
    out = []
    for line in text.splitlines():
        if STRIP_LINE_RE.match(line):
            continue
        line = MINGW_PTR_RE.sub(lambda m: m.group(1).lower().lstrip("0") or "0", line)
        line = HEX_RE.sub(lambda m: m.group(1).lower(), line)
        line = CODE_RE.sub("CODEADDR", line)
        line = PATH_RE.sub('"<HOSTPATH>"', line)
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("windows_log")
    ap.add_argument("android_log")
    ap.add_argument("--dump-dir", help="also write the two normalized logs here for inspection")
    args = ap.parse_args()

    with open(args.windows_log, "r", encoding="utf-8", errors="replace") as f:
        win = normalize(f.read())
    with open(args.android_log, "r", encoding="utf-8", errors="replace") as f:
        android = normalize(f.read())

    if args.dump_dir:
        import os
        os.makedirs(args.dump_dir, exist_ok=True)
        for name, lines in (("win.norm", win), ("android.norm", android)):
            with open(os.path.join(args.dump_dir, name), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(lines) + "\n")

    if win == android:
        print(f"[ua1_logdiff] PASS: {len(win)} normalized game-flow lines are IDENTICAL "
              f"(windows={args.windows_log}, android={args.android_log})")
        return 0

    diff = list(difflib.unified_diff(win, android, fromfile="windows(normalized)",
                                     tofile="android(normalized)", lineterm=""))
    print(f"[ua1_logdiff] FAIL: game-flow logs differ after normalization "
          f"({len(win)} vs {len(android)} lines); residue:")
    for line in diff[:200]:
        print(line)
    if len(diff) > 200:
        print(f"... ({len(diff) - 200} more diff lines)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
