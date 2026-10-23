"""CPU-only registration of the isolated lateral-camera plant captures.

Uses the captured installed camera, not the output aspect, and the corrected
receiver plane. Reports display-RGB changes, not physical AO visibility or a
general visual acceptance score. No GPU/native work or writes on import.
"""
import argparse
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build/ao-gpu-test-deps'))
import numpy as np


class Camera:
    def __init__(self, record, output_size):
        self.record = dict(record)
        self.width, self.height = output_size
        self.matrix = np.asarray(record['viewMatrix'], dtype=np.float64).reshape(3, 4)
        self.tan_y = math.tan(math.radians(record['fov']) * .5)
        self.aspect = record['aspect']
        if (self.width < 2 or self.height < 2 or not 0 < record['fov'] < 179
                or not np.isfinite(self.matrix).all() or self.aspect <= 0
                or not math.isfinite(self.aspect)):
            raise ValueError('invalid camera')
        # These captures cover the entire EFB. Sub-viewports need the full
        # logical EFB dimensions, which older capture records do not contain.
        x, y, w, h = record['viewport']
        if x != 0 or y != 0 or w <= 0 or h <= 0:
            raise ValueError('only full-EFB viewport captures are supported')
        self.inverse = np.linalg.inv(self.matrix[:, :3])

    def shifted_x(self, delta):
        matrix = self.matrix.copy()
        matrix[:, 3] -= matrix[:, :3] @ np.array([delta, 0., 0.])
        record = dict(self.record, viewMatrix=matrix.reshape(-1).tolist())
        return Camera(record, (self.width, self.height))

    def project(self, world):
        q = np.asarray(world) @ self.matrix[:, :3].T + self.matrix[:, 3]
        depth = -q[..., 2]
        u = (q[..., 0] / (depth * self.tan_y * self.aspect) + 1.) * self.width / 2. - .5
        v = (1. - q[..., 1] / (depth * self.tan_y)) * self.height / 2. - .5
        return u, v, depth

    def world_on_plane(self, u, v, plane_y):
        x = (np.asarray(u) + .5) / self.width * 2. - 1.
        y = 1. - (np.asarray(v) + .5) / self.height * 2.
        # y is already camera-up. Negating it here reflects the image.
        rays = np.stack((x * self.tan_y * self.aspect, y * self.tan_y,
                         -np.ones_like(x)), axis=-1) @ self.inverse.T
        origin = -self.inverse @ self.matrix[:, 3]
        with np.errstate(divide='ignore', invalid='ignore'):
            distance = (plane_y - origin[1]) / rays[..., 1]
        if not np.isfinite(distance).all() or np.any(distance <= 0):
            raise ValueError('sample rays do not intersect the receiver in front of the camera')
        return origin + rays * distance[..., None]


def bilinear(image, u, v):
    height, width = image.shape[:2]
    x, y = np.floor(u).astype(int), np.floor(v).astype(int)
    valid = (x >= 0) & (y >= 0) & (x < width-1) & (y < height-1)
    x, y = np.clip(x, 0, width-2), np.clip(y, 0, height-2)
    fx, fy = u-np.floor(u), v-np.floor(v)
    result = np.zeros(u.shape + (3,), dtype=np.float64)
    for dx, dy in ((0,0), (1,0), (0,1), (1,1)):
        weight = (fx if dx else 1-fx) * (fy if dy else 1-fy)
        result += image[y+dy, x+dx] * weight[..., None]
    return result, valid


def motion_stats(values, common):
    signal = np.stack(values)[:, common]
    changes = np.abs(np.diff(signal, axis=0))
    temporal = np.std(signal, axis=0)
    return dict(mean=float(signal.mean()), p95=float(np.quantile(signal, .95)),
                std_mean=float(temporal.mean()), std_p95=float(np.quantile(temporal, .95)),
                step_p95=float(np.quantile(changes, .95)), step_max=float(changes.max()))


