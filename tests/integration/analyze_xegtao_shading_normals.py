"""Compare supplied HSF shading normals with depth normals on captured geometry.

This is an isolated opaque-mesh diagnostic, not a native renderer or acceptance
test. Software coverage is two-sided, single-sample, and ignores materials,
alpha tests, textures, and other models. Both AO runs use exactly the same
software depth; only the optional normal input changes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
from test_xegtao_gpu import Renderer, np, _decode_packed_normals
from analyze_ao_leaf_motion import Camera


def corner_triangles(kind, count):
    """Corner order from native HSF FaceDraw, including GX_QUADS remapping."""
    if kind == 2 and count == 3:
        yield (0, 2, 1)
    elif kind == 3 and count == 4:
        yield (0, 2, 3)
        yield (0, 3, 1)
    elif kind == 4 and count >= 3:
        strip = [0, 2, 1, *range(3, count)]
        for i in range(count - 2):
            yield tuple(strip[i:i + 3])
    else:
        raise ValueError(f"unsupported HSF face kind/count {kind}/{count}")


def clip_triangle(vertices, normals, near=20.0, far=10000.0):
    """Clip canonical +Z view positions, retaining linearly varying normals."""
    points = [(v, n) for v, n in zip(vertices, normals)]
    for plane, sign in ((near, 1.0), (far, -1.0)):
        result = []
        if not points:
            break
        before, normal_before = points[-1]
        before_in = sign * (before[2] - plane) >= 0
        for current, normal_current in points:
            current_in = sign * (current[2] - plane) >= 0
            if before_in != current_in:
                t = (plane - before[2]) / (current[2] - before[2])
                result.append((before + t * (current - before),
                               normal_before + t * (normal_current - normal_before)))
            if current_in:
                result.append((current, normal_current))
            before, normal_before, before_in = current, normal_current, current_in
        points = result
    for i in range(1, len(points) - 1):
        yield (np.asarray([points[j][0] for j in (0, i, i + 1)]),
               np.asarray([points[j][1] for j in (0, i, i + 1)]))


def rasterize(rows, camera, width, height):
    """Perspective-correct depth and smooth HSF normals for selected records."""
    depth = np.full((height, width), np.inf, dtype=np.float64)
    normalmap = np.zeros((height, width, 3), dtype=np.float64)
    normalmap[..., 2] = -1
    owners = np.full((height, width), -1, dtype=np.int32)
    triangles = 0
    for row in rows:
        if row.get("indexedNormalsAreFinal") is not True:
            raise ValueError(f"{row['name']}: indexed normals are not verified final shading normals")
        world = np.asarray(row["vertices"], dtype=np.float64)
        view = world @ camera.matrix[:, :3].T + camera.matrix[:, 3]
        view[:, 2] *= -1
        normals = np.asarray(row["viewNormals"], dtype=np.float64)
        if normals.ndim != 2 or normals.shape[1] != 3 or not np.isfinite(normals).all():
            raise ValueError(f"{row['name']}: no valid captured view normals")
        if np.any(np.linalg.norm(normals, axis=-1) <= 1e-10):
            raise ValueError(f"{row['name']}: zero-length captured view normal")
        normals = normals.copy()
        normals[:, 2] *= -1  # GX forward -Z -> XeGTAO canonical +Z.
        normal_faces = row["faceNormalIndices"]
        if len(row["faces"]) != len(normal_faces):
            raise ValueError("face/normal index lists differ")
        for face, n_indices in zip(row["faces"], normal_faces):
            kind, *v_indices = face
            if len(n_indices) != len(v_indices):
                raise ValueError("normal indices must match captured face corners")
            if any(i < 0 or i >= len(world) for i in v_indices):
                raise ValueError("out-of-range vertex index")
            if any(i < 0 or i >= len(normals) for i in n_indices):
                raise ValueError("out-of-range normal index")
            for corners in corner_triangles(kind, len(v_indices)):
                positions = view[[v_indices[i] for i in corners]]
                shading = normals[[n_indices[i] for i in corners]]
                for p, n in clip_triangle(positions, shading):
                    z = p[:, 2]
                    x = (p[:, 0] / (z * camera.tan_y * camera.aspect) + 1) * width * .5 - .5
                    y = (1 - p[:, 1] / (z * camera.tan_y)) * height * .5 - .5
                    x0, x1 = max(0, int(np.ceil(x.min()))), min(width - 1, int(np.floor(x.max())))
                    y0, y1 = max(0, int(np.ceil(y.min()))), min(height - 1, int(np.floor(y.max())))
                    if x0 > x1 or y0 > y1:
                        continue
                    det = (y[1]-y[2])*(x[0]-x[2]) + (x[2]-x[1])*(y[0]-y[2])
                    if abs(det) < 1e-12:
                        continue
                    yy, xx = np.mgrid[y0:y1+1, x0:x1+1]
                    a = ((y[1]-y[2])*(xx-x[2]) + (x[2]-x[1])*(yy-y[2])) / det
                    b = ((y[2]-y[0])*(xx-x[2]) + (x[0]-x[2])*(yy-y[2])) / det
                    c = 1-a-b
                    bary = np.stack((a, b, c), axis=-1)
                    weights = bary / z
                    reciprocal_z = weights.sum(axis=-1)
                    with np.errstate(divide="ignore", invalid="ignore"):
                        candidate_z = 1 / reciprocal_z
                        candidate_n = (weights @ n) / reciprocal_z[..., None]
                    length = np.linalg.norm(candidate_n, axis=-1)
                    valid = ((bary.min(axis=-1) >= -1e-9) &
                             (candidate_z <= depth[y0:y1+1, x0:x1+1]))
                    if np.any(valid & (~np.isfinite(length) | (length <= 1e-10))):
                        raise ValueError(f"{row['name']}: degenerate interpolated view normal")
                    if valid.any():
                        depth[y0:y1+1, x0:x1+1][valid] = candidate_z[valid]
                        normalmap[y0:y1+1, x0:x1+1][valid] = candidate_n[valid] / length[valid, None]
                        owners[y0:y1+1, x0:x1+1][valid] = row.get("objectIndex", -1)
                    triangles += 1
    depth[~np.isfinite(depth)] = 0
    return depth.astype(np.float32), normalmap.astype(np.float32), owners, triangles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="native capture directory")
    parser.add_argument("--model", type=int, default=133)
    parser.add_argument("--tick", type=int, required=True)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 32 <= args.width <= 4096 or not 32 <= args.height <= 4096:
        parser.error("dimensions must be 32..4096")
    cameras = [json.loads(s) for s in (args.run/"geometry-camera.jsonl").read_text().splitlines()]
    matches = [r for r in cameras if r["tick"] == args.tick and r["camera"] == 0]
    if len(matches) != 1:
        raise ValueError("missing/ambiguous camera record")
    camera = Camera(matches[0], (args.width, args.height))
    if abs(matches[0]["fov"] - 45) > 1e-5:
        raise ValueError("this diagnostic currently requires FOV45 to match GPU harness")
    source = args.run/"geometry.jsonl"
    rows = [json.loads(s) for s in source.read_text().splitlines()]
    rows = [r for r in rows if r["tick"] == args.tick and r["model"] == args.model]
    if not rows:
        raise ValueError("no selected model geometry")
    depth, normals, owners, count = rasterize(rows, camera, args.width, args.height)
    coverage = depth > 0
    if not coverage.any():
        raise ValueError("selected geometry has no visible pixels")
    renderer = Renderer()
    reconstructed = renderer.render(depth, quality="High", denoise_passes=2, aspect=camera.aspect)
    supplied = renderer.render(depth, quality="High", denoise_passes=2, normals=normals, aspect=camera.aspect)
    difference = supplied.visibility - reconstructed.visibility
    reconstructed_normals = _decode_packed_normals(reconstructed.packed_normals)
    reconstructed_normals /= np.linalg.norm(reconstructed_normals, axis=-1, keepdims=True)
    normal_dot = np.sum(normals * reconstructed_normals, axis=-1)
    output = dict(scope=__doc__, camera=matches[0], model=args.model, tick=args.tick,
                  triangles=count, pixels=int(coverage.sum()),
                  size=[args.width, args.height], quality="High", denoise_passes=2,
                  adapter=dict(renderer.adapter.info),
                  shader_sha256={name: hashlib.sha256(stage.code.encode()).hexdigest()
                                 for name, stage in renderer.stages.items()},
                  executable=json.loads((args.run/"result.json").read_text())["sha256"],
                  geometry_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  mean_visibility_depth=float(reconstructed.visibility[coverage].mean()),
                  mean_visibility_authored=float(supplied.visibility[coverage].mean()),
                  mean_normal_dot=float(normal_dot[coverage].mean()),
                  mean_abs_visibility_delta=float(np.abs(difference[coverage]).mean()),
                  p95_abs_visibility_delta=float(np.percentile(np.abs(difference[coverage]), 95)))
    output["per_object"] = []
    for object_id in np.unique(owners[coverage]):
        mask = coverage & (owners == object_id)
        before, after = 1-reconstructed.visibility[mask], 1-supplied.visibility[mask]
        output["per_object"].append(dict(
            object=int(object_id), pixels=int(mask.sum()),
            mean_occlusion_depth=float(before.mean()), mean_occlusion_authored=float(after.mean()),
            p95_occlusion_depth=float(np.percentile(before, 95)),
            p95_occlusion_authored=float(np.percentile(after, 95))))
    args.output.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output/"comparison.npz", depth=depth, normals=normals,
                        reconstructed_normals=reconstructed_normals, owners=owners,
                        reconstructed=reconstructed.visibility, authored=supplied.visibility)
    from PIL import Image
    for name, values in (("depth-normals", reconstructed_normals * .5 + .5),
                         ("authored-normals", normals * .5 + .5),
                         ("depth-ao", reconstructed.visibility),
                         ("authored-ao", supplied.visibility)):
        Image.fromarray(np.clip(values * 255, 0, 255).astype(np.uint8)).save(args.output/f"{name}.png")
    (args.output/"report.json").write_text(json.dumps(output, indent=2)+"\n")
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
