"""Verify the raised W01 lawn against an identical Strong-AO saved scene.

The before run uses the QA-only --no-lawn-lift switch, including rebuilding
the visual contact registry after restore. No gameplay state is altered.
"""
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw
from compare_ao_captures import frame
from measure_board_ground import triangles, height

LAWN = {'haikei10', 'haikei11', 'haikei17'}
BACKGROUND = LAWN | {'haikei1','haikei12','haikei14','haikei15','haikei16','haikei2','grid309'}


def read(directory, name):
    return [json.loads(s) for s in (directory/name).read_text().splitlines()]


def path_gaps(scene):
    floor = [t for m in scene if m['name'] in LAWN for t in triangles(m)]
    path = [m for m in scene if m['name'] == 'start']
    assert len(path) == 1
    gaps = []
    for tri in triangles(path[0]):
        for u in range(21):
            for v in range(21-u):
                p = [tri[0][a]+u/20*(tri[1][a]-tri[0][a])+v/20*(tri[2][a]-tri[0][a]) for a in range(3)]
                hits = [h for t in floor if (h := height(t, p[0], p[2])) is not None and abs(h-p[1]) < 32]
                assert hits, ('Unsupported path sample', p)
                gaps.append(p[1]-max(hits))
    return dict(samples=len(gaps), minimum=min(gaps), maximum=max(gaps))


