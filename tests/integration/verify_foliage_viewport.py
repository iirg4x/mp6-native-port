"""Same-state rendered regression for the W01 grass-strip AO viewport bug.

Arguments: BEFORE AFTER UNSHADED FOREGROUND run directories. All four must
restore one state with one QA executable; the last two are diagnostic passes,
not game screenshots with AO disabled (which would also change grounding).
"""
import json
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from compare_ao_captures import frame

runs=list(map(Path,sys.argv[1:5]))
assert len(runs)==4
captures=[frame(sorted((r/'frames').glob('*.mfd'))[0]) for r in runs]
assert len({c[0] for c in captures})==1
geometry=[[json.loads(s) for s in (r/'geometry.jsonl').read_text().splitlines()] for r in runs]
reference=[(m['model'],m['name'],m['vertices'],m['pos']) for m in geometry[0]]
for meshes in geometry[1:]:
    assert reference==[(m['model'],m['name'],m['vertices'],m['pos']) for m in meshes]
assert captures[0][1].shape==(1080,1920,3)
box=(1360,420,1920,720)
x0,y0,x1,y1=box
before,after,unshaded,foreground=[c[1][y0:y1,x0:x1].astype(np.int16) for c in captures]
opaque_grass=(np.abs(foreground-unshaded).max(axis=2)<=1) & (foreground[:,:,1]>foreground[:,:,0]+15)
assert opaque_grass.sum()>5000, 'Coverage must align with the actual visible grass, not displaced pixels'
old_error=np.abs(before-unshaded).max(axis=2)[opaque_grass]
new_error=np.abs(after-unshaded).max(axis=2)[opaque_grass]
assert (old_error>8).sum()>1000, 'Old viewport must reproduce the reported dark band'
assert new_error.max()<=1, 'Opaque grass must retain its unshaded RGB to one UNORM rounding step'
unprotected=foreground.max(axis=2)==0
assert np.abs(after-before).max(axis=2)[unprotected].max()<=1, 'Do not erase AO on nearby solid surfaces'
delta=after-before
report=dict(tick=captures[0][0],identical_meshes=len(reference),crop=list(box),
    protected_grass_pixels=int(opaque_grass.sum()),old_band_pixels=int((old_error>8).sum()),
    old_max_error=int(old_error.max()),new_max_error=int(new_error.max()),
    changed_pixels=int(np.any(delta,axis=2).sum()),max_brightening=int(delta.max()),max_darkening=int(-delta.min()))
(runs[1]/'viewport-regression.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
image=Image.new('RGB',(1120,342),(24,27,33)); draw=ImageDraw.Draw(image)
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',22)
for i,(label,capture) in enumerate(zip(('Before - AO Strong','After - AO Strong'),captures)):
    draw.text((i*560+12,5),label,font=font,fill='white')
    image.paste(Image.fromarray(capture[1]).crop(box),(i*560,42))
image.save(runs[1]/'grass-edge-comparison.png')
