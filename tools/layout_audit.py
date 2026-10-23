"""Find structs that only line up with 4-byte pointers.

Recovered code often views one object through two struct definitions: the
owner's real one, and another file's partial copy that skips fields it
does not use with an opaque byte pad (`u8 unk[64]`). On the GameCube both
copies agree on every offset. Natively, pointers are 8 bytes, so a pad that
stood in for pointer fields is now too short and every field after it reads
the wrong bytes (the battle-results coin sparkles wrote over the particle
renderer's display list this way).

The same goes for code that reaches a field by its GameCube byte offset,
`*(s32 *)((u8 *)work + 4812)`: natively that offset lands on a different
field once pointers before it have grown. The audit reads every constant
byte offset applied to a struct pointer from clang's AST and reports the
ones whose GameCube field has moved.

This compiles every recovered source for 32-bit Windows, which lays structs
out like the GameCube (4-byte pointers, BOOL is int, doubles 8-aligned),
and for both native layouts: Windows x64, and Android (with the Android
build's own compiler, flags and headers; BOOL is a 1-byte bool there, as on
the web). It reads clang's record layouts. The Android pass needs the NDK
and a prior Android build tree; without them it is skipped with a note. Two structs are views of the same object when
they share a leading run of fields (same names and types at the same 32-bit
offsets). For each such pair, every later field that starts at the same
32-bit offset in both must also start at the same native offset; one that
does not is reported, with the native layout it fails on.

    python tools/layout_audit.py [-j N] [--json OUT]

Exits 1 when it finds a mismatch. Run tools/apply_patches.py first.
"""
import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

TARGET_32 = 'x86-windows-gnu'  # GameCube-like: 4-byte pointers, int BOOL, 8-byte double alignment
# Windows x64, and Android's layout (BOOL is bool there; dolphin/types.h).
ANDROID = 'android'
NATIVE_TARGETS = (build.TARGET, ANDROID)
MIN_SHARED = 3  # leading fields two definitions must share to count as views of one object

HEADER = re.compile(r'^\s*0 \| (struct|union|class) (.+?)\s*$')
FIELD = re.compile(r'^\s*(\d+)(?::\d+-\d+)? \|( +)(.+?)\s*$')
SIZE = re.compile(r'^\s*\| \[sizeof=(\d+)')


def android_compiler():
    """NDK clang and the Android row's flags, or None when unavailable."""
    root = build._find_ndk_root()
    clang = build._ndk_clang(root) if root else None
    if not clang or not os.path.exists(clang) or not os.path.isdir(build.ANDROID_PATCHED_INCLUDE):
        return None
    return clang, build.android_common_flags()


def parse_layouts(text):
    """{record name: (size, [(name, type, offset), ...] top-level fields)}"""
    records = {}
    for block in text.split('*** Dumping AST Record Layout')[1:]:
        lines = block.strip('\n').splitlines()
        head = HEADER.match(lines[0]) if lines else None
        if not head:
            continue
        fields, size = [], None
        for line in lines[1:]:
            m = SIZE.match(line)
            if m:
                size = int(m.group(1))
                break
            m = FIELD.match(line)
            if not m or len(m.group(2)) != 3:  # nested members are indented further
                continue
            decl = m.group(3)
            # An anonymous member prints as its type alone, "union (anonymous at file:1:2)".
            name = decl.rsplit(' ', 1)[-1] if ' ' in decl and not decl.endswith(')') else ''
            fields.append((name, decl[:len(decl) - len(name)].strip(), int(m.group(1))))
        if size is not None:
            records[f'{head.group(1)} {head.group(2)}'] = (size, fields)
    return records


def dump(rel, flags, target, android=None):
    if target == ANDROID:
        compiler = [android[0], *android[1]]
    else:
        common = list(build.COMMON_FLAGS)
        common[common.index('-target') + 1] = target
        compiler = [build.ZIG, 'cc', *common]
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
        cmd = [*compiler, *flags, '-fsyntax-only', '-Xclang',
               '-fdump-record-layouts-complete', build.resolve_source(rel)]
        cmd = build.transport_long_defines(cmd, str(Path(temporary) / 'unit.o'))
        out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    # zig cc exits non-zero after -fsyntax-only (no object written); judge by output.
    errors = [line for line in out.stderr.splitlines() if ': error:' in line and 'FileNotFound' not in line]
    return parse_layouts(out.stdout), errors


