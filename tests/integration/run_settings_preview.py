"""Capture the actual game frame inside the menu, isolated from the user session."""
from pathlib import Path
import os
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags

base = ROOT / 'build/settings-qa-preview'
exe = base / 'release/mp6native.exe'
original_aspect = '--original-aspect' in sys.argv
out = base / ('captures-original-aspect' if original_aspect else 'captures')
out.mkdir(exist_ok=True)
# Interactive mode deliberately reads VSync from config, not MP6_VSYNC. Use
# this QA executable's own config so its hidden swapchain is not FIFO-throttled.
(exe.parent / 'mp6_config.json').write_text(json.dumps({'video.vsync': False}), encoding='utf-8')
env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
env.update(MP6_LAUNCHER='1', MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
           MP6_GPU_CACHE_PATH=str(out / 'gpu-cache'), MP6_WINDOW_SIZE='1440x900',
           MP6_BOOT_TO='mdparty', MP6_AUTO_START_TICKS='60,150',
           # Pace simulation explicitly, not through the hidden swapchain.
           # An unbounded run can hit 1800 while startup's async reads are pending.
           MP6_TICK_HZ='60', MP6_VSYNC='0', MP6_UNLOCKED_FPS='0',
           MP6_ENH_AMBIENT_OCCLUSION='0', MP6_ENH_AA='2',
           MP6_FRAME_DUMP=str(out / 'frames'), MP6_FRAME_DUMP_COUNT='2',
           MP6_FRAME_DUMP_DELAY='1830', MP6_FRAME_DUMP_STRIDE='60')
if original_aspect:
    env.update(MP6_ENH_WIDESCREEN='0', MP6_WIDESCREEN='0')
isolate_background(env, exe)
with (out / 'run.log').open('w') as log:
    result = subprocess.run([str(exe), '2250', '--input-script',
                             'period:30;timeout:2400;pressuntil:a/qa.complete/done'],
                            cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
                            timeout=180, creationflags=background_process_flags())
text = (out / 'run.log').read_text(errors='replace')
assert '[SETTINGS-QA] live game preview opened' in text, text[-4000:]
for check in ['in-game settings play linked-disc music', 'in-game settings select an approved idle track',
              'game audio muted only during menu music',
              'closing settings releases menu music', 'closing settings restores the current master volume']:
    assert '[SETTINGS-QA] PASS ' + check in text, text[-6000:]
assert text.count('[MENU-MUSIC] linked disc: 6 idle tracks') == 2, 'idle music did not reopen cleanly'
assert result.returncode == 0, text[-4000:]
assert '[AURORA ERROR' not in text and 'Validation Error' not in text, text[-4000:]
import struct
from PIL import Image, ImageChops, ImageStat
for name, tick, expected_window in [('live-preview-wide', 1830, (1440, 900)), ('live-preview', 1920, (1280, 960)),
                                  ('live-preview-ultrawide', 2020, (1840, 790)), ('live-preview-720p', 2120, (1280, 720)),
                                  ('live-preview-hidpi', 2220, (2560, 1440))]:
    capture=Image.open(out / f'{name}.ppm')
    capture.save(out / f'{name}.png')
    sizes = re.search(rf'live-size {tick} window=(\d+)x(\d+) presentation=(\d+)x(\d+)', text)
    assert sizes, f'{name}: missing current-viewport evidence'
    ww, wh, pw, ph = map(int, sizes.groups())
    if original_aspect and tick == 1830:
        expected_window = (1200, 900)  # Initial game aspect lock, before the explicit resize sweep.
    assert (ww, wh) == expected_window, f'{name}: actual window did not resize: {(ww, wh)}'
    assert capture.size == (pw, ph) == (ww, wh), f'{name}: menu is not full-window: {capture.size}'
    if original_aspect and tick == 2020:
        scene = re.search(rf'scene-size {tick} (\d+)x(\d+)', text)
        assert scene and int(scene[1])/int(scene[2]) < 1.5, 'original game aspect was not preserved'
    print(f'{name}: window {ww}x{wh}, current presentation {pw}x{ph}')
    w, h = capture.size
    bounds = re.search(rf'preview-layout {tick} mount=([\d.,-]+) caption=([\d.,-]+) gap=([\d.]+)', text)
    assert bounds, f'{name}: missing live layout bounds'
    mx, my, mw, mh = map(float, bounds[1].split(','))
    cx, cy, cw, ch = map(float, bounds[2].split(','))
    assert float(bounds[3]) >= 14*min(w/1672,h/941)+1.4, f'{name}: caption overlaps curved mount'
    # Verify the gap in rendered pixels too: no borrowed scene or black caption
    # ink may escape their measured areas into this strip of ivory paper.
    scale = min(w/1672, h/941)
    ox, oy = (w-1672*scale)/2, (h-941*scale)/2
    gap = capture.crop((round(ox+(cx+4)*scale), round(oy+(my+mh)*scale)+3,
                        round(ox+(cx+cw-4)*scale), round(oy+cy*scale)-2)).convert('RGB')
    assert gap.width>0 and gap.height>0, f'{name}: collapsed photo/caption gap'
    assert min(channel[0] for channel in gap.getextrema())>140, f'{name}: image or caption bleeds into paper gap'
    # Interior of the fitted, warped photo at each current aspect ratio. A lost
    # scene texture or GX destination alpha as coverage makes this mount blank.
    photo=capture.crop((int(ox+1120*scale),int(oy+235*scale),int(ox+1305*scale),int(oy+325*scale)))
    stats=ImageStat.Stat(photo)
    assert max(stats.stddev)>18 and min(stats.mean)<220, f'{name}: live scene photo is blank'
