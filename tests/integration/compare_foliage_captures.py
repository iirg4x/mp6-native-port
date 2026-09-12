"""Compare identical saved frames with/without the private foliage AO mask."""
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from compare_ao_captures import frame

before,after=map(Path,sys.argv[1:3])
color_comparison='--color' in sys.argv[3:]
a,b=[frame(sorted((p/'frames').glob('*.mfd'))[0]) for p in (before,after)]
assert a[0]==b[0] and a[1].shape==b[1].shape
delta=b[1].astype(np.int16)-a[1]
if not color_comparison:
    assert delta.min()>=-1, 'AO coverage may only remove darkening (allow one rounding step)'
old=[json.loads(s) for s in (before/'geometry.jsonl').read_text().splitlines()]
new=[json.loads(s) for s in (after/'geometry.jsonl').read_text().splitlines()]
assert len(old)==len(new)
for x,y in zip(old,new):
    assert (x['model'],x['name'],x['vertices'],x['pos'])==(y['model'],y['name'],y['vertices'],y['pos'])
points=set(map(tuple,np.argwhere((np.abs(delta) if color_comparison else delta).max(axis=2)>2)))
groups=[]
while points:
    seed=points.pop(); group=[seed]; stack=[seed]
    while stack:
        y,x=stack.pop()
        for dy in (-1,0,1):
            for dx in (-1,0,1):
                p=(y+dy,x+dx)
                if p in points: points.remove(p); group.append(p); stack.append(p)
    if len(group)>2:
        p=np.array(group)
        groups.append(dict(pixels=len(group),bounds=[int(p[:,1].min()),int(p[:,0].min()),int(p[:,1].max()+1),int(p[:,0].max()+1)]))
groups.sort(key=lambda g:g['pixels'],reverse=True)
report=dict(tick=a[0],unchanged_meshes=len(old),max_brightening=int(delta.max()),
            max_darkening=int(-delta.min()),color_comparison=color_comparison,regions=groups)
(after/'foliage-metrics.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
if groups:
    image=Image.new('RGB',(840,40+len(groups[:3])*230),(24,27,33))
    draw=ImageDraw.Draw(image); font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',20)
    draw.text((10,5),'Before — GTAO Strong',fill='white',font=font)
    draw.text((430,5),'After — GTAO Strong',fill='white',font=font)
    for row,g in enumerate(groups[:3]):
        x0,y0,x1,y1=g['bounds']; cx=(x0+x1)//2; cy=(y0+y1)//2
        left=max(0,min(a[1].shape[1]-400,cx-200)); top=max(0,min(a[1].shape[0]-210,cy-105))
        for col,capture in enumerate((a,b)):
            image.paste(Image.fromarray(capture[1][top:top+210,left:left+400]),(10+420*col,40+row*230))
    image.save(after/'foliage-before-after.png')
