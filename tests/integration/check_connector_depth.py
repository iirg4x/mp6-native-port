"""Verify white path links above raised W01 surfaces in a same-state A/B.

The baseline uses only --no-connector-lift. Geometry, original material state,
space ordering and actual framebuffer pixels are checked independently.
"""
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw
from compare_ao_captures import frame
from measure_board_ground import triangles, height


def lines(path):
    return [json.loads(s) for s in path.read_text().splitlines()]


def compare(before, after):
    runs = [json.loads((p/'result.json').read_text()) for p in (before, after)]
    assert runs[0]['sha256'] == runs[1]['sha256'], 'Build mismatch'
    for field in ('state_path', 'load_at', 'aa', 'ao', 'window_size', 'widescreen'):
        assert runs[0]['request'][field] == runs[1]['request'][field], field
    for run, disabled in zip(runs, (True, False)):
        request = run['request']
        assert run['exit_code'] == 0 and not run['crash_marker']
        assert request['load_at'] is not None and request['ao'] == 2
        assert request['no_connector_lift'] == disabled
        assert not any(request.get(k) for k in ('unshaded_ao', 'no_lawn_lift', 'no_grounding', 'ao_cycle'))
    scenes = [lines(p/'geometry.jsonl') for p in (before, after)]
    key = lambda m: tuple(m[k] for k in ('tick', 'model', 'name', 'objectIndex', 'player'))
    assert scenes[0] and len(scenes[0]) == len(scenes[1])
    assert all(len({key(m) for m in scene}) == len(scene) for scene in scenes)
    report = dict(unchanged_meshes=0, unchanged_logical_positions=0, links=[])
    for a, b in zip(*(sorted(s, key=key) for s in scenes)):
        assert key(a) == key(b)
        assert a['pos'] == b['pos']
        report['unchanged_logical_positions'] += 1
        if a['vertices'] == b['vertices']:
            report['unchanged_meshes'] += 1
            continue
        assert a['name'] == 'b01_m001' and a['cenv'] == 0 and a['player'] < 0
        assert {k:v for k,v in a.items() if k!='vertices'} == {k:v for k,v in b.items() if k!='vertices'}, 'Non-geometry change'
        av, bv = np.array(a['vertices']), np.array(b['vertices'])
        changed = np.any(av != bv, axis=1)
        assert changed.sum() == 16 and (~changed).sum() == 352
        assert np.all(np.abs(av[changed, 1]-2) < .005)
        assert np.all(np.abs(bv[changed, 1]-6.25) < .005)
        assert np.max(np.abs(av[:, [0, 2]]-bv[:, [0, 2]])) < .002
        floor = [t for m in scenes[1] if m['name'] in ('haikei10', 'haikei11', 'haikei17', 'start') for t in triangles(m)]
        gaps = []
        for t in triangles(b):
            if not all(6.24 < p[1] < 6.26 for p in t):
                continue
            for u in range(21):
                for v in range(21-u):
                    p = [t[0][i]+u/20*(t[1][i]-t[0][i])+v/20*(t[2][i]-t[0][i]) for i in range(3)]
                    hits = [h for f in floor if (h:=height(f,p[0],p[2])) is not None and abs(h-p[1]) < 32]
                    assert hits
                    gaps.append(p[1]-max(hits))
        assert len(gaps) == 1848 and min(gaps) > .49
        # The original space layer is at Y=9 here, still above the connector.
        spaces = lines(after/'spaces.jsonl')
        space_y = min(s['matrix'][7] for s in spaces if s['id'] in (18,21,28))
        assert space_y-bv[changed,1].max() > 2.7
        report['links'].append(dict(raised_vertices=int(changed.sum()), unchanged_vertices=int((~changed).sum()),
            samples=len(gaps), minimum_surface_clearance=min(gaps), maximum_surface_clearance=max(gaps),
            minimum_space_clearance=float(space_y-bv[changed,1].max())))
    assert len(report['links']) == 1
    assert lines(before/'spaces.jsonl') == lines(after/'spaces.jsonl')
    frames = [frame(sorted((p/'frames').glob('*.mfd'))[0]) for p in (before, after)]
    assert frames[0][0] == frames[1][0]
    a, b = (f[1] for f in frames)
    assert a.shape == b.shape == (720,1280,3)
    delta = b.astype(np.int16)-a.astype(np.int16)
    # Middle of each white connector, clear of space icons/foreground geometry.
    white = np.zeros((720,1280), bool)
    white[331:343,460:540] = True
    white[327:340,670:745] = True
    report['brighter_connector_pixels'] = int((np.any(delta > 8, axis=2) & white).sum())
    assert report['brighter_connector_pixels'] > 150, 'Connector not restored in actual framebuffer'
    blue = np.zeros((720,1280), bool)
    blue[327:338,370:410] = True
    blue[326:337,580:620] = True
    blue[322:334,790:830] = True
    assert np.max(np.abs(delta[blue])) <= 1, 'Space icon was overpainted'
    report['space_icon_max_rgb_delta'] = int(np.max(np.abs(delta[blue])))
    (after/'connector-depth-check.json').write_text(json.dumps(report,indent=2)+'\n')
    # Display actual native-resolution pixels with no resampling/retouching.
    crop = (300,310,880,365)
    sheet = Image.new('RGB',(590,164),(24,27,33)); draw = ImageDraw.Draw(sheet)
    for i,(label,pixels) in enumerate(zip(('Before: covered connector','After: restored connector'),(a,b))):
        draw.text((5,i*82+4),label,fill='white')
        sheet.paste(Image.fromarray(pixels).crop(crop),(5,i*82+22))
    sheet.save(after/'connector-before-after.png')
    Image.fromarray(b).save(after/'connector-after.png')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    compare(*map(Path,sys.argv[1:3]))
