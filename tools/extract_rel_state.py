"""Move REL state-reset blocks out of compat/decomp patches into src/rel_state.

A native overlay stays linked between visits, so each recovered module ends
its source files with an MP6_REL_RESET_DEFINE block (include/mp6_rel_state.h)
listing the file's mutable data and BSS. Those blocks are port code; keeping
them in the decomp patches made them most of the patch volume. This moves
each block verbatim to src/rel_state/<decomp path>.inc, which the build
appends to the generated source (tools/apply_patches.py), and rewrites
the patch without it (or deletes it when nothing else remains). Data/BSS
list macros that a patch defines higher up in the file move with it.

    python tools/extract_rel_state.py [--skip m609dll,...] [--check]

--check reports blocks still inside patches without changing anything.
Run tools/apply_patches.py first. tools/gen_rel_state.py checks the lists.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

MARKER = '#include "mp6_rel_state.h"'
LIST_MACRO = re.compile(r'^#define (MP6_\w+_(?:BSS|DATA))\(X\)(?:[^\n]*\\\n)*[^\n]*\n', re.M)


def split_lists(before):
    """Remove the list macros from the patched source when nothing but the
    reset block uses them. Returns (source, [macro text, ...])."""
    found = list(LIST_MACRO.finditer(before))
    if not found:
        return before, []
    rest = LIST_MACRO.sub('', before)
    if any(re.search(r'\b' + m.group(1) + r'\b', rest) for m in found):
        return before, []
    return rest, [m.group(0) for m in found]


def rewrite_patch(rel, patch, before):
    original = (Path(build.DECOMP) / rel).read_bytes().decode('utf-8').replace('\r\n', '\n')
    if before.rstrip('\n') == original.rstrip('\n'):
        patch.unlink()
        return
    tmp_before = ROOT / 'build' / 'rel_state_before.c'
    tmp_after = ROOT / 'build' / 'rel_state_after.c'
    tmp_before.write_text(original, encoding='utf-8', newline='\n')
    tmp_after.write_text(before, encoding='utf-8', newline='\n')
    subprocess.run([sys.executable, str(ROOT.parent / 'generate_zero_patch.py'), str(tmp_before),
                    str(tmp_after), rel, str(patch)], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)
    tmp_before.unlink()
    tmp_after.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip', default='', help='comma-separated module directories to leave alone')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    skip = {s for s in args.skip.split(',') if s}
    patch_root = ROOT / 'compat' / 'decomp'
    moved = 0
    for patch in sorted(patch_root.rglob('*.c.patch')):
        rel = patch.relative_to(patch_root).as_posix()[:-len('.patch')]
        if not rel.startswith('src/REL/') or rel.split('/')[2] in skip:
            continue
        inc = ROOT / 'src' / 'rel_state' / (rel[len('src/'):] + '.inc')
        patched_path = Path(build.PATCHED_SRC_DIR) / rel
        if not patched_path.exists():
            continue
        patched = patched_path.read_text(encoding='utf-8')
        at = patched.find(MARKER)
        if at < 0:
            continue
        # Generated copies already end with a moved block (apply_all appends it).
        block = inc.read_text(encoding='utf-8') if inc.exists() else patched[at:]
        before, lists = split_lists(patched[:at].rstrip('\n') + '\n')
        if inc.exists() and not lists:
            continue
        if args.check:
            print('still in patch:', rel)
            continue
        if lists:
            head, _, tail = block.partition(MARKER + '\n')
            block = head + MARKER + '\n' + ''.join(lists) + tail
        inc.parent.mkdir(parents=True, exist_ok=True)
        inc.write_text(block, encoding='utf-8', newline='\n')
        rewrite_patch(rel, patch, before)
        moved += 1
    print(f'{"found" if args.check else "moved"} {moved} block(s)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