BYTE_CAST = re.compile(r'\((?:const )?(?:u8|s8|char|unsigned char|signed char) ?\*\)')
BYTE_TYPES = {'u8 *', 's8 *', 'char *', 'unsigned char *', 'signed char *', 'const u8 *', 'const s8 *',
              'const char *'}


def _strip(node):
    while node.get('kind') in ('ParenExpr', 'ImplicitCastExpr') and node.get('inner'):
        node = node['inner'][0]
    return node


_TYPEDEFS = threading.local()  # typedef name -> 'struct X' for the TU being read


def _typedefs(ast):
    out = {}
    for node in ast.get('inner', ()):
        if node.get('kind') == 'TypedefDecl':
            t = node.get('type', {})
            q = t.get('desugaredQualType', t.get('qualType', ''))
            if q.startswith(('struct ', 'union ')):
                out[node['name']] = q
    return out


def _qual(t):
    """A type with struct typedefs resolved: 'PositionModel *' -> 'struct PositionModel *'."""
    q = t.get('desugaredQualType', t.get('qualType', ''))
    base = q.rstrip(' *')
    stars = q[len(base):]
    names = getattr(_TYPEDEFS, 'map', {})
    if not base.startswith(('struct ', 'union ')) and base in names:
        return names[base] + stars
    return q


def _pointee(node):
    t = node.get('type', {})
    q = _qual(t)
    return q[:-2] if q.endswith(' *') and q.startswith(('struct ', 'union ')) else None


def _assigned_records(ast):
    """{decl id: 'struct X'} for untyped (void or byte) pointers, from how
    they are used: `void *work = boardWork;` or `Board *b = work;`. A
    pointer copied from another untyped one takes that one's type; the
    type used most often wins."""
    votes = defaultdict(lambda: defaultdict(int))
    aliases = []

    def ref(node):
        node = _strip(node)
        if node.get('kind') == 'CStyleCastExpr' and node.get('inner'):
            node = _strip(node['inner'][0])
        return node['referencedDecl']['id'] if node.get('kind') == 'DeclRefExpr' else None

    def pair(target_id, target_record, source):
        source_id = ref(source)
        source_record = _pointee(_strip(source))
        if target_id and source_record:
            votes[target_id][source_record] += 1
        if source_id and target_record:
            votes[source_id][target_record] += 1
        if target_id and source_id and not target_record and not source_record:
            aliases.append((target_id, source_id))

    def visit(value):
        if isinstance(value, list):
            for child in value:
                visit(child)
            return
        if not isinstance(value, dict):
            return
        kind = value.get('kind')
        if kind == 'VarDecl' and value.get('inner') and value.get('init'):
            t = value.get('type', {})
            q = _qual(t)
            record = q[:-2] if q.endswith(' *') and q.startswith(('struct ', 'union ')) else None
            pair(value['id'], record, value['inner'][0])
        elif kind == 'BinaryOperator' and value.get('opcode') == '=' and len(value.get('inner', ())) == 2:
            lhs = _strip(value['inner'][0])
            pair(ref(lhs), _pointee(lhs), value['inner'][1])
        for child in value.values():
            if isinstance(child, (dict, list)):
                visit(child)

    visit(ast)
    out = {decl: max(v.items(), key=lambda kv: kv[1])[0] for decl, v in votes.items()}
    for target, source in aliases:
        if target not in out and source in out:
            out[target] = out[source]
        if source not in out and target in out:
            out[source] = out[target]
    return out


def _record_of(node, assigned):
    """'struct X' when the node is a pointer to a record, &record, or a
    void/byte pointer that was assigned one."""
    node = _strip(node)
    if node.get('kind') == 'CStyleCastExpr' and node.get('inner'):
        node = _strip(node['inner'][0])
    if node.get('kind') == 'DeclRefExpr' and node['referencedDecl'].get('id') in assigned and not _pointee(node):
        return assigned[node['referencedDecl']['id']]
    if node.get('kind') == 'UnaryOperator' and node.get('opcode') == '&' and node.get('inner'):
        t = node['inner'][0].get('type', {})
        q = _qual(t)
        return q if q.startswith(('struct ', 'union ')) and not q.endswith(']') else None
    t = node.get('type', {})
    q = _qual(t)
    if q.endswith(' *') and q.startswith(('struct ', 'union ')):
        return q[:-2]
    return None


