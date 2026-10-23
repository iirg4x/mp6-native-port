"""Opt-in comparison of the upstream depth-MIP control; no GPU work on import."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
OFFSETS = (2.0, 3.3, 5.0, 6.0)
SCOPE = ("Synthetic world-registered leaf quality diagnostic only; no native "
         "motion acceptance, GPU timing, FPS or Android claim")


def raw_visibility(np, values):
    # Upstream stores working visibility divided by XE_GTAO_OCCLUSION_TERM_SCALE.
    # Undo that encoding before comparing it with the final denoised visibility.
    if not np.isfinite(values).all() or (values < 0).any() or (values > 255).any():
        raise ValueError("working AO is outside upstream R8_UINT encoding")
    return np.clip(values.astype(np.float32) * (1.5 / 255.0), 0.0, 1.0)


def motion_metrics(np, samples, common):
    """Measure fixed receiver points; never mix differently visible populations."""
    fields = np.asarray(samples, dtype=np.float64)
    common = np.asarray(common, dtype=bool)
    if fields.ndim != 3 or fields.shape[0] < 2 or fields.shape[1:] != common.shape:
        raise ValueError("expected at least two HxW registered fields and one common mask")
    if min(common.shape) < 5 or not common.any() or not np.isfinite(fields).all():
        raise ValueError("invalid or empty registered receiver fields")
    signal = 1.0 - fields[:, common]
    if (signal < -1e-7).any() or (signal > 1.0 + 1e-7).any():
        raise ValueError("visibility lies outside [0,1]")
    temporal = np.std(signal, axis=0)
    changes = np.abs(np.diff(signal, axis=0))
    region = common[2:-2, 2:-2].copy()
    filtered = np.zeros((fields.shape[0], *region.shape), dtype=np.float64)
    for dy in range(5):
        for dx in range(5):
            region &= common[dy:dy+region.shape[0], dx:dx+region.shape[1]]
            filtered += (1.0 - fields[:, dy:dy+region.shape[0], dx:dx+region.shape[1]]) / 25.0
    if not region.any():
        raise ValueError("no complete 5x5 receiver neighborhood")
    mean = float(signal.mean())
    return {
        "samples": int(common.sum()), "coherent_samples": int(region.sum()),
        "mean_darkening": mean,
        "p95_darkening": float(np.quantile(signal, .95)),
        "temporal_std_mean": float(temporal.mean()),
        "temporal_std_p95": float(np.quantile(temporal, .95)),
        "temporal_step_p95": float(np.quantile(changes, .95)),
        "temporal_step_max": float(changes.max()),
        "temporal_std_relative": float(temporal.mean() / max(mean, 1e-12)),
        "coherent_5x5_std_mean": float(np.std(filtered[:, region], axis=0).mean()),
        "coherent_5x5_step_max": float(np.abs(np.diff(filtered[:, region], axis=0)).max()),
    }


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args):
    if not args.run_gpu:
        raise ValueError("explicit --run-gpu opt-in required")
    if not 8 <= args.frames <= 32:
        raise ValueError("frames must be between 8 and 32")
    output = args.output.resolve()
    if (output / "metrics.json").exists():
        raise ValueError("choose a new output directory; existing evidence is preserved")
    os.environ["WGPU_BACKEND_TYPE"] = args.backend
    sys.path[:0] = [str(ROOT / "build/ao-gpu-test-deps"), str(ROOT / "tests/integration"),
                   str(ROOT / "tools")]
    import numpy as np
    from PIL import Image, ImageDraw
    from test_ao_leaf_motion import leaf_scene, world_samples
    from test_xegtao_gpu import Renderer
    from compile_xegtao import normalized_sha256, verify_upstream

    verify_upstream()
    renderer = Renderer()
    manifest_path = renderer.shader_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for entry in manifest["entries"]:
        if sha(renderer.shader_dir / entry["wgsl"]) != entry["wgslSha256"]:
            raise ValueError("WGSL does not match its manifest: " + entry["wgsl"])
    for name, expected in manifest["upstream"]["files"].items():
        if normalized_sha256(ROOT / "third_party/xegtao" / name) != expected:
            raise ValueError("upstream source does not match shader manifest: " + name)

    report = {
        "scope": SCOPE, "backend_requested": args.backend,
        "adapter": dict(renderer.adapter.info), "frames": args.frames,
        "offsets": OFFSETS, "size": [640, 360], "radius": 32.0,
        "strength": .35, "quality": "High", "denoise_passes": 2,
        "noise_index": 0, "shift_world": [-.7, .7],
        "raw_metric_scope": "working AO decoded with scale1.5 and clamped before metrics; not final visibility",
        "source_hashes": {str(path.relative_to(ROOT)): sha(path) for path in (
            Path(__file__), ROOT / "tests/integration/test_ao_leaf_motion.py",
            ROOT / "tests/integration/test_xegtao_gpu.py",
            ROOT / "src/gx/xegtao/parameters.hpp", manifest_path)},
        "shader_hashes": {entry["wgsl"]: entry["wgslSha256"] for entry in manifest["entries"]},
        "lifts": {},
    }
    output.mkdir(parents=True, exist_ok=True)
    plane_ranges = {}
    plane_depth = np.full((360, 640), 180.0, dtype=np.float32)
    plane_reference = renderer.render(plane_depth, quality="High", denoise_passes=2,
                                      reuse_resources=True).decoded_visibility.copy()
    for offset in OFFSETS:
        plane = renderer.render(plane_depth,
                                quality="High", denoise_passes=2, reuse_resources=True,
                                depth_mip_offset=offset).decoded_visibility
        # Match the existing upstream flat-plane smoke contract, not an
        # invented perfect-white requirement. Also require exact baseline
        # equality: offset tuning must not introduce new planar occlusion.
        interior = plane[2:-2, 2:-2]
        if (not np.isfinite(plane).all() or float(plane.min()) < 0 or float(plane.max()) > 1
                or float(np.quantile(interior, .05)) <= .90 or float(np.median(interior)) <= .97):
            raise ValueError("flat plane fails the upstream harness smoke contract")
        if not np.array_equal(plane, plane_reference):
            raise ValueError("offset changes flat-plane output from default")
        plane_ranges[str(offset)] = {
            "full_range": [float(plane.min()), float(plane.max())],
            "interior_p05_median_p95": np.quantile(interior, [.05, .5, .95]).tolist(),
            "exact_default_match": True,
        }
    report["flat_plane"] = plane_ranges
    for lift in (4.0, 10.0):
        samples = {str(offset): [] for offset in OFFSETS}
        raw_samples = {str(offset): [] for offset in OFFSETS}
        validity, panels, snapshots, raw_snapshots = [], {}, {}, {}
        depths = []
        for frame, shift in enumerate(np.linspace(-.7, .7, args.frames)):
            depth, _, coords, mask = leaf_scene(np, 640, 360, float(shift), lift)
            depths.append(depth)
            for offset in OFFSETS:
                result = renderer.render(depth, quality="High", denoise_passes=2,
                                         reuse_resources=True, radius=32.0,
                                         depth_mip_offset=offset)
                if not np.isfinite(result.decoded_visibility).all():
                    raise ValueError("nonfinite visibility")
                visibility = 1.0 - .35 * (1.0 - result.decoded_visibility)
                aligned, valid = world_samples(np, visibility, ~mask, coords)
                samples[str(offset)].append(aligned)
                validity.append(valid)
                raw = 1.0 - .35 * (1.0 - raw_visibility(np, result.raw_ao))
                raw_aligned, _ = world_samples(np, raw, ~mask, coords)
                raw_samples[str(offset)].append(raw_aligned)
                if frame == 0:
                    snapshots[offset] = result.decoded_visibility.copy()
                    raw_snapshots[offset] = result.raw_ao.copy()
                    gray = np.clip(1.0 - (1.0 - visibility) * 3.0, 0.0, 1.0)
                    rgb = np.repeat((gray[..., None] * 255).astype("uint8"), 3, axis=2)
                    rgb[mask] = [95, 154, 52]
                    panel = Image.fromarray(rgb)
                    ImageDraw.Draw(panel).text((10, 8), f"Offset {offset}  AO x3", fill=(0, 0, 0))
                    panels[offset] = panel
        common = np.logical_and.reduce(validity)
        if int(common.sum()) < 1000:
            raise ValueError("insufficient shared world-ground samples")
        rows = {}
        for offset in OFFSETS:
            repeated = renderer.render(depths[0], quality="High", denoise_passes=2,
                                       reuse_resources=True, depth_mip_offset=offset)
            if not np.array_equal(snapshots[offset], repeated.decoded_visibility):
                raise ValueError("same-pose output changed after other offsets/frames")
            if not np.array_equal(raw_snapshots[offset], repeated.raw_ao):
                raise ValueError("same-depth raw output changed after other offsets/frames")
            row = motion_metrics(np, samples[str(offset)], common)
            row["raw_ao"] = motion_metrics(np, raw_samples[str(offset)], common)
            row["same_depth_rerender_exact"] = True
            row["same_depth_raw_rerender_exact"] = True
            if row["mean_darkening"] <= .0001 or row["p95_darkening"] <= .001:
                raise ValueError("inactive or effectively disabled AO")
            rows[str(offset)] = row
        default = renderer.render(depths[0], quality="High", denoise_passes=2,
                                  reuse_resources=True)
        if not np.array_equal(default.decoded_visibility, snapshots[3.3]):
            raise ValueError("explicit3.3 does not preserve default output")
        baseline = rows["3.3"]
        for row in rows.values():
            row["mean_darkening_ratio_to_default"] = row["mean_darkening"] / baseline["mean_darkening"]
            row["temporal_std_ratio_to_default"] = row["temporal_std_mean"] / baseline["temporal_std_mean"]
            row["coherent_std_ratio_to_default"] = row["coherent_5x5_std_mean"] / baseline["coherent_5x5_std_mean"]
        canvas = Image.new("RGB", (1280, 720), "white")
        for index, offset in enumerate(OFFSETS):
            canvas.paste(panels[offset], ((index % 2) * 640, (index // 2) * 360))
        canvas.save(output / f"lift-{lift:g}.png")
        np.savez_compressed(output / f"lift-{lift:g}-registered.npz", common=common,
                            **{f"offset_{key}": np.stack(values) for key, values in samples.items()})
        report["lifts"][str(lift)] = rows
        print(f"completed lift={lift:g}, shared receivers={int(common.sum())}", flush=True)
    (output / "metrics.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(str(output / "metrics.json"), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-gpu", action="store_true")
    parser.add_argument("--backend", choices=("D3D12", "Vulkan"), required=True)
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        run(args)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
