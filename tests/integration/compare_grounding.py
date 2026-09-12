"""Verify matched geometry and make a focused before/after grounding sheet."""
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont
from measure_board_ground import triangles, height

before,after=map(Path,sys.argv[1:3])
old=[json.loads(s) for s in (before/'geometry.jsonl').read_text().splitlines()]
new=[json.loads(s) for s in (after/'geometry.jsonl').read_text().splitlines()]
assert len(old)==len(new)
terrain={'haikei10','haikei11','haikei17','grid309','sora1','sora2','sora3','kutinaka','kirikabu'}
stats={'unchanged_terrain_meshes':0,'changed_meshes':0,'unchanged_logical_positions':0,'contacts':{}}
for a,b in zip(old,new):
    assert (a['tick'],a['model'],a['name'])==(b['tick'],b['model'],b['name'])
    assert a['pos']==b['pos'],a['name']
    stats['unchanged_logical_positions']+=1
    if a['name'] in terrain:
        assert a['vertices']==b['vertices'],a['name']
        stats['unchanged_terrain_meshes']+=1
    elif a['vertices']!=b['vertices']:
        stats['changed_meshes']+=1
    if a['name'] in ('luigi_m2','mario_m2','yoshi_m2','foot_R'):
        pair=[]
        for scene,m in ((old,a),(new,b)):
            foot=min(m['vertices'],key=lambda v:v[1])
            hits=[]
            for ground in scene:
                if ground['name'] not in ('start','haikei17'): continue
                for tri in triangles(ground):
                    y=height(tri,foot[0],foot[2])
                    if y is not None and y<foot[1]+.5: hits.append(y)
            pair.append(round(foot[1]-max(hits),4))
        stats['contacts'][a['name']]={'before_gap':pair[0],'after_gap':pair[1]}
print(json.dumps(stats,indent=2))
(after/'grounding-metrics.json').write_text(json.dumps(stats,indent=2)+'\n')
images=[Image.open(p/'frames/f000000.png').convert('RGB') for p in (before,after)]
boxes=[('Luigi / ground contact',(1520,1350,1860,1660)),
       ('Flowers / stem contact',(350,965,930,1385)),
       ('Brighton / ground contact',(1770,995,2060,1380))]
sheet=Image.new('RGB',(1160,1140),(23,27,34))
draw=ImageDraw.Draw(sheet)
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',22)
y=0
for label,box in boxes:
    draw.text((16,y+4),label,fill='white',font=font)
    y+=38
    for i,img in enumerate(images):
        crop=img.crop(box)
        crop.thumbnail((570,300))
        sheet.paste(crop,(i*580+5,y+28))
        draw.text((i*580+10,y),'Before' if i==0 else 'After',fill=(215,220,230),font=font)
    y+=335
sheet.crop((0,0,1160,y)).save(after/'grounding-before-after.png')
