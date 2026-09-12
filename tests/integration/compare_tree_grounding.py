"""Compare same-state W01 captures with only --no-tree-grounding changed."""
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from compare_ao_captures import frame
from measure_board_ground import triangles, height


def compare(before, after):
    old=[json.loads(s) for s in (before/'geometry.jsonl').read_text().splitlines()]
    new=[json.loads(s) for s in (after/'geometry.jsonl').read_text().splitlines()]
    assert len(old)==len(new)
    floor=[t for m in old if m['name'] in ('haikei10','haikei11','haikei17') for t in triangles(m)]
    report=dict(unchanged_meshes=0,unchanged_positions=0,bases=[])
    for a,b in zip(old,new):
        assert tuple(a[k] for k in ('tick','model','objectIndex'))==tuple(b[k] for k in ('tick','model','objectIndex'))
        assert a['pos']==b['pos']
        report['unchanged_positions']+=1
        av,bv=np.array(a['vertices']),np.array(b['vertices'])
        changed=np.any(av!=bv,axis=1)
        if not changed.any():
            report['unchanged_meshes']+=1
            continue
        assert a['name'] in ('b01_m240','r_ashi','l_ashi')
        assert np.max(np.abs(av[:,[0,2]]-bv[:,[0,2]]))<.002
        assert np.all(bv[changed,1]<av[changed,1])
        gaps=[]
        for p,q in zip(av[changed],bv[changed]):
            hits=[h for t in floor if (h:=height(t,*p[[0,2]])) is not None and h<=p[1]+.5 and p[1]-h<32]
            assert hits
            support=max(hits)
            gaps.append([p[1]-support,q[1]-support])
        gaps=np.array(gaps)
        assert np.all((gaps[:,0]>.5)&(gaps[:,0]<32))
        assert np.max(np.abs(gaps[:,1]+.1))<.003
        report['bases'].append(dict(name=a['name'],object=a['objectIndex'],vertices=int(changed.sum()),
            before_gap_range=[float(gaps[:,0].min()),float(gaps[:,0].max())],
            after_gap_range=[float(gaps[:,1].min()),float(gaps[:,1].max())],
            unchanged_vertices=int((~changed).sum())))
    assert len(report['bases'])==4,report
    (after/'tree-grounding-metrics.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    captures=[frame(sorted((p/'frames').glob('*.mfd'))[0]) for p in (before,after)]
    assert captures[0][0]==captures[1][0]
    assert captures[0][1].shape==captures[1][1].shape==(1080,1920,3)
    # Native framebuffer samples of the slide-end base and tree-ground seam.
    boxes=[(1210,260,1800,530),(400,285,990,555)]
    canvas=Image.new('RGB',(1210,600),(24,27,33))
    draw=ImageDraw.Draw(canvas)
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',20)
    draw.text((10,5),'Before - GTAO Strong',font=font,fill='white')
    draw.text((610,5),'After - GTAO Strong',font=font,fill='white')
    for row,(x0,y0,x1,y1) in enumerate(boxes):
        for col,(_,pixels) in enumerate(captures):
            canvas.paste(Image.fromarray(pixels[y0:y1,x0:x1]),(10+col*600,35+row*280))
    canvas.save(after/'tree-grounding-before-after.png')


if __name__=='__main__':
    compare(*map(Path,sys.argv[1:3]))