def _constant(node):
    node = _strip(node)
    if node.get('kind') == 'IntegerLiteral':
        return int(node['value'])
    return None


def byte_offsets(rel, flags):
    """[(line, record, offset)]: constant byte offsets applied to struct
    pointers (`(u8 *)p + N`, `((u8 *)p)[N]`), read from clang's AST."""
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
        cmd = [build.ZIG, 'cc', *build.COMMON_FLAGS, *flags, '-fsyntax-only', '-Xclang', '-ast-dump=json',
               build.resolve_source(rel)]
        cmd = build.transport_long_defines(cmd, str(Path(temporary) / 'unit.o'))
        out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if not out.stdout.lstrip().startswith('{'):
        return []
    main = os.path.basename(build.resolve_source(rel))
    found = []
    state = {'file': None, 'line': None}

    def note(loc):
        for part in (loc.get('spellingLoc'), loc.get('expansionLoc')) if 'spellingLoc' in loc else (loc,):
            if part and 'offset' in part:
                state['file'] = part.get('file', state['file'])
                state['line'] = part.get('line', state['line'])

    def walk(value):
        if isinstance(value, list):
            for child in value:
                walk(child)
            return
        if not isinstance(value, dict):
            return
        if 'offset' in value:
            note(value)
            return
        if 'spellingLoc' in value:
            note(value)
            return
        candidate = None
        kind = value.get('kind')
        if kind in ('BinaryOperator', 'ArraySubscriptExpr') and len(value.get('inner', ())) == 2 and \
                (kind == 'ArraySubscriptExpr' or value.get('opcode') == '+'):
            base, index = value['inner']
            base = _strip(base)
            if base.get('kind') == 'CStyleCastExpr' and base.get('type', {}).get('qualType') in BYTE_TYPES:
                record = _record_of(base['inner'][0], assigned) if base.get('inner') else None
                offset = _constant(index)
                if record and offset is not None:
                    candidate = (record, offset)
        for key, child in value.items():
            if key == 'range' and candidate:
                walk(child.get('begin', {}))
                if state['file'] is None or os.path.basename(state['file']) == main:
                    found.append((state['line'], candidate[0], candidate[1]))
                walk(child.get('end', {}))
                continue
            walk(child)

    ast = json.loads(out.stdout)
    _TYPEDEFS.map = _typedefs(ast)
    assigned = _assigned_records(ast)
    walk(ast)
    return found


def sources():
    units = [(rel, list(flags)) for rel, flags in build.game_sources()]
    units += [(rel, list(flags)) for rel, flags in build.board_sources()]
    units += [(rel, list(flags)) for rel, flags in build.native_rel_sources()]
    return units


def collect(jobs, only=()):
    """({native target: records}, failures, byte offsets); records are
    {key: {'name', 'size32', 'size64', 'fields': [(name, type, off32, off64)], 'units'}}"""
    """{name: {'size32', 'size64', 'fields': [(name, type, off32, off64)], 'units': set}}, failures"""
    android = android_compiler()
    targets = [t for t in NATIVE_TARGETS if t != ANDROID or android]
    if not android:
        print('[note] no NDK or Android build tree: checking the Windows layout only')
    units = [u for u in sources() if not only or any(o in u[0] for o in only)]

    def layouts(unit):
        rel, flags = unit
        l32, errors = dump(rel, flags, TARGET_32)
        natives = {}
        for target in targets:
            natives[target], more = dump(rel, flags, target, android)
            errors += [f'{target}: {e}' for e in more]
        return rel, l32, natives, errors

    by_target = {target: {} for target in targets}
    failures, offsets = [], []
    with ThreadPoolExecutor(jobs) as pool:
        scanned = [u for u in units if BYTE_CAST.search(Path(build.resolve_source(u[0])).read_text(
            encoding='utf-8', errors='replace'))]
        for unit, found in zip(scanned, pool.map(lambda u: byte_offsets(*u), scanned)):
            offsets += [(unit[0], line, record, offset) for line, record, offset in found]
        for rel, l32, natives, errors in pool.map(layouts, units):
            if errors:
                failures.append((rel, errors[:3]))
            for target, l64 in natives.items():
                records = by_target[target]
                for name, (size32, f32) in l32.items():
                    if name not in l64:
                        continue
                    size64, f64 = l64[name]
                    if [(n, t) for n, t, _ in f32] != [(n, t) for n, t, _ in f64]:
                        continue  # target-dependent declaration; not comparable
                    fields = [(n, t, o32, o64) for (n, t, o32), (_, _, o64) in zip(f32, f64)]
                    # One name can be several definitions (REL-local types).
                    key = (name, tuple(fields))
                    entry = records.setdefault(key, {'name': name, 'size32': size32, 'size64': size64,
                                                     'fields': fields, 'units': set()})
                    entry['units'].add(rel)
    return by_target, failures, offsets


