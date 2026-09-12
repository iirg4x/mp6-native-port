"""Verify a uniform landscape lift and unchanged dynamic placement, from captures."""
import json
from pathlib import Path
import sys
import numpy as np

BACKGROUND={'haikei1','haikei10','haikei11','haikei12','haikei14',
            'haikei15','haikei16','haikei17','haikei2','grid309'}


def check(before,after):
    runs=[json.loads((p/'result.json').read_text()) for p in (before,after)]
    assert runs[0]['sha256']==runs[1]['sha256']
    assert runs[0]['request']['state_path']==runs[1]['request']['state_path']
    assert all(r['exit_code']==0 and not r['crash_marker'] for r in runs)
    assert runs[0]['request']['ao']==0 and runs[1]['request']['ao']==2
    scenes=[[json.loads(s) for s in (p/'geometry.jsonl').read_text().splitlines()] for p in (before,after)]
    key=lambda m:(m['model'],m['objectIndex'],m['name'])
    old,new=({key(m):m for m in scene} for scene in scenes)
    assert old.keys()==new.keys()
    pairs=[]; changes={}; dynamics=0
    for k,a in old.items():
        b=new[k]
        assert a['pos']==b['pos'] and a['tick']==b['tick']
        if a['player']>=0:
            assert a['vertices']==b['vertices']
            dynamics+=1
        if a['name'] in BACKGROUND:
            av,bv=np.array(a['vertices']),np.array(b['vertices'])
            delta=bv-av
            assert np.abs(delta[:,[0,2]]).max()<.003
            assert np.all((delta[:,1]>15.12)&(delta[:,1]<15.15))
            assert np.ptp(delta[:,1])<.004, 'Terrain was stretched instead of translated'
            changes[a['name']]={'vertices':len(av),'lift_min':float(delta[:,1].min()),'lift_max':float(delta[:,1].max())}
            pairs.append((a['name'],av,bv))
    seams=[]
    for i,(an,av,al) in enumerate(pairs):
        for bn,bv,bl in pairs[i+1:]:
            distance=np.linalg.norm(av[:,None,:]-bv[None,:,:],axis=2)
            ai,bi=np.where(distance<.1)
            if len(ai):
                new_distance=np.linalg.norm(al[ai]-bl[bi],axis=1)
                assert np.max(np.abs(new_distance-distance[ai,bi]))<.003
                seams.append({'meshes':[an,bn],'paired_vertices':len(ai),'max_after_gap':float(new_distance.max())})
    spaces=[(p/'spaces.jsonl').read_text() for p in (before,after)]
    assert spaces[0]==spaces[1]
    # Frustum culling hides distant sections at some slide/landing angles.
    # Check every submitted terrain mesh, not a fixed number of visible tiles.
    assert {'haikei10','haikei17'}.issubset(changes), changes
    assert seams and dynamics
    result={'uniformly_raised_background':changes,'preserved_seams':seams,
            'unchanged_player_meshes':dynamics,'unchanged_logical_positions':len(old),
            'unchanged_space_matrices':len(spaces[0].splitlines())}
    (after/'terrain-check.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__': check(*map(Path,sys.argv[1:3]))
