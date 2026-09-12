"""Check the full submitted space footprints against captured W01 terrain."""
import json
from pathlib import Path
import sys
from measure_board_ground import triangles, height

def check(directory):
    meshes=[json.loads(s) for s in (directory/'geometry.jsonl').read_text().splitlines()]
    floor=[t for m in meshes if m['name'] in ('haikei10','haikei11','haikei17') for t in triangles(m)]
    spaces=[json.loads(s) for s in (directory/'spaces.jsonl').read_text().splitlines()]
    assert floor and spaces, 'Missing actual terrain/space capture'
    changed=[]
    old_sinking=[]
    for s in spaces:
        m=s['matrix']; px,py,pz=s['pos']
        if not s.get('corrected',abs(m[7]-(py+3))>=.02):
            continue
        centre=[h for t in floor if (h:=height(t,px,pz)) is not None and py-32<=h<=py+.5]
        minimum=1000.; old_minimum=1000.
        for x in range(-100,101,10):
            for z in range(-100,101,10):
                wx=m[0]*x+m[2]*z+m[3]; wy=m[4]*x+m[6]*z+m[7]; wz=m[8]*x+m[10]*z+m[11]
                hits=[h for t in floor if (h:=height(t,wx,wz)) is not None and py-32<=h<=py+.5]
                assert hits, f'Space {s["id"]}: unsupported footprint at {x},{z}'
                minimum=min(minimum,wy-max(hits))
                if centre: old_minimum=min(old_minimum,max(centre)+.25-max(hits))
        assert minimum>=.47, f'Space {s["id"]} intersects terrain by {-minimum}'
        changed.append(dict(id=s['id'],min_clearance=minimum))
        if old_minimum<0: old_sinking.append(dict(id=s['id'],min_clearance=old_minimum))
    assert changed, 'No grounded spaces were exercised'
    result=dict(space_count=len(spaces),corrected=changed,centre_only_sinking=old_sinking)
    print(json.dumps(result,indent=2))
    return result

if __name__=='__main__': check(Path(sys.argv[1]))
