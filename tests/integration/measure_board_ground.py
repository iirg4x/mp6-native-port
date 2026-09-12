"""Measure submitted geometry captured by the QA-only ground probe."""
import json
from pathlib import Path
import sys


def triangles(mesh):
    v = mesh['vertices']
    for face in mesh['faces']:
        kind, *indices = face
        if kind == 2:
            groups = [indices[:3]]
        elif kind == 3:
            # Native GX_QUADS order is 0,2,3,1 (not file tuple order).
            groups = [[indices[0], indices[2], indices[3]], [indices[0], indices[3], indices[1]]]
        elif kind == 4:
            indices = [indices[0], indices[2], indices[1], *indices[3:]]
            groups = [indices[i:i+3] for i in range(len(indices)-2)]
        else:
            continue
        for group in groups:
            if all(0 <= i < len(v) for i in group):
                yield [v[i] for i in group]


def height(tri, x, z):
    a, b, c = tri
    bx, bz = b[0]-a[0], b[2]-a[2]
    cx, cz = c[0]-a[0], c[2]-a[2]
    det = bx*cz-bz*cx
    if abs(det) < 1e-6:
        return None
    u = ((x-a[0])*cz-(z-a[2])*cx)/det
    v = (bx*(z-a[2])-bz*(x-a[0]))/det
    if min(u, v, 1-u-v) < -1e-5:
        return None
    return a[1]+u*(b[1]-a[1])+v*(c[1]-a[1])


def inspect(path):
    meshes = [json.loads(s) for s in Path(path).read_text().splitlines()]
    for m in meshes:
        m['triangles'] = list(triangles(m))
    for m in meshes:
        if not m['vertices']:
            continue
        lo = [min(v[i] for v in m['vertices']) for i in range(3)]
        hi = [max(v[i] for v in m['vertices']) for i in range(3)]
        print('mesh', m['model'], m['name'], 'player', m['player'], 'cenv',m['cenv'],
              'bounds', [round(x, 3) for x in lo], [round(x, 3) for x in hi])
        if m['player'] < 0:
            continue
        feet = sorted(m['vertices'], key=lambda v: v[1])[:3]
        for foot in feet[:1]:
            hits = []
            for other in meshes:
                if other['model'] == m['model'] or other['player'] >= 0:
                    continue
                for tri in other['triangles']:
                    y = height(tri, foot[0], foot[2])
                    if y is not None and abs(y-foot[1]) < 100:
                        hits.append((round(foot[1]-y, 4), other['model'], other['name'], round(y, 4)))
            print(' FOOT', m['name'], foot, 'hits', sorted(set(hits), key=lambda h:abs(h[0]))[:8])


if __name__ == '__main__':
    inspect(sys.argv[1])