def analyze(root, camera_record, plane_y, roi, indices, reference=30):
    from compare_ao_captures import frame
    names = ('unshaded', 'legacy', 'ultra3', 'half-ultra3')
    runs = {name: root / f'leaf-camera-fixed-{name}-20261005' for name in names}
    def read(name, index):
        return frame(runs[name] / 'frames' / f'f{index:06d}.mfd')
    ticks0 = {name: read(name, 0)[0] for name in names}
    ref_tick, ref_image = read('unshaded', reference)
    height, width = ref_image.shape[:2]
    captured = Camera(camera_record, (width, height))
    # This fixture starts its sinusoidal translation at tick 3000 in the
    # paced camera/geometry capture. The older unpaced image runs instead use
    # their recorded trigger tick. Each run is aligned by relative tick.
    captured_phase = camera_record['tick'] - 3000
    def camera_at(relative_tick):
        delta = 4. * (math.sin(math.radians(relative_tick * 3.))
                      - math.sin(math.radians(captured_phase * 3.)))
        return captured.shifted_x(delta)
    x0, y0, x1, y1 = roi
    yy, xx = np.mgrid[y0:y1:2, x0:x1:2]
    ref_camera = camera_at(ref_tick - ticks0['unshaded'])
    world = ref_camera.world_on_plane(xx, yy, plane_y)
    u, v, _ = ref_camera.project(world)
    roundtrip = float(max(np.max(np.abs(u-xx)), np.max(np.abs(v-yy))))
    if roundtrip > 1e-6:
        raise ValueError('projection roundtrip failed; discard all motion metrics')
    rgb_samples = {name: [] for name in names}
    validity = []
    for i in indices:
        relative_tick = None
        for name in names:
            tick, image = read(name, i)
            if image.shape != ref_image.shape:
                raise ValueError('capture sizes differ')
            phase = tick - ticks0[name]
            if relative_tick is not None and phase != relative_tick:
                raise ValueError('camera phases differ across variants')
            relative_tick = phase
            u, v, depth = camera_at(phase).project(world)
            sampled, valid = bilinear(image, u, v)
            rgb_samples[name].append(sampled)
            validity.append(valid & (depth > 0))
    common = np.logical_and.reduce(validity)
    unshaded = np.stack(rgb_samples['unshaded'])
    raster_common = int(common.sum())
    # The introduction logo grows over this ground during the capture. Use
    # only the neutral control to reject covered/changing samples; never
    # reject a pixel because its AO signal varies. A two-code RGB range
    # allows quantization and bilinear sampling of the stable lawn texture.
    common &= np.max(np.ptp(unshaded, axis=0), axis=-1) <= 2.
    if common.sum() < 100:
        raise ValueError('insufficient neutral-stable receiver samples')
    luminance = np.array([.2126, .7152, .0722])
    output = dict(scope='Native visible-ground ROI with animated occluders; not isolated sampling noise or whole-plant acceptance',
                  roi=list(roi), frames=list(indices), samples=int(common.sum()),
                  receiver_y=plane_y, camera=camera_record, roundtrip_max_pixels=roundtrip)
    output['neutral_mask'] = dict(rule='Each neutral RGB channel range <= 2 display codes',
                                 raster_common=raster_common, retained=int(common.sum()),
                                 limitation='May exclude genuine moving geometry; not a whole-plant metric')
    output['neutral_world_motion_rgb'] = motion_stats(
        list(unshaded @ luminance), common)
    # Retain signed differences; clamping would conceal registration defects.
    for name in names[1:]:
        signed_darkening = (unshaded - np.stack(rgb_samples[name])) @ luminance
        output[name] = motion_stats(list(signed_darkening), common)
        output[name]['units'] = 'signed luma difference, 0..255 display units'
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT/'build/board-qa-runs')
    parser.add_argument('--roi', type=int, nargs=4, default=(300, 385, 980, 490))
    parser.add_argument('--first-frame', type=int, default=20,
                        help='Exclude the introductory screen fade before comparison')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.first_frame < 59:
        parser.error('--first-frame must be between 1 and 58')
    x0, y0, x1, y1 = args.roi
    if not (0 <= x0 < x1 and 0 <= y0 < y1):
        parser.error('--roi requires nonnegative increasing x/y bounds')
    capture = args.root / 'leaf-ground-camera-20261005'
    camera = json.loads((capture/'geometry-camera.jsonl').read_text().splitlines()[0])
    from measure_board_ground import triangles, height
    receivers = []
    for line in (capture/'geometry.jsonl').read_text().splitlines():
        row = json.loads(line)
        if row.get('name') == 'haikei17':
            for triangle in triangles(row):
                receiver = height(triangle, -333., 3073.)
                if receiver is not None:
                    receivers.append(float(receiver))
    if len(receivers) != 1:
        raise ValueError('missing or ambiguous corrected receiver')
    result = analyze(args.root, camera, receivers[0], args.roi, range(args.first_frame, 60))
    formatted = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(formatted)
    print(formatted)


if __name__ == '__main__':
    main()