# Compare the original tick-1830 capture with the extra tick-1840 capture.
# Both are before the first resize, so any interior change is borrowed scene
# freshness rather than a viewport/layout change.  The ROI excludes the photo
# frame and caption, and the low threshold tolerates normal GPU quantization
# while rejecting a stale identical texture.
refresh = Image.open(out / 'live-preview-refresh.ppm')
wide = Image.open(out / 'live-preview-wide.ppm')
assert refresh.size == wide.size, f'live refresh changed viewport: {wide.size} -> {refresh.size}'
refresh_log = re.findall(
    r'\[SETTINGS-QA\] live-refresh tick=1840 viewport=(\d+)x(\d+) '
    r'geometry_reuses=(\d+)->(\d+) render_calls=(\d+)->(\d+)', text)
assert len(refresh_log) == 1, 'missing unique same-viewport retained-render evidence'
rw, rh, reuse_before, reuse_after, render_before, render_after = map(int, refresh_log[0])
assert (rw, rh) == wide.size, f'live refresh log viewport differs: {(rw, rh)} vs {wide.size}'
assert reuse_after > reuse_before, f'live refresh geometry was not reused: {reuse_before}->{reuse_after}'
assert render_after >= render_before + 1, f'live refresh did not render a second frame: {render_before}->{render_after}'

def photo_interior(capture):
    w, h = capture.size
    scale = min(w/1672, h/941)
    ox, oy = (w-1672*scale)/2, (h-941*scale)/2
    return capture.crop((round(ox+1120*scale), round(oy+235*scale),
                         round(ox+1305*scale), round(oy+325*scale))).convert('RGB')

photo_delta = ImageChops.difference(photo_interior(wide), photo_interior(refresh))
delta_pixels = photo_delta.width * photo_delta.height
changed = sum(max(pixel) >= 4 for pixel in photo_delta.getdata())
mean_delta = max(ImageStat.Stat(photo_delta).mean)
assert changed >= max(32, int(delta_pixels * .005)) and mean_delta > .5, \
    f'live photo texture did not advance: {changed}/{delta_pixels} pixels, mean delta {mean_delta:.3f}'
print(f'Live preview freshness passed at {wide.size[0]}x{wide.size[1]}: '
      f'photo changed in {changed}/{delta_pixels} interior pixels (mean delta {mean_delta:.3f}); '
      f'geometry reuse {reuse_before}->{reuse_after}, render calls {render_before}->{render_after}.')
print('Live preview stayed on the paper with a clear caption gap at five viewport/UI scales; captures:', out)
print('Linked-disc music plays in-game, releases on close, restores master volume, and reopens cleanly.')
for path in (out / 'frames').glob('*.mfd'):
    data = path.read_bytes()
    width, height = struct.unpack_from('<II', data, 12)
    fmt, = struct.unpack_from('<I', data, 24)
    Image.frombytes('RGBA', (width, height), data[96:], 'raw',
                    'BGRA' if fmt in (3, 4) else 'RGBA').save(path.with_suffix('.png'))
