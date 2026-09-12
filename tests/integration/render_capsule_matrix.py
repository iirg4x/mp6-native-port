"""Contact sheets and state deltas from completed capsule QA runs (no pass inference)."""
import argparse
import json
from pathlib import Path
import re
import struct
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix')
    parser.add_argument('group',choices=('use','place','land','pass','boo','special'))
    args=parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+',args.matrix): parser.error('simple matrix name required')
    directory=ROOT/'build/board-qa-runs'/args.matrix
    reports=json.loads((directory/'matrix.json').read_text())
    reports=sorted((r for r in reports if r['case'].startswith(args.group)),key=lambda r:r['case'])
    if not reports: parser.error('no completed cases in this group')
    sheet=Image.new('RGB',(4*320,len(reports)*210))
    draw=ImageDraw.Draw(sheet)
    summary=[]
    for row,report in enumerate(reports):
        run=directory.parent/(args.matrix+'-'+report['case'])
        frames=sorted((run/'frames').glob('*.mfd'))
        if args.group=='place': indices=[1,2,3,4]
        else: indices=[int((len(frames)-1)*f) for f in (.2,.4,.6,.85)]
        for column,index in enumerate(indices):
            if not frames: continue
            path=frames[min(index,len(frames)-1)]
            raw=path.read_bytes()
            assert raw[:8]==b'MP6FDUMP'
            width,height,bpp,fmt=struct.unpack_from('<IIII',raw,12)
            tick=struct.unpack_from('<Q',raw,40)[0]
            assert bpp==4 and len(raw)==96+width*height*4
            image=Image.frombytes('RGBA',(width,height),raw[96:],'raw','BGRA' if fmt in (3,4) else 'RGBA')
            image.thumbnail((320,180))
            x,y=column*320,row*210
            sheet.paste(image,(x,y))
            draw.text((x+3,y+184),f"{report['case']} {path.stem} tick {tick}",fill='white')
        log=(run/'game.log').read_text(errors='replace')
        blocks=re.findall(r'\[BOARD-QA\] (audit\.(?:begin|end)[^\n]*\n(?:\[BOARD-QA\] player=[^\n]*\n){4})',log)
        summary.append({'case':report['case'],'errors':report['evidence']['errors'],'states':blocks})
    output=directory/(args.group+'-contact-sheet.png')
    sheet.save(output)
    (directory/(args.group+'-states.json')).write_text(json.dumps(summary,indent=2)+'\n')
    print(output)

if __name__=='__main__': main()
