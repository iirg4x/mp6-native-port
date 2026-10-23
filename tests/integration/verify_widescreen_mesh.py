"""Compare engine face censuses, including native positions, UVs and colors.

Only appended QUADs are allowed on explicitly selected meshes. This checks
observed geometry, not camera coverage or gameplay completion.
"""
import argparse
from pathlib import Path
import re


def census(path, name, model=None):
    faces = {}
    owner = re.escape(model) if model is not None else r'\S+'
    pattern = re.compile(r'\[WIDE-MESH\] ' + owner + '/' + re.escape(name) + r' face=(\d+) (.*)')
    for line in path.read_text(errors='replace').splitlines():
        match = pattern.fullmatch(line)
        if match:
            index = int(match[1])
            if not match[2].startswith(('type=2 ', 'type=3 ')):
                raise ValueError(f'{name}: census does not include triangle-strip tails')
            if index in faces:
                raise ValueError(f'{name}: ambiguous duplicate face {index}')
            faces[index] = match[2]
    if not faces or set(faces) != set(range(len(faces))):
        raise ValueError(f'{name}: missing or incomplete census in {path}')
    return faces


def verify(before, after, name, added=0, model=None):
    native, widened = census(before, name, model), census(after, name, model)
    if len(widened) != len(native) + added:
        raise ValueError(f'{name}: expected {added} added faces; got {len(widened)-len(native)}')
    for index, face in native.items():
        if widened[index] != face:
            raise ValueError(f'{name}: native face {index} changed')
    for index in range(len(native), len(widened)):
        if not widened[index].startswith('type=3 '):
            raise ValueError(f'{name}: appended face {index} is not a QUAD')
    return f'{name}: {len(native)} native faces identical, {added} added QUADs'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--extend', nargs='*', default=[], metavar='NAME:COUNT')
    parser.add_argument('--unchanged', nargs='*', default=[])
    parser.add_argument('--model', help='Exact census owner, e.g. wide-ovl7-model0')
    args = parser.parse_args()
    for item in args.extend:
        name, count = item.rsplit(':', 1)
        count = int(count)
        if count <= 0:
            parser.error('Extensions must add at least one face')
        print(verify(args.before, args.after, name, count, args.model))
    for name in args.unchanged:
        print(verify(args.before, args.after, name, model=args.model))
    if not args.extend and not args.unchanged:
        parser.error('Specify meshes to verify')


if __name__ == '__main__':
    main()
