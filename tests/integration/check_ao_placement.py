"""Check same-state AO off/on captures without allowing dynamic placement changes."""
import argparse
import json
from pathlib import Path


def read(directory, name):
    return [json.loads(line) for line in (directory / name).read_text().splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('off', type=Path)
    parser.add_argument('on', type=Path)
    args = parser.parse_args()
    reports = [json.loads((path / 'result.json').read_text()) for path in (args.off, args.on)]
    assert reports[0]['sha256'] == reports[1]['sha256'], 'Different executable builds'
    assert reports[0]['request']['state_path'] == reports[1]['request']['state_path'], 'Different states'
    for run, level in zip(reports, (0, 2)):
        request = run['request']
        assert run['exit_code'] == 0 and not run['crash_marker'], 'Failed run'
        assert request['load_at'] is not None and request['ao'] == level, 'Expected same-state Off/Strong'
        assert not request['no_grounding'] and request['ao_cycle'] is None, 'Placement override enabled'
        assert not request.get('no_lawn_lift', False), 'Lawn lift disabled'
        assert not request.get('no_connector_lift', False), 'Connector lift disabled'
    off, on = (read(path, 'geometry.jsonl') for path in (args.off, args.on))
    assert off and len(off) == len(on), 'Missing or mismatched geometry captures'
    report = dict(meshes=len(on), unchanged_dynamic_meshes=0, unchanged_player_meshes=0,
                  player_slots=[], static_scenery_changes=[], unchanged_logical_positions=0)
    key = ('tick', 'model', 'name', 'objectIndex', 'player')
    identity = lambda mesh: tuple(mesh[k] for k in key)
    # Equal-depth opaque batches need not have the same submission ordering.
    assert len({identity(m) for m in off}) == len(off), 'Duplicate mesh identity'
    assert len({identity(m) for m in on}) == len(on), 'Duplicate mesh identity'
    for a, b in zip(sorted(off, key=identity), sorted(on, key=identity)):
        assert identity(a) == identity(b), (identity(a), identity(b), 'Scene/tick mismatch')
        assert a['pos'] == b['pos'], (a['name'], 'logical position changed')
        report['unchanged_logical_positions'] += 1
        # The separately registered static tree is not in the foliage registry.
        static_tree_base = b['name'] in ('b01_m240', 'r_ashi', 'l_ashi') and b['cenv'] == 0
        if b['player'] >= 0 or (not b['aoScenery'] and not static_tree_base):
            assert a['vertices'] == b['vertices'], (a['name'], 'dynamic placement changed')
            report['unchanged_dynamic_meshes'] += 1
            if b['player'] >= 0:
                report['unchanged_player_meshes'] += 1
                report['player_slots'].append(b['player'])
        elif a['vertices'] != b['vertices']:
            if b['name'] == 'start':
                # The user-requested surface lift changes draw vertices only,
                # never the path's parent matrix or the actors above it.
                for p, q in zip(a['vertices'], b['vertices']):
                    assert abs(p[0]-q[0]) < .002 and abs(p[2]-q[2]) < .002
                    assert 8.6 < q[1]-p[1] < 8.7
            report['static_scenery_changes'].append((b['model'], b['name'], b['objectIndex']))
    report['player_slots'] = sorted(set(report['player_slots']))
    assert report['player_slots'] == [0, 1, 2, 3], report
    a, b = (read(path, 'spaces.jsonl') for path in (args.off, args.on))
    assert a and a == b and all(not s['corrected'] for s in b), 'Space placement changed'
    report['unchanged_authored_space_matrices'] = len(b)
    (args.on / 'placement-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