def compare(before, after):
    runs = [json.loads((p/'result.json').read_text()) for p in (before, after)]
    assert runs[0]['sha256'] == runs[1]['sha256'], 'Executable mismatch'
    assert runs[0]['request']['state_path'] == runs[1]['request']['state_path'], 'State mismatch'
    for run, disabled in zip(runs, (True, False)):
        req = run['request']
        assert run['exit_code'] == 0 and not run['crash_marker']
        assert req['load_at'] is not None and req['ao'] == 2 and req['no_lawn_lift'] == disabled
        assert not any(req[k] for k in ('no_grounding', 'no_log_grounding', 'no_tree_grounding', 'ao_cycle'))
    old, new = (read(p, 'geometry.jsonl') for p in (before, after))
    identity = lambda m: tuple(m[k] for k in ('tick', 'model', 'objectIndex', 'name', 'player'))
    assert old and len(old) == len(new)
    assert len({identity(m) for m in old}) == len(old)
    assert len({identity(m) for m in new}) == len(new)
    report = dict(meshes=len(new), unchanged_logical_positions=0, unchanged_meshes=0,
                  unchanged_player_meshes=0, player_slots=[], lawn={}, other_static_changes=[])
    for a, b in zip(sorted(old, key=identity), sorted(new, key=identity)):
        assert identity(a) == identity(b), 'Scene/tick mismatch'
        assert a['pos'] == b['pos'], (a['name'], 'Logical position changed')
        report['unchanged_logical_positions'] += 1
        if a['vertices'] == b['vertices']:
            report['unchanged_meshes'] += 1
        av, bv = np.array(a['vertices']), np.array(b['vertices'])
        assert av.shape == bv.shape and np.isfinite(bv).all()
        if a['name'] in BACKGROUND:
            changed = np.any(av != bv, axis=1)
            assert changed.any(), (a['name'], 'Lawn was not raised')
            assert changed.all(), 'Only part of the connected background was raised'
            assert np.max(np.abs(av[:, [0, 2]]-bv[:, [0, 2]])) < .002, 'Horizontal displacement'
            lift = bv[changed, 1]-av[changed, 1]
            assert np.all((lift > 15.12) & (lift < 15.15)), lift
            report['lawn'][a['name']] = dict(raised_vertices=int(changed.sum()),
                unchanged_vertices=int((~changed).sum()), lift_min=float(lift.min()), lift_max=float(lift.max()))
        elif a['vertices'] != b['vertices']:
            static_tree = b['name'] in ('b01_m240', 'r_ashi', 'l_ashi') and b['cenv'] == 0
            assert b['player'] < 0 and (b['aoScenery'] or static_tree), 'Dynamic geometry moved'
            changed = np.any(av != bv, axis=1)
            assert np.max(np.abs((bv-av)[:, [0, 2]])) < .002, 'Scenery shifted horizontally'
            assert np.all((bv-av)[changed, 1] > 0), 'Existing scenery correction moved downward'
            report['other_static_changes'].append(dict(name=b['name'], object_index=b['objectIndex'],
                changed_vertices=int(changed.sum()), dy_min=float((bv-av)[changed, 1].min()),
                dy_max=float((bv-av)[changed, 1].max())))
            if b['name'] == 'start':
                assert changed.all() and np.all((bv-av)[:, 1] > 8.6) and np.all((bv-av)[:, 1] < 8.7)
                report['path_lift'] = dict(minimum=float((bv-av)[:, 1].min()), maximum=float((bv-av)[:, 1].max()))
        if b['player'] >= 0:
            assert a['vertices'] == b['vertices']
            report['unchanged_player_meshes'] += 1
            report['player_slots'].append(b['player'])
    assert LAWN.issubset(report['lawn'])
    report['player_slots'] = sorted(set(report['player_slots']))
    assert report['player_slots'] == [0, 1, 2, 3]
    spaces = [read(p, 'spaces.jsonl') for p in (before, after)]
    assert spaces[0] and spaces[0] == spaces[1] and all(not s['corrected'] for s in spaces[1])
    report['unchanged_space_matrices'] = len(spaces[1])
    assert report.get('path_lift'), 'Path not raised to the authored start position'
    path = next(m for m in new if m['name'] == 'start')
    report['feet'] = {}
    # Shoe-bearing body meshes, not eyebrows/eyes or Peach's lower dress hem.
    for name in ('mario_m2', 'luigi_m2', 'yoshi_m2', 'foot_R'):
        mesh = next(m for m in new if m['name'] == name)
        p = min(mesh['vertices'], key=lambda p: p[1])
        hits = [h for t in triangles(path) if (h := height(t, p[0], p[2])) is not None]
        assert hits, (name, 'Foot not above starting path')
        gap = p[1]-max(hits)
        # Slight sole overlap is normal for the independent authored idle poses;
        # the fixed surface must not chase animation or leave the old 8-9 unit gap.
        assert -1 < gap < .5, (name, gap)
        report['feet'][name] = dict(y=p[1], path_y=max(hits), gap=gap)
    report['path_before'], report['path_after'] = map(path_gaps, (old, new))
    assert report['path_before']['minimum'] > 6.9
    assert .49 < report['path_after']['minimum'] < report['path_after']['maximum'] < .65
    floor = [t for m in new if m['name'] in LAWN or m['name']=='start' for t in triangles(m)]
    report['start_space_clearance'] = {}
    for space in spaces[1]:
        if space['id'] not in (18, 21, 28, 64):
            continue
        m = np.array(space['matrix']).reshape(3,4)
        gaps = []
        for x in range(-100, 101, 10):
            for z in range(-100, 101, 10):
                p = m @ np.array([x, 0, z, 1])
                hits = [h for t in floor if (h := height(t, p[0], p[2])) is not None and abs(h-p[1]) < 32]
                assert hits, (space['id'], 'Unsupported artwork sample')
                gaps.append(p[1]-max(hits))
        assert min(gaps) > 3.2, (space['id'], 'Raised surface clips board-space artwork')
        report['start_space_clearance'][space['id']] = min(gaps)
    assert len(report['start_space_clearance']) == 4
    (after/'lawn-lift-metrics.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='other_static_changes'}, indent=2))
    captures = [frame(sorted((p/'frames').glob('*.mfd'))[0]) for p in (before, after)]
    assert captures[0][0] == captures[1][0], 'Framebuffer tick mismatch'
    for name, (_, pixels) in zip(('before', 'after'), captures):
        Image.fromarray(pixels).save(after/f'lawn-{name}.png')
    # Identical native-resolution crop: real pixels, no magnification/retouching.
    # Show the feet and their receiving surface, not a high camera crop.
    crop = (350, 450, 600, 515)
    pair = Image.new('RGB', (520, 95), (24, 27, 33))
    labels = ImageDraw.Draw(pair)
    for i, (label, (_, pixels)) in enumerate(zip(('Before', 'Raised surface'), captures)):
        labels.text((10+i*260, 4), label, fill='white')
        pair.paste(Image.fromarray(pixels).crop(crop), (10+i*260, 25))
    pair.save(after/'foot-contact-before-after.png')


if __name__ == '__main__':
    compare(*map(Path, sys.argv[1:3]))
