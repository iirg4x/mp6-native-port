"""Read W01 directly from the retail ISO and compare static mesh coordinates.

This is an asset/transform audit, not proof of the original GPU's final image.
No port loader, native matrix functions or geometry corrections are executed.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib
import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def u32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def iso_file(path, name):
    with path.open('rb') as disc:
        assert disc.read(6) == b'GP6E01', 'Expected the USA Mario Party 6 disc'
        disc.seek(0x424)
        fst_at, fst_size = struct.unpack('>II', disc.read(8))
        disc.seek(fst_at)
        fst = disc.read(fst_size)
        count = u32(fst, 8)
        strings = count * 12
        stack = [('', count)]
        for i in range(1, count):
            while i >= stack[-1][1]:
                stack.pop()
            kind_name, offset, size = struct.unpack_from('>III', fst, i*12)
            at = strings + (kind_name & 0xffffff)
            entry = fst[at:fst.index(b'\0', at)].decode('ascii')
            full = stack[-1][0] + entry
            if kind_name >> 24:
                stack.append((full+'/', size))
            elif full == name:
                disc.seek(offset)
                result = disc.read(size)
                assert len(result) == size
                return result, offset
    raise ValueError(f'Not found in ISO: {name}')


def unpack_entry(archive, index):
    assert index < u32(archive, 0)
    at = u32(archive, 4+index*4)
    size, kind = struct.unpack_from('>II', archive, at)
    payload = archive[at+8:]
    if kind == 0:
        result = payload[:size]
    elif kind == 7:
        compressed = u32(payload, 4)
        result = zlib.decompress(payload[8:8+compressed])
    else:
        raise ValueError(f'Unsupported audit compression {kind}; do not guess')
    assert len(result) == size
    return result


def read_meshes(data, names=('start','haikei10','haikei11','haikei17')):
    assert data[:3] == b'HSF'
    sec = [struct.unpack_from('>II', data, 8+i*8) for i in range(21)]
    obj_at, obj_count = sec[8]
    string_at, string_size = sec[20]
    vert_at, vert_count = sec[4]
    objects = []
    for i in range(obj_count):
        at = obj_at+i*324
        name_at = string_at+u32(data, at)
        name = data[name_at:data.index(b'\0', name_at, string_at+string_size)].decode('ascii')
        parent = struct.unpack_from('>i', data, at+16)[0]
        transform = struct.unpack_from('>9f', data, at+28)
        vertex = struct.unpack_from('>i', data, at+264)[0]
        objects.append(dict(name=name, parent=parent, transform=transform, vertex=vertex))

    def matrix(index, seen=()):
        assert index not in seen and len(seen)<64
        obj = objects[index]
        t = obj['transform']
        local = np.diag([*t[6:9],1.0])
        x,y,z = map(math.radians,t[3:6])
        for axis,angle in enumerate((x,y,z)):
            c,s=math.cos(angle),math.sin(angle)
            rotation=np.eye(4)
            a,b=((1,2),(2,0),(0,1))[axis]
            rotation[a,a]=rotation[b,b]=c
            rotation[a,b]=-s; rotation[b,a]=s
            local=rotation@local
        local[:3,3]=t[:3]
        if obj['parent']>=0:
            local=matrix(obj['parent'],(*seen,index))@local
        return local

    for i,obj in enumerate(objects):
        if names is not None and obj['name'] not in names:
            continue
        if names is None and not (0 <= obj['vertex'] < vert_count):
            continue
        assert 0 <= obj['vertex'] < vert_count
        header=vert_at+obj['vertex']*12
        count, relative=struct.unpack_from('>II',data,header+4)
        points=np.array([struct.unpack_from('>3f',data,vert_at+vert_count*12+relative+j*12) for j in range(count)])
        transformed=(matrix(i)@np.column_stack((points,np.ones(count))).T).T[:,:3]
        yield i,obj,transformed


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=Path('D:/Games/Emulation/GameCube-Wii/Games/Mario Party 6 (USA).iso'))
    parser.add_argument('--capture', type=Path, default=ROOT/'build/board-qa-runs/ao-placement-actors-off')
    args=parser.parse_args()
    archive, offset=iso_file(args.iso,'data/w01.bin')
    cached=(ROOT/'build/disc-cache/orig/GP6E01/files/data/w01.bin').read_bytes()
    assert archive==cached, 'Cached archive differs from the ISO'
    hsf=unpack_entry(archive,4)
    capture=[json.loads(s) for s in (args.capture/'geometry.jsonl').read_text().splitlines()]
    report=dict(iso=str(args.iso), archive_offset=offset, archive_sha256=hashlib.sha256(archive).hexdigest(),
                hsf_sha256=hashlib.sha256(hsf).hexdigest(), meshes={})
    for i,obj,points in read_meshes(hsf):
        recorded=next(m for m in capture if m['model']==31 and m['objectIndex']==i and m['name']==obj['name'])
        actual=np.array(recorded['vertices'])
        assert actual.shape==points.shape
        error=np.abs(actual-points)
        report['meshes'][obj['name']]=dict(vertices=len(points),
            iso_world_min=points.min(axis=0).tolist(),iso_world_max=points.max(axis=0).tolist(),
            max_port_coordinate_error=float(error.max()), max_port_y_error=float(error[:,1].max()),
            object_file_offset=u32(hsf,8+8*8)+i*324,base_transform=obj['transform'])
        assert error.max()<.02, (obj['name'],error.max())
    assert len(report['meshes'])==4
    print(json.dumps(report,indent=2))
    (args.capture/'iso-ground-audit.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
