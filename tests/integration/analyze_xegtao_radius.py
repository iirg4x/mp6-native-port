"""QA-only radius tradeoff on the fixed Treetop mesh and synthetic shoe contacts.

Consumes the captured-normal diagnostic's saved software depth, without changing
native shaders or production settings. The tree is two-sided opaque software
coverage, not native color/depth acceptance. Shoe contact is a separate synthetic
control in world units; its results are not a visual target or mobile benchmark.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
from test_xegtao_gpu import Renderer, np, feet_scene, plane, _validated_radius


def segment_band(shape, start, end, width, trim=.05):
    y, x = np.mgrid[:shape[0], :shape[1]]
    dx, dy = end[0]-start[0], end[1]-start[1]
    square = dx*dx + dy*dy
    if square <= 0 or width <= 0 or not 0 <= trim < .5:
        raise ValueError("invalid segment band")
    along = ((x-start[0])*dx + (y-start[1])*dy) / square
    across = np.abs((x-start[0])*dy - (y-start[1])*dx) / square**.5
    return (along >= trim) & (along <= 1-trim) & (across <= width)


def stats(values, mask):
    selected = values[mask]
    if not selected.size or not np.isfinite(selected).all():
        raise ValueError("empty or non-finite diagnostic mask")
    return dict(pixels=int(selected.size), mean=float(selected.mean()),
                median=float(np.median(selected)), p90=float(np.percentile(selected,90)),
                p95=float(np.percentile(selected,95)), maximum=float(selected.max()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path, help="normal-comparison directory")
    parser.add_argument("--radii", type=float, nargs="+", default=[8,16,24,32,48])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    radii = [_validated_radius(r) for r in args.radii]
    if len(radii)>16 or len(set(radii))!=len(radii) or 32.0 not in radii:
        parser.error("supply up to 16 unique radii including the reference 32")
    source = args.fixture / "comparison.npz"
    provenance_path = args.fixture / "report.json"
    provenance = json.loads(provenance_path.read_text())
    camera = provenance["camera"]
    expected_view = [1,0,0,0,0,.982006431,-.188847393,-755.389526,
                     0,.188847393,.982006431,-4014.89551]
    if (provenance["model"]!=133 or provenance["size"]!=[1280,720]
            or camera["fov"]!=45 or abs(camera["aspect"]-1.59000015)>1e-7
            or camera["viewport"]!=[0,0,848,480]
            or not np.allclose(camera["viewMatrix"], expected_view, rtol=0, atol=1e-6)):
        raise ValueError("seam masks require the recorded fixed Treetop camera")
    with np.load(source) as saved:
        depth, owners = saved["depth"], saved["owners"]
        baseline = saved["reconstructed"]
    if depth.shape!=(720,1280) or owners.shape!=depth.shape or baseline.shape!=depth.shape:
        raise ValueError("invalid fixture shapes")
    body = (owners==1) & (depth>0)
    masks = {"body":body}
    for name,start,end in (("left",(263,151),(350,270)),("right",(1012,235),(1127,327))):
        masks[name+"_seam_3px"] = body & segment_band(depth.shape,start,end,3)
        masks[name+"_corridor_24px"] = body & segment_band(depth.shape,start,end,24)
    ground,_ = plane(512,288)
    shoes,contact = feet_scene(512,288)
    renderer = Renderer()
    results, images = [], {}
    for radius in radii:
        tree = renderer.render(depth, denoise_passes=2, aspect=camera["aspect"], radius=radius)
        if radius==32:
            # Reuse the existing exact diagnostic as a guard against silently
            # changing shaders, input handling, or the baseline parameter ABI.
            np.testing.assert_array_equal(tree.visibility, baseline)
        flat = renderer.render(ground, denoise_passes=2, radius=radius)
        feet = renderer.render(shoes, denoise_passes=2, radius=radius)
        result = dict(radius=radius, effective_radius=radius*1.457,
                      tree={name:stats(1-tree.visibility,mask) for name,mask in masks.items()},
                      contacts=stats(1-feet.visibility,contact),
                      contact_increment=stats(flat.visibility-feet.visibility,contact),
                      ground_control=stats(1-flat.visibility,contact))
        results.append(result)
        images[f"tree-r{radius:g}"] = tree.visibility
        images[f"contact-r{radius:g}"] = feet.visibility
    report = dict(scope=__doc__, source_npz_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  source_report_sha256=hashlib.sha256(provenance_path.read_bytes()).hexdigest(),
                  adapter=dict(renderer.adapter.info), quality="High", denoise_passes=2,
                  shader_sha256={name:hashlib.sha256(stage.code.encode()).hexdigest()
                                 for name,stage in renderer.stages.items()},
                  contact_fixture=dict(size=[512,288],plane_distance=400,shoe_axes=[9,7,15],
                                       minimum_ground_gap=1), results=results)
    args.output.mkdir(parents=True,exist_ok=True)
    from PIL import Image
    for name,values in images.items():
        Image.fromarray(np.clip(values*255,0,255).astype(np.uint8)).save(args.output/f"{name}.png")
    (args.output/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(results,indent=2))


if __name__=="__main__":
    main()