def moved_offsets(records, offsets):
    """Byte offsets whose GameCube field sits elsewhere natively."""
    found = []
    for rel, line, record, offset in offsets:
        recs = [r for r in records.values() if r['name'] == record and rel in r['units']]
        if not recs:
            continue
        fields = recs[0]['fields']
        # The field this GameCube offset falls in, and where it went.
        hit = None
        for name, ftype, o32, o64 in fields:
            if o32 <= offset:
                hit = (name, ftype, o32, o64)
        if hit and hit[3] != hit[2]:
            found.append((rel, line, record, offset, hit[0], hit[3] + (offset - hit[2])))
    return found


def shared_prefix(a, b):
    n = 0
    for fa, fb in zip(a, b):
        if fa[:3] != fb[:3]:
            break
        n += 1
    return n


PAD = re.compile(r'^(u8|s8|char|unsigned char|signed char|uint8_t|int8_t)\[\d+\]$')


def module_of(rec):
    """REL module directory; shared game code pairs only within one file."""
    unit = sorted(rec['units'])[0]
    parts = unit.split('/')
    return parts[2] if len(parts) > 3 and parts[1] == 'REL' else unit


def is_pointer(ftype):
    return ftype.endswith('*') or '(*' in ftype


def pad_hides_pointer(view, other, before32):
    """Does an opaque pad in `view` that ends at or before 32-bit offset
    `before32` cover a pointer field of `other`?"""
    for name, ftype, o32, o64 in view:
        m = PAD.match(ftype)
        if not m or o32 >= before32:
            continue
        end = o32 + int(ftype[ftype.index('[') + 1:-1])
        if any(is_pointer(t) and o32 <= p32 < end for _, t, p32, _ in other):
            return True
    return False


# The decomp spells one type several ways (s32/int, HuVecF/Vec); a view
# written in another file often uses the other spelling.
CANONICAL = {
    's8': 'signed char', 'int8_t': 'signed char', 'u8': 'unsigned char', 'uint8_t': 'unsigned char',
    's16': 'short', 'int16_t': 'short', 'u16': 'unsigned short', 'uint16_t': 'unsigned short',
    's32': 'int', 'int32_t': 'int', 'BOOL': 'int', 'u32': 'unsigned int', 'uint32_t': 'unsigned int',
    'f32': 'float', 'f64': 'double', 'HuVecF': 'Vec', 'struct Vec': 'Vec',
}
TAIL_PAD = 64  # a leading pad this long skips the owner's head: the view keeps only a few tail fields


def canonical(ftype):
    if is_pointer(ftype):
        return 'pointer'
    base, bracket, rest = ftype.partition('[')
    base = base.strip()
    return CANONICAL.get(base, base) + bracket + rest


def tail_view_fields(rec):
    """Fields after a long leading pad (`u8 earlierState[4780]; s32 index;`),
    or None when the record does not start that way."""
    fields = rec['fields']
    if not fields or not PAD.match(fields[0][1]) or fields[0][2] != 0:
        return None
    if int(fields[0][1][fields[0][1].index('[') + 1:-1]) < TAIL_PAD:
        return None
    tail = [f for f in fields[1:] if not PAD.match(f[1])]
    return tail or None


