"""Inventory widescreen-sensitive calls in the actual native source closure.

This is a navigation/coverage aid, not proof that a literal is wrong. Texture
copies, shadows and split-screen views require their own coordinate domains.
Dependency sources are read-only; apply compatibility patches in memory.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

try:
    from tools import apply_patches, build
except ModuleNotFoundError:
    import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]
CALLS = re.compile(r'\b(?:Hu3DCamera(?:Create|Kill|PerspectiveSet|ViewportSet|ScissorSet)|'
                   r'Hu3DFbCopy(?:Exec|LayerSet)|'
                   r'(?:C_)?MTX(?:Ortho|Perspective|LightPerspective)|'
                   r'GXSet(?:Viewport(?:Jitter|Render)?|Scissor(?:Render)?|TexCopySrc|TexCopyDst)|'
                   r'HuSpr(?:GrpPosSet|PosSet|GrpScaleSet|ScaleSet|ScissorSet)|'
                   r'HuWin(?:Create|CreateF|PosSet|ScaleSet)|esp(?:PosSet|ScaleSet)|'
                   r'GameMesPosSet|Hu3DModelScaleSet|mbObjScaleSet|'
                   r'mp6_widescreen_\w+)\s*\(')


def uncomment(text):
    # Preserve quoted strings and line numbers while removing both comment forms.
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                  lambda m: re.sub(r'[^\n]', ' ', m[0]) if m[0].startswith('/') else m[0],
                  text, flags=re.S)


def calls(text):
    text = uncomment(text)
    # Strings can quote APIs, braces or parentheses; none are C syntax. Keep
    # offsets stable so recorded arguments still contain useful mesh names.
    syntax = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: re.sub(r'[^\n]', ' ', m[0]), text)
    functions = []
    signatures = []
    definitions = re.compile(r'^[A-Za-z_][\w\s*]*?\b(\w+)\s*\([^;{}]*\)\s*\{', re.M)
    for definition in definitions.finditer(syntax):
        depth, end = 1, definition.end()
        while depth and end < len(syntax):
            depth += (syntax[end] == '{') - (syntax[end] == '}')
            end += 1
        if not depth:
            functions.append((definition.end(), end, definition[1]))
            signatures.append((definition.start(), definition.end()))
    for match in CALLS.finditer(syntax):
        if any(lo <= match.start() < hi for lo, hi in signatures):
            continue
        start = match.end()
        depth, end = 1, start
        while depth and end < len(syntax):
            depth += (syntax[end] == '(') - (syntax[end] == ')')
            end += 1
        if not depth:
            owner = next((name for lo, hi, name in functions if lo <= match.start() < hi), None)
            # Definitions/prototypes are not call sites. Keep unknown-domain
            # macro calls, instead of silently discarding them as declarations.
            if owner is None:
                prefix=syntax[syntax.rfind('\n',0,match.start())+1:match.start()].strip()
                # Include project typedef return types, but not return
                # expressions or assignment/lambda/macro wrappers.
                if (prefix and 'return' not in prefix.split() and
                        re.fullmatch(r'[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*\s*\**',prefix)):
                    continue
            yield dict(line=text.count('\n', 0, match.start())+1,
                       api=match[0].split('(')[0].strip(),
                       function=owner,
                       args=' '.join(text[start:end-1].split()))


def inventory():
    decomp = Path(apply_patches.require_pinned_decomp())
    selected = (list(build.game_sources()) + list(build.board_sources())
                + build.native_rel_sources())
    result = []
    for rel, _ in selected:
        source = (decomp / rel).read_text(encoding='utf-8')
        patch = ROOT / 'compat/decomp' / (rel + '.patch')
        if patch.exists():
            source = apply_patches.apply_unified_diff(source, patch.read_text(encoding='utf-8'), label=rel)
        owner = '/'.join(Path(rel).parts[1:-1])
        result.append(dict(source=rel, owner=owner, patched=patch.exists(), calls=list(calls(source))))
    return result


def port_inventory():
    # Inspect actual port adapters too, without invoking collect_units (which
    # stages patched dependency sources). Include both host platform variants.
    paths = set(build.PLATFORM_SOURCES_COMMON + build.PLATFORM_AURORA_ONLY)
    paths.update((build.MAIN_NATIVE, 'src/host/host_android.c'))
    return [dict(source=rel, owner='port/'+'/'.join(Path(rel).parts[1:-1]),
                 patched=False, calls=list(calls((ROOT/rel).read_text(encoding='utf-8'))))
            for rel in sorted(paths)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/widescreen-audit/inventory.json')
    parser.add_argument('--owner', help='Print calls for one owner, e.g. REL/mdpartydll')
    args = parser.parse_args()
    rows = inventory() + port_inventory()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
    print(f'{len(rows)} native + port source units; inventory: {args.output}')
    if args.owner:
        for row in rows:
            if row['owner'] == args.owner:
                for call in row['calls']:
                    print(f'{row["source"]}:{call["line"]} {call["function"] or "<macro/global>"}: {call["api"]}({call["args"]})')
    else:
        for owner in sorted({r['owner'] for r in rows}):
            group = [r for r in rows if r['owner'] == owner]
            counts = Counter(c['api'] for r in group for c in r['calls'])
            print(f'{owner}: {len(group)} units; ' + ', '.join(f'{k}={v}' for k,v in sorted(counts.items())))


if __name__ == '__main__':
    main()
