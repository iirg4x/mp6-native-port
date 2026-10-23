"""Run only the isolated menu harness, never a user's executable/config/window."""
from pathlib import Path
import os
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags


def main():
    base = ROOT / ('build/settings-qa-touch' if '--touch' in sys.argv else 'build/settings-qa')
    exe = base / 'release/mp6native.exe'
    output = base / 'captures'
    output.mkdir(exist_ok=True)
    # Asset-only changes must not silently capture the last build's artwork.
    resources = [*(ROOT / 'res/menu').rglob('*.png'), *(ROOT / 'res/rml').glob('*.rcss'),
                 ROOT / 'res/fonts/TitanOne-Regular.ttf']
    for asset in resources:
        staged = exe.parent / asset.relative_to(ROOT)
        if not staged.is_file() or hashlib.sha256(asset.read_bytes()).digest() != hashlib.sha256(staged.read_bytes()).digest():
            raise RuntimeError(f'Stale QA resource: {asset.name}; rebuild the settings QA executable')
    env = {key: value for key, value in os.environ.items() if not key.startswith('MP6_')}
    env.update(MP6_LAUNCHER='1', MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
               MP6_GPU_CACHE_PATH=str(output / 'gpu-cache'), MP6_WINDOW_SIZE='1440x900',
               MP6_VSYNC='0', MP6_ALLOW_CPU_ADAPTER='0')
    isolate_background(env, exe)
    # Only this isolated test executable's config; never the user's release.
    # Interactive launcher mode ignores MP6_VSYNC in favor of this preference.
    (exe.parent / 'mp6_config.json').write_text(json.dumps({'video.vsync': False}), encoding='utf-8')
    with (output / 'run.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([str(exe)], cwd=output, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=90,
                                creationflags=background_process_flags())
    text = (output / 'run.log').read_text(encoding='utf-8', errors='replace')
    for line in text.splitlines():
        if any(s in line for s in ['SETTINGS-QA', 'SETTINGS-PERF', 'BACKGROUND-QA', 'ERROR', 'WARN', 'Failed', 'Invalid']):
            print(line)
    from PIL import Image
    for capture in output.glob('*.ppm'):
        Image.open(capture).save(capture.with_suffix('.png'))
    errors = [line for line in text.splitlines() if any(word in line for word in
              ['Syntax error', '[AURORA ERROR', '[SETTINGS-QA] FAIL', 'Validation Error'])]
    if result.returncode or '[SETTINGS-QA] COMPLETE' not in text or errors:
        return result.returncode or 1
    from tests.integration.check_launcher_logo import check as check_launcher_logo
    check_launcher_logo(output)
    # A full-width book sprite used to repeat-sample its opposite (opaque
    # spine) texel, drawing a thin vertical line into the night sky.
    for name in ['launcher-linked', 'launcher-hidpi', 'launcher-4x3', 'launcher-ultrawide']:
        capture = Image.open(output / f'{name}.png').convert('RGB')
        w, h = capture.size
        scale = min(w/1672, h/941)
        edge = round((w-1672*scale)/2 + 1652*scale)
        for x in range(edge-2, min(edge+3,w-2)):
            spikes = sum(max(abs(2*b-a-c) for a,b,c in zip(capture.getpixel((x-2,y)),
                         capture.getpixel((x,y)),capture.getpixel((x+2,y)))) > 8
                         for y in range(round(h*.2),round(h*.7)))
            assert spikes < h*.15, f'{name}: repeat-sampler book edge at {x}: {spikes}'
    print('[SETTINGS-QA] No vertical book-texture bleed at four window sizes')
    # Early entrance exposed two full-height one-pixel strips: repeat filtering
    # sampled the opposite mask edge even after the ray polygons were clipped.
    intro = Image.open(output / 'native-intro-first-frame.png').convert('RGB')
    w, h = intro.size
    lit = [sum(max(intro.getpixel((x, y))) > 5 for y in range(h)) for x in range(w)]
    for x in range(2, w-2):
        assert not (lit[x] > h*.95 and lit[x-2] < h*.05 and lit[x+2] < h*.05), \
            f'wrapped sky-mask seam at x={x}'
    print('[SETTINGS-QA] Early-intro sky mask has no wrapped edge seams')
    portrait = Image.open(output / 'launcher-link-dialog-portrait.png').convert('RGB')
    for x in [2, portrait.width-3]:
        assert max(portrait.getpixel((x,portrait.height//2)))<210, 'link dialog paint clips the portrait viewport edge'
    notice = Image.open(output / 'ui-confirm-dismissed.png').convert('RGB')
    red = [x for y in range(500,650) for x in range(notice.width)
           if (lambda c: c[0]>180 and c[1]<60 and c[2]<60)(notice.getpixel((x,y)))]
    assert red and min(red)>=10 and max(red)<notice.width-10, 'controller notice is cropped in portrait'
    mask = Image.open(ROOT / 'res/menu/storybook/zigzag-mask.png').convert('RGBA')
    assert mask.size == (64,126) and mask.getextrema()[3] == (255,255), 'mask must be one opaque luminance tile'
    assert all(r==g==b for r,g,b,a in mask.getdata()), 'mask is not black/white luminance'
    assert mask.crop((0,0,64,1)).tobytes()==mask.crop((0,125,64,126)).tobytes(), 'mask vertical seam is not tileable'
    assert not (ROOT / 'res/menu/storybook/day-mask.png').exists(), 'redundant inverse mask remains'
    # All these frames are settled with motion off: no navy letterbox and no
    # day/night background leaking through the fold at any supported aspect.
    balloon_sizes = []
    for name in ['native-picture-720p', 'native-picture-4x3',
                 'native-picture-ultrawide', 'native-picture-hidpi']:
        picture = Image.open(output / f'{name}.png').convert('RGB')
        w, h = picture.size
        r, g, b = picture.getpixel((3, 3))
        assert r > 170 and g > 140, f'{name}: menu does not fill the viewport'
        scale = min(w / 1672, h / 941)
        offset_x, offset_y = (w-1672*scale)/2, (h-941*scale)/2
        for logical_y in range(160, 780, 16):
            for dx in [-1, 0, 1]:
                r, g, b = picture.getpixel((w // 2 + dx, round(offset_y + logical_y * scale)))
                assert r > 160 and g > 130 and b > 60, f'{name}: open spine at {logical_y}'
        # The bright paper contour must occupy the same uniformly scaled
        # coordinates, not expand independently toward the viewport edges.
        for logical_y in [350, 450, 550]:
            scan_y = round(offset_y + logical_y*scale)
            paper = [x for x in range(w) if (lambda c: c[0]>220 and c[1]>215 and c[2]>180)(picture.getpixel((x,scan_y)))]
            assert paper, f'{name}: missing paper contour'
            left, right = (min(paper)-offset_x)/scale, (max(paper)-offset_x)/scale
            assert 65 < left < 120 and 1552 < right < 1607, f'{name}: stretched book contour: {(left,right)}'
        # The orange balloon is isolated below the book in these motion-off frames
        # on both layouts (the fifth touch tab can occlude the upper blue balloon).
        # Its physical silhouette must scale uniformly, not with the page axes.
        import math
        scale = min(w / 1672, h / 941)
        cx = (375 + math.sin(4) * 7) * w / 1672
        cy = 898 * h / 941
        pixels = [(x, y, picture.getpixel((x, y)))
                  for y in range(max(0, int(cy-28*scale)), int(cy+15*scale))
                  for x in range(int(cx-17*scale), int(cx+17*scale))]
        colored = [(x, y, c) for x, y, c in pixels if c[0] > 230 and 90 < c[1] < 140 and c[2] < 140]
        assert colored, f'{name}: balloon disappeared'
        balloon_sizes.append(((max(p[0] for p in colored)-min(p[0] for p in colored)+1)/scale,
                              (max(p[1] for p in colored)-min(p[1] for p in colored)+1)/scale))
        assert len({p[2] for p in colored}) > 8, f'{name}: balloon edge has no coverage antialiasing'
    for axis in range(2):
        sizes = [b[axis] for b in balloon_sizes]
        assert max(sizes)-min(sizes) < 3.5, f'balloon distorted across window aspects: {balloon_sizes}'
    print('[SETTINGS-QA] Full-viewport and continuous-spine pixel checks passed at four resolutions')
    print('[SETTINGS-QA] Proportional balloon silhouette and antialias coverage passed at four resolutions')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