def pad_view_pairs(records):
    """Pairs where one struct skips fields with an opaque byte pad and the
    two agree on at least MIN_SHARED other fields (type at 32-bit offset)
    -- the shape of a partial copy viewing another file's object. A view
    that skips the owner's whole head with one long pad keeps too few
    fields to vote; it pairs with an owner that has every one of its
    fields, by type, at the same GameCube offset (E. Gadd's Garage's
    capsule cannon read its throw-path list this way)."""
    index = defaultdict(set)
    padded = []
    for key, rec in records.items():
        mod = module_of(rec)
        for name, ftype, o32, o64 in rec['fields']:
            if not PAD.match(ftype):
                index[(mod, o32, canonical(ftype))].add(key)
        if any(PAD.match(f[1]) for f in rec['fields']):
            padded.append(key)
    pairs = set()
    for a in padded:
        mod = module_of(records[a])
        votes, named = defaultdict(int), defaultdict(int)
        for name, ftype, o32, o64 in records[a]['fields']:
            if not PAD.match(ftype):
                for b in index[(mod, o32, canonical(ftype))]:
                    if b != a:
                        votes[b] += 1
                        named[b] += any(f[0] == name and f[2] == o32 for f in records[b]['fields'])
        # Type agreement alone pairs unrelated structs; a view also keeps
        # some of the owner's field names.
        tail = tail_view_fields(records[a])
        for b, n in votes.items():
            if (n >= MIN_SHARED and named[b]) or (tail and n == len(tail)):
                pairs.add((min(a, b), max(a, b)))
    return pairs


def mismatches(records):
    """[(view, owner, field, off32, view off64, owner field, owner off64)]"""
    by_lead = defaultdict(list)
    for key, rec in records.items():
        if len(rec['fields']) > MIN_SHARED:
            by_lead[tuple(f[:3] for f in rec['fields'][:MIN_SHARED])].append(key)
    pairs = pad_view_pairs(records)
    for names in by_lead.values():
        for a in names:
            for b in names:
                if a < b:
                    pairs.add((a, b))
    found = []
    for a, b in sorted(pairs):
        fa, fb = records[a]['fields'], records[b]['fields']
        shared = shared_prefix(fa, fb)
        at_b = {f[2]: f for f in fb[shared:] if not PAD.match(f[1])}
        for f in fa[shared:]:
            g = at_b.get(f[2])
            # Same GameCube offset, same kind of field: the two views
            # must agree natively as well.
            if g is None or PAD.match(f[1]) or (canonical(f[1]) != canonical(g[1]) and f[0] != g[0]):
                continue
            # The bug: one side skips the other's pointers with an opaque pad.
            if f[3] != g[3] and (pad_hides_pointer(fa, fb, f[2]) or pad_hides_pointer(fb, fa, f[2])):
                ra, rb = records[a], records[b]
                found.append((ra['name'], rb['name'], f[0], f[2], f[3], g[0], g[3],
                              sorted(ra['units'])[0], sorted(rb['units'])[0]))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-j', type=int, default=4)
    parser.add_argument('--json')
    parser.add_argument('only', nargs='*', help='limit to sources whose path contains one of these')
    args = parser.parse_args()
    by_target, failures, offsets = collect(args.j, args.only)
    for rel, errors in failures:
        print(f'[compile] {rel}: {errors[0]}')
    found, moved = [], []
    for target, records in by_target.items():
        for rel, line, record, offset, field, native in moved_offsets(records, offsets):
            moved.append((target, rel, line, record, offset, field, native))
            print(f'{rel}:{line}: byte offset {offset} into {record} is `{field}` on the GameCube, '
                  f'but that is offset {native} on {target}')
        for a, b, field, o32, o64, other, other64, ua, ub in mismatches(records):
            found.append((target, a, b, field, o32, o64, other, other64, ua, ub))
            print(f'{a} ({ua}) / {b} ({ub}): `{field}` and `{other}` share 32-bit offset {o32}, '
                  f'but sit at {o64} and {other64} on {target}')
    count = max(len(r) for r in by_target.values())
    print(f'{count} records, {len(found)} mismatch(es), {len(offsets)} byte offset(s) '
          f'({len(moved)} moved), {len(failures)} unit(s) failed to compile')
    if args.json:
        Path(args.json).write_text(json.dumps({'mismatches': found, 'moved': moved}, indent=1), encoding='utf-8')
    return 1 if found or moved else 0


if __name__ == '__main__':
    raise SystemExit(main())
