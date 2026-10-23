"""Derive REL state-reset lists from the compiler's view of each source.

A native overlay stays linked between visits, so every recovered REL source
lists its mutable file-scope data and BSS for MP6_REL_RESET_DEFINE
(include/mp6_rel_state.h). Writing those lists by hand misses variables.
This reads the variables from clang's AST instead (via zig cc, with the
same flags the build uses):

    python tools/gen_rel_state.py --check
        Compare every existing list with the AST and report differences.
    python tools/gen_rel_state.py MODULE [--entry UNIT]
        Write src/rel_state/<path>.inc for each unit of MODULE (a directory
        under src/REL listed in a native inventory) that has mutable storage
        and no list yet, plus the module's mp6_<module>_reset aggregate in
        UNIT (default: the first unit with storage).

A variable is data when it has an initializer and BSS otherwise. Variables
whose own type is const are skipped; extern declarations without an
initializer are not definitions. Run tools/apply_patches.py first.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

MARKER = '#include "mp6_rel_state.h"'
REL_STATE = ROOT / 'src' / 'rel_state'
LIST_RE = re.compile(r'#define (MP6_\w+)_(BSS|DATA)\(X\)((?:[^\n]*\\\n)*[^\n]*)')
ALIAS_RE = re.compile(r'-D([A-Za-z_]\w*)=([A-Za-z_]\w*)\Z')


def inc_path(rel):
    return REL_STATE / (rel[len('src/'):] + '.inc')


def _locations(value):
    """Every source location in the JSON dump, in document order. Clang names
    a file only when it differs from the previously printed location, so the
    file of any one location depends on all locations printed before it."""
    if isinstance(value, dict):
        if 'offset' in value:
            yield value
        for key, child in value.items():
            if key != 'includedFrom':
                yield from _locations(child)
    elif isinstance(value, list):
        for child in value:
            yield from _locations(child)


def _decl_file(node, current):
    """(file of the declaration's own location, file after the whole node)."""
    # 'loc' is the first key holding locations; for a macro it is printed as
    # spellingLoc then expansionLoc, and the expansion is where it is declared.
    for part in _locations(node.get('loc', {})):
        current = part.get('file', current)
    here = current
    for key, value in node.items():
        if key != 'loc':
            for part in _locations(value):
                current = part.get('file', current)
    return here, current


def _own_type_is_const(qual_type):
    base = re.sub(r'(\[[^\]]*\])+$', '', qual_type).strip()
    if '(' in base:  # pointer to function, or array of them
        return re.search(r'\*\s*const\b', base) is not None
    if '*' in base:  # only a const after the last '*' qualifies the pointer itself
        return re.search(r'\bconst\b', base.rsplit('*', 1)[1]) is not None
    return re.search(r'\bconst\b', base) is not None


def compiler_storage(rel, flags):
    """(data, bss) names of the mutable file-scope variables in the source
    without its reset block, in declaration order, spelled as in the source."""
    text = Path(build.resolve_source(rel)).read_text(encoding='utf-8')
    at = text.find(MARKER)
    if at >= 0:
        text = text[:at]
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
        source = Path(temporary) / Path(rel).name
        source.write_text(text, encoding='utf-8')
        cmd = [build.ZIG, 'cc', *build.COMMON_FLAGS, *flags,
               '-I', str(Path(build.PATCHED_SRC_DIR) / Path(rel).parent),
               '-I', str(Path(build.DECOMP) / Path(rel).parent),
               '-fsyntax-only', '-Xclang', '-ast-dump=json', str(source)]
        cmd = build.transport_long_defines(cmd, str(Path(temporary) / 'unit.o'))
        out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    # zig cc exits non-zero after a dump (no object written); judge by output.
    if not out.stdout.lstrip().startswith('{'):
        raise RuntimeError(f'{rel}: clang AST dump failed\n{out.stderr[-2000:]}')
    ast = json.loads(out.stdout)
    renamed = {m.group(2): m.group(1) for m in map(ALIAS_RE.match, flags) if m}
    data, bss, seen = [], [], set()
    current = None
    for node in ast.get('inner', ()):
        here, current = _decl_file(node, current)
        if node.get('kind') != 'VarDecl' or here is None or Path(here).name != source.name:
            continue
        has_init = 'init' in node
        if node.get('storageClass') == 'extern' and not has_init:
            continue
        if _own_type_is_const(node.get('type', {}).get('qualType', '')):
            continue
        name = renamed.get(node['name'], node['name'])
        if name in seen:
            # A tentative definition can precede the initialized one; the
            # variable is data then, or a reset would zero its initializer.
            if has_init and name in bss:
                bss.remove(name)
                data.append(name)
            continue
        seen.add(name)
        (data if has_init else bss).append(name)
    return data, bss


def written_lists(rel):
    """{owner prefix: {'BSS': [...], 'DATA': [...]}} as the build compiles them."""
    text = Path(build.resolve_source(rel)).read_text(encoding='utf-8')
    out = {}
    for m in LIST_RE.finditer(text):
        out.setdefault(m.group(1), {'BSS': [], 'DATA': []})[m.group(2)] = re.findall(r'X\(([\w.]+)\)', m.group(3))
    return out


def rel_sources():
    return dict(build.native_rel_sources())


def differences(jobs):
    """(sources checked, ['<rel> data|bss: missing [...] extra [...]', ...])."""
    sources = rel_sources()
    owned = [rel for rel in sources if rel.startswith('src/REL/') and inc_path(rel).exists()]
    with ThreadPoolExecutor(jobs) as pool:
        derived = dict(zip(owned, pool.map(lambda rel: compiler_storage(rel, sources[rel]), owned)))
    problems = []
    for rel in owned:
        data, bss = derived[rel]
        lists = written_lists(rel)
        listed_data = {n for v in lists.values() for n in v['DATA']}
        listed_bss = {n for v in lists.values() for n in v['BSS']}
        for kind, listed, actual in (('data', listed_data, set(data)), ('bss', listed_bss, set(bss))):
            if listed != actual:
                problems.append(f'{rel} {kind}: missing {sorted(actual - listed)} extra {sorted(listed - actual)}')
    return len(owned), problems


def check(jobs):
    checked, problems = differences(jobs)
    for problem in problems:
        print(problem)
    print(f'{checked} source(s) checked, {len(problems)} difference(s)')
    return 1 if problems else 0


def write_module(module, entry, jobs):
    sources = rel_sources()
    units = [rel for rel in sources if rel.startswith(f'src/REL/{module}/')]
    if not units:
        raise SystemExit(f'{module}: not in a native REL inventory (tools/build.py native_rel_sources)')
    with ThreadPoolExecutor(jobs) as pool:
        derived = dict(zip(units, pool.map(lambda rel: compiler_storage(rel, sources[rel]), units)))
    prefix = module.lower().removesuffix('dll')
    stateful = [rel for rel in units if any(derived[rel])]
    if not stateful:
        print(f'{module}: no mutable file-scope storage')
        return 0
    entry_rel = f'src/REL/{module}/{entry}.c' if entry else stateful[0]
    if entry_rel not in stateful:
        raise SystemExit(f'{entry_rel}: not a unit with mutable storage')
    aggregate_exists = any(f'void mp6_{prefix}_reset(void)' in inc_path(rel).read_text(encoding='utf-8')
                           for rel in units if inc_path(rel).exists())
    for rel in stateful:
        target = inc_path(rel)
        unit = Path(rel).stem
        if target.exists() or written_lists(rel):
            print(f'{rel}: already has a list (see --check)')
            continue
        data, bss = derived[rel]
        macro = f'MP6_{prefix.upper()}_{unit.upper()}'

        def listing(kind, names):
            if not names:
                return f'#define {macro}_{kind}(X)\n'
            return f'#define {macro}_{kind}(X) \\\n' + ' \\\n'.join(f'    X({n})' for n in names) + '\n'
        text = (MARKER + '\n' + listing('BSS', bss) + listing('DATA', data)
                + f'MP6_REL_RESET_DEFINE(mp6_{prefix}_{unit}_reset, {macro}_BSS, {macro}_DATA)\n')
        if rel == entry_rel and not aggregate_exists:
            others = [Path(r).stem for r in stateful if r != rel]
            text += '\n' + ''.join(f'void mp6_{prefix}_{o}_reset(void);\n' for o in others)
            text += (f'void mp6_{prefix}_reset(void)\n{{\n'
                     + ''.join(f'    mp6_{prefix}_{u}_reset();\n' for u in [unit] + others) + '}\n')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8', newline='\n')
        print(f'wrote {target.relative_to(ROOT).as_posix()} ({len(data)} data, {len(bss)} bss)')
    print(f'Call mp6_{prefix}_reset from the module\'s ctor list in src/os/dll_bridge.c, '
          'then rerun tools/apply_patches.py.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('module', nargs='?')
    parser.add_argument('--entry')
    parser.add_argument('--check', action='store_true')
    # Each dump is tens of MB of JSON; keep the default modest.
    parser.add_argument('-j', type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.check:
        return check(args.j)
    if not args.module:
        parser.error('give a MODULE or --check')
    return write_module(args.module, args.entry, args.j)


if __name__ == '__main__':
    raise SystemExit(main())
