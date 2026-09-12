"""Read-only inventory of decompiled player-motion requests plus port patches.

Check stock one-shot slots at the call sites, never impose a global loop filter.
Dynamic/event-specific motion IDs remain visible as unresolved entries; their
meaning cannot be inferred from a numeric slot allocated at runtime.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import apply_patches, build

# Player table is 1-based (object slot 0 is the model's embedded motion).
# Stun (6), movement (1/2/3/14), and sustained damage (15) may deliberately loop.
ONE_SHOTS = {4, 5, 7, 8, 9, 10, 11, 12, 13}
APIS = {'mbPlayerMotionSet': (3, 1, 2),
        'mbPlayerMotionShiftSet': (5, 1, 4),
        'mbev_CapPlayerMotShiftWait': (4, 1, 2),
        'mbev_CapPlayerMotionSet': (8, 2, 4)}
PATTERN = re.compile(r'\b(' + '|'.join(APIS) + r')\s*\(')
CONSTANTS = {'FALSE': 0, 'TRUE': 1, 'HU3D_MOTATTR_NONE': 0,
             'HU3D_ATTR_NONE': 0, 'HU3D_MOTATTR_LOOP': 0x40000001,
             'HU3D_MOTATTR_PAUSE': 0x40000002, 'HU3D_MOTATTR_REV': 0x40000004,
             'HU3D_MOTATTR_SHIFT_LOOP': 0x40000008}


def calls(source):
    # Preserve positions/newlines while removing comments and string contents.
    clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                   lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)
    constants = dict(CONSTANTS)
    for name, value in re.findall(r'\b(\w+)\s*=\s*(0x[\da-fA-F]+|\d+)\s*[,}]', clean):
        constants[name] = int(value, 0)

    def number(s):
        s = s.strip()
        if s in constants:
            return constants[s]
        if re.fullmatch(r'0x[\da-fA-F]+|\d+', s):
            return int(s, 0)
        return None

    for m in PATTERN.finditer(clean):
        depth, start, args = 1, m.end(), []
        for i in range(start, len(clean)):
            c = clean[i]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            if not depth or (c == ',' and depth == 1):
                args.append(clean[start:i].strip())
                start = i + 1
            if not depth:
                break
        arity, slot_index, attr_index = APIS[m[1]]
        if len(args) != arity or re.match(r'(?:int|s16|u32)\s', args[0]):
            continue  # Function declaration, not a request.
        slot, attr = number(args[slot_index]), number(args[attr_index])
        yield {'line': clean.count('\n', 0, m.start()) + 1, 'api': m[1],
               'slot': slot, 'attr': attr,
               'slot_expression': args[slot_index], 'attr_expression': args[attr_index],
               'bad_one_shot_loop': slot in ONE_SHOTS and attr is not None and bool(attr & 9)}


def inventory(patched=True):
    records = []
    source_root = Path(build.DECOMP)
    for path in sorted((source_root / 'src').rglob('*.c')):
        source = path.read_text(encoding='utf-8')
        relative = path.relative_to(source_root)
        patch = ROOT / 'compat/decomp' / (relative.as_posix() + '.patch')
        if patched and patch.exists():
            source = apply_patches.apply_unified_diff(source, patch.read_text(encoding='utf-8'))
        for call in calls(source):
            records.append({'file': relative.as_posix(), **call})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unpatched', action='store_true', help='Negative-control inventory')
    parser.add_argument('--unresolved', action='store_true', help='List runtime IDs/attributes for manual review')
    args = parser.parse_args()
    records = inventory(not args.unpatched)
    if args.unresolved:
        print(json.dumps([r for r in records if r['slot'] is None or r['attr'] is None], indent=2))
        return 0
    violations = [r for r in records if r['bad_one_shot_loop']]
    print(json.dumps({'requests': len(records),
                      'known_stock_one_shots': sum(r['slot'] in ONE_SHOTS for r in records),
                      'unresolved_requests': sum(r['slot'] is None or r['attr'] is None for r in records),
                      'stock_slots': dict(sorted(Counter(str(r['slot']) for r in records).items())),
                      'violations': violations}, indent=2))
    return bool(violations)


if __name__ == '__main__':
    raise SystemExit(main())
