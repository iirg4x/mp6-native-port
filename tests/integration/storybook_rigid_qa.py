"""Build, run, and compare the transformed-sky Storybook A/B diagnostic.

The two executables differ only in a copied ``StorybookView`` header:
``rigidSky=false`` exercises the legacy procedural scenery, while
``rigidSky=true`` exercises the current transformed mesh groups.  The copied
header also redirects the scenery clock to a test-owned value, so captures at
the same logical time are comparable across processes.  Production sources,
the normal builder, and the user's executable/configuration are untouched.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tests.integration.background_qa import isolate_background, background_process_flags
from tests.integration.build_settings_qa import (
    _prepare_storybook_header,
    _qa_output_path,
    _rewrite_storybook_source,
)

SIZES = ((1280, 720), (1440, 900), (2560, 1440), (1920, 820))
TIMES = (0, 3, 11, 43)
SAMPLE_RE = re.compile(
    r"^\[RIGID-QA\] sample variant=(?P<variant>\S+) size=(?P<w>\d+)x(?P<h>\d+) "
    r"time=(?P<t>\d+) sky_builds=(?P<sky_builds>\d+) sky_reuses=(?P<sky_reuses>\d+) "
    r"geometry_builds=(?P<geometry_builds>\d+) geometry_reuses=(?P<geometry_reuses>\d+) "
    r"render_batches=(?P<render_batches>\d+)$"
)


def replace_once(text: str, before: str, after: str) -> str:
    count = text.count(before)
    if count != 1:
        raise RuntimeError(f"expected one Storybook diagnostic anchor, found {count}: {before!r}")
    return text.replace(before, after, 1)


def option_path(raw: str | None, option: str) -> Path | None:
    if raw is None:
        return None
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    path = path.resolve()
    if not path.is_file():
        raise SystemExit(f"{option} does not name a file: {path}")
    return path


def variant_header(source: Path, output: Path, variant: str) -> tuple[Path, str]:
    text = source.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "const bool rigidSky=mState.motion;",
        f"const bool rigidSky={'true' if variant == 'rigid' else 'false'};",
    )
    text = replace_once(
        text,
        "const float t=mState.motion?float(clock()):0;",
        "const float t=mp6_rigid_qa_time();",
    )
    # All non-scenery springs must also be settled and deterministic during
    # render. Input/state timestamps still use the ordinary startup clock.
    text = replace_once(text, "mFrameTime=wall_clock();mRendering=true;",
                        "mFrameTime=100.0+mp6_rigid_qa_time();mRendering=true;")
    text = text.replace(
        "#pragma once\n",
        "#pragma once\nextern \"C\" float mp6_rigid_qa_time();\n",
        1,
    )

    # These members are test-only observability in the copied header.  They
    # isolate the transformed sky's cache lifecycle from all normal book
    # geometry, allowing the runner to assert layout-only rebuilds directly.
    getter_anchor = "    size_t render_call_count() const { return mRenderCalls; }\n"
    getters = getter_anchor + (
        "    size_t rigid_qa_sky_build_count() const { return mRigidQaSkyBuilds; }\n"
        "    size_t rigid_qa_sky_reuse_count() const { return mRigidQaSkyReuses; }\n"
    )
    text = replace_once(text, getter_anchor, getters)
    end_anchor = (
        "            if(group.building){batch.geometry=GetRenderManager()->MakeGeometry(std::move(batch.mesh));++mGeometryBuilds;}\n"
        "            else ++mGeometryReuses;\n"
    )
    end_replacement = (
        "            if(group.building){batch.geometry=GetRenderManager()->MakeGeometry(std::move(batch.mesh));"
        "++mGeometryBuilds;++mRigidQaSkyBuilds;}\n"
        "            else {++mGeometryReuses;++mRigidQaSkyReuses;}\n"
    )
    text = replace_once(text, end_anchor, end_replacement)
    field_anchor = "    size_t mStateRevision=0,mLayoutRevision=0,mRenderBatches=0,mGeometryBuilds=0,mGeometryReuses=0,mTextGenerations=0,mRenderCalls=0;\n"
    field_replacement = field_anchor + "    size_t mRigidQaSkyBuilds=0,mRigidQaSkyReuses=0;\n"
    text = replace_once(text, field_anchor, field_replacement)

    staged_source = output / f"storybook_view_{variant}.hpp"
    staged_source.parent.mkdir(parents=True, exist_ok=True)
    staged_source.write_text(text, encoding="utf-8")
    return staged_source, hashlib.sha256(text.encode("utf-8")).hexdigest()


def build_variant(variant: str, output: Path, source: Path, jobs: int) -> int:
    os.environ.setdefault("MP6_DISC_ROOT", str(ROOT / "build/disc-cache/orig/GP6E01"))
    os.environ.setdefault("MP6_DECOMP_INC_DATA", str(ROOT / "build/disc-cache/split/include"))
    sys.path.insert(0, str(ROOT / "tools"))
    import build

    output = _qa_output_path(str(output), output)
    output.mkdir(parents=True, exist_ok=True)
    copied_source, _ = variant_header(source, output, variant)
    copied_header, copied_hash = _prepare_storybook_header(output, copied_source)
    # The current Storybook header also has a generated local mesh helper.
    # Keep the diagnostic's complete header family isolated from later source
    # edits; the normal helper predates that include and copies two older deps.
    sky_mesh = ROOT / "src/gx/ui/storybook_sky_mesh.hpp"
    (copied_header.parent / sky_mesh.name).write_bytes(sky_mesh.read_bytes())
    original_configure = build.configure_windows
    original_collect = build.collect_units
    original_objects: Path | None = None

    def configure(configuration):
        nonlocal original_objects
        original_configure(configuration)
        original_objects = Path(build.OBJ_DIR)
        # Keep all generated objects and the executable in this variant's
        # isolated tree; the selected Aurora include/archive remains the one
        # chosen by configure_windows.
        build.BUILD_DIR = str(output)
        build.OBJ_DIR = str(output / "obj")

    def collect(*args, **kwargs):
        assert original_objects is not None
        units = []
        harness = (ROOT / "tests/integration/storybook_rigid_qa.hpp").as_posix()
        ui_include = str(ROOT / "src/gx/ui")
        for source_path, flags, name, flavor in original_collect(*args, **kwargs):
            obj = original_objects / name
            filename = Path(source_path).name
            if filename == "launcher_core.cpp":
                text = Path(source_path).read_text(encoding="utf-8")
                anchor = 'extern "C" int mp6_launcher_run_menu(void *sdlWindowPtr)\n{'
                text = replace_once(
                    text,
                    anchor,
                    f'#include "{harness}"\n{anchor}\n    return mp6_rigid_qa_run(sdlWindowPtr);',
                )
                destination = output / filename
                destination.write_text(text, encoding="utf-8")
                source_path = str(destination)
                obj = output / "obj" / name
                flags = [*flags, "-I", ui_include, "-DMP6_RIGID_QA"]
            if copied_header is not None:
                source_path, flags, obj = _rewrite_storybook_source(
                    source_path, output, copied_header, copied_hash, flags, obj
                )
            units.append((source_path, flags, str(obj), flavor))
        return units

    build.configure_windows = configure
    build.collect_units = collect
    sys.argv = [str(ROOT / "tools/build.py"), "--configuration", "release", f"-j{jobs}"]
    return build.main()


def parse_metrics(path: Path) -> list[dict]:
    rows = []
    for line in (path / "run.log").read_text(encoding="utf-8", errors="replace").splitlines():
        match = SAMPLE_RE.match(line)
        if not match:
            continue
        row = {key: int(value) if key not in {"variant"} else value
               for key, value in match.groupdict().items()}
        rows.append(row)
    return rows


def run_variant(variant: str, output: Path) -> int:
    exe = output / "release" / "mp6native.exe"
    if not exe.is_file():
        raise SystemExit(f"missing rigid sky QA executable: {exe}")
    captures = output / "captures"
    captures.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items() if not key.startswith("MP6_")}
    env.update(
        MP6_LAUNCHER="1",
        MP6_DISC_ROOT=str(ROOT / "build/disc-cache/orig/GP6E01"),
        MP6_GPU_CACHE_PATH=str(output / "gpu-cache"),
        MP6_WINDOW_SIZE="1440x900",
        MP6_VSYNC="0",
        MP6_ALLOW_CPU_ADAPTER="0",
        MP6_RIGID_QA_CAPTURE_ROOT=str(captures),
        MP6_RIGID_QA_VARIANT=variant,
    )
    isolate_background(env, exe)
    (exe.parent / "mp6_config.json").write_text(json.dumps({"video.vsync": False}), encoding="utf-8")
    log_path = output / "run.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(
            [str(exe)], cwd=captures, env=env, stdout=log, stderr=subprocess.STDOUT,
            timeout=90, creationflags=background_process_flags(), check=False,
        )
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    for line in log_text.splitlines():
        if "[RIGID-QA]" in line or "[BACKGROUND-QA]" in line:
            print(line)
    rows = parse_metrics(output)
    expected = {(w, h, t) for w, h in SIZES for t in TIMES}
    actual = {(row["w"], row["h"], row["t"]) for row in rows}
    if result.returncode != 0 or "[RIGID-QA] COMPLETE" not in log_text or actual != expected:
        print(log_text[-5000:])
        return result.returncode or 1
    (output / "metrics.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return 0


def compare(baseline: Path, rigid: Path) -> int:
    from PIL import Image, ImageChops, ImageStat

    baseline = baseline.resolve()
    rigid = rigid.resolve()
    baseline_metrics = json.loads((baseline / "metrics.json").read_text(encoding="utf-8"))
    rigid_metrics = json.loads((rigid / "metrics.json").read_text(encoding="utf-8"))
    expected_sample_count = len(SIZES) * len(TIMES)
    if (len(baseline_metrics) != expected_sample_count or
            len(rigid_metrics) != expected_sample_count):
        raise SystemExit("rigid sky metrics do not contain all 16 size/time samples")
    if any(row["sky_builds"] or row["sky_reuses"] for row in baseline_metrics):
        raise AssertionError("legacy A/B unexpectedly used transformed sky groups")
    for width, height in SIZES:
        rows = [row for row in rigid_metrics if row["w"] == width and row["h"] == height]
        if not rows or rows[0]["sky_builds"] <= 0:
            raise AssertionError(f"rigid sky did not build its cache at {width}x{height}")
        if any(row["sky_builds"] != rows[0]["sky_builds"] for row in rows[1:]):
            raise AssertionError(f"rigid sky rebuilt on a time-only change at {width}x{height}")
        if any(rows[i]["sky_reuses"] <= rows[i - 1]["sky_reuses"] for i in range(1, len(rows))):
            raise AssertionError(f"rigid sky did not reuse its cache at {width}x{height}")
    rows = []
    diff_root = rigid / "diff"
    diff_root.mkdir(parents=True, exist_ok=True)
    for width, height in SIZES:
        for time in TIMES:
            name = f"{{variant}}-{width}x{height}-t{time}.ppm"
            old_path = baseline / "captures" / name.format(variant="baseline")
            new_path = rigid / "captures" / name.format(variant="rigid")
            if not old_path.is_file() or not new_path.is_file():
                raise SystemExit(f"missing A/B capture: {old_path} or {new_path}")
            old = Image.open(old_path).convert("RGB")
            new = Image.open(new_path).convert("RGB")
            old.save(old_path.with_suffix(".png"))
            new.save(new_path.with_suffix(".png"))
            if old.size != new.size:
                raise SystemExit(f"capture size mismatch: {old_path} vs {new_path}")
            difference = ImageChops.difference(old, new)
            extrema = difference.getextrema()
            maximum = max(high for _, high in extrema)
            mean = sum(ImageStat.Stat(difference).mean) / 3
            changed = sum(1 for pixel in difference.getdata() if max(pixel) > 16)
            fraction = changed / (old.width * old.height)
            # The book intentionally hides most of the sky. Check the exposed
            # top/side/bottom bands separately so a missing balloon/star cannot
            # be diluted by the large identical paper surface.
            top = int(old.height * .18)
            bottom = int(old.height * .90)
            left = int(old.width * .12)
            right = int(old.width * .88)
            exposed_regions = [
                difference.crop((0, 0, old.width, top)),
                difference.crop((0, bottom, old.width, old.height)),
                difference.crop((0, top, left, bottom)),
                difference.crop((right, top, old.width, bottom)),
            ]
            exposed_pixels = sum(region.width * region.height for region in exposed_regions)
            exposed_changed = sum(
                sum(max(pixel) > 16 for pixel in region.getdata())
                for region in exposed_regions
            )
            exposed_sum = sum(
                sum(sum(pixel) / 3 for pixel in region.getdata())
                for region in exposed_regions
            )
            exposed_fraction = exposed_changed / exposed_pixels
            exposed_mean = exposed_sum / exposed_pixels
            diff_path = diff_root / f"rigid-vs-baseline-{width}x{height}-t{time}.png"
            difference.save(diff_path)
            rows.append({"width": width, "height": height, "time": time,
                         "max_abs": maximum, "mean_abs": mean,
                         "changed_fraction_over_16": fraction,
                         "exposed_mean_abs": exposed_mean,
                         "exposed_changed_fraction_over_16": exposed_fraction,
                         "diff": str(diff_path)})
            # A transform-palette edge may move by a few coverage values, but
            # broad sky changes are a regression. These bounds are deliberately
            # tight enough to catch dropped particles or wrong colors.
            if mean > .02 or fraction > .0001 or exposed_mean > .05 or exposed_fraction > .0003:
                raise AssertionError(f"rigid sky visual delta is too large: {rows[-1]}")
    (rigid / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(rows, indent=2))
    print("[RIGID-QA] A/B visual comparison passed tolerant raster bounds")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("baseline", "rigid"), default="rigid")
    parser.add_argument("--output", help="variant build/output directory under build/")
    parser.add_argument("--storybook-header", help="source StorybookView header; defaults to production")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("MP6_QA_JOBS", "2")))
    parser.add_argument("--build", action="store_true", help="build the isolated variant")
    parser.add_argument("--run", action="store_true", help="run the isolated variant")
    parser.add_argument("--compare", nargs=2, metavar=("BASELINE_DIR", "RIGID_DIR"),
                        help="compare captures from two completed runs")
    args = parser.parse_args()
    if args.compare:
        return compare(Path(args.compare[0]), Path(args.compare[1]))
    output = Path(args.output) if args.output else ROOT / "build" / f"storybook-rigid-qa-{args.variant}"
    output = _qa_output_path(str(output), output)
    source = option_path(args.storybook_header, "--storybook-header") or ROOT / "src/gx/ui/storybook_view.hpp"
    # The default is build+run for an explicit variant; --build/--run allow
    # the parent to separate serial native build and GPU execution.
    explicit_action = args.build or args.run
    do_build = args.build or not explicit_action
    do_run = args.run or not explicit_action
    if do_build:
        result = build_variant(args.variant, output, source, max(1, args.jobs))
        if result:
            return result
    if do_run:
        return run_variant(args.variant, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
