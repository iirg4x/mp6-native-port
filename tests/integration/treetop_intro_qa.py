#!/usr/bin/env python3
"""Build and run a private Treetop intro diagnostic.

This is intentionally separate from the normal board-QA executable.  It
reuses the board-QA fixture builder, but writes all generated sources/objects
under ``build/treetop-intro-qa`` and instruments only that executable:

* ``MasuDraw`` reports the model id, visible authored space count, and an FNV1a
  hash of the visible space ids once per simulation tick.
* the frame-interpolation pairer reports the camera/model group and counts
  whenever its membership guard forces a pair to snap.

The runner compares AO/FI combinations at the same boot route.  It is a
diagnostic, not a gameplay pass; the log and optional frame dumps are the
evidence consumed by the parent task.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "tests/integration/build_board_qa.py"
PRIVATE = ROOT / "build/treetop-intro-qa"
RUNS = ROOT / "build/treetop-intro-qa-runs"
sys.path.insert(0, str(ROOT))
from tests.integration.background_qa import isolate_background, background_process_flags


def _load_board_builder():
    """Load build_board_qa definitions without executing its build."""
    text = BUILDER.read_text(encoding="utf-8")
    marker = "build.configure_windows = configure\n"
    prefix, sep, _ = text.partition(marker)
    if not sep:
        raise RuntimeError("board-QA builder footer changed; refuse private build")
    # The board builder unlinks its READY file at module execution. Redirect
    # that literal before exec so a private diagnostic can never invalidate
    # the parent's standard board-QA attestation.
    prefix = prefix.replace(
        "OUTPUT = ROOT / 'build/board-qa'",
        "OUTPUT = ROOT / 'build/treetop-intro-qa'",
        1,
    )
    namespace = {"__name__": "_treetop_board_qa_builder", "__file__": str(BUILDER)}
    exec(compile(prefix, str(BUILDER), "exec"), namespace)
    namespace["OUTPUT"] = PRIVATE
    namespace["READY"] = PRIVATE / "build-ready.json"
    return namespace


def _instrument_masu(contents: str) -> str:
    contents = '#include <stdint.h>\n#include <stdio.h>\nextern long mp6_tick_count;\n' + contents
    marker = "    int capPlayerNo;\n"
    inject = marker + (
        "    int qaVisibleSpaces = 0;\n"
        "    uint32_t qaVisibleHash = 2166136261u;\n"
    )
    if contents.count(marker) != 1:
        raise RuntimeError("MasuDraw locals changed; private probe not applied")
    contents = contents.replace(marker, inject, 1)
    cull_candidates = (
        "        if (!mp6StableSpaceDraw && !mbCameraCullCheck(&pos, 200.0f)) {\n"
        "            continue;\n"
        "        }\n",
        "        if (!mbCameraCullCheck(&pos, 200.0f)) {\n"
        "            continue;\n"
        "        }\n",
    )
    cull = next((candidate for candidate in cull_candidates if contents.count(candidate) >= 2), None)
    if cull is None:
        raise RuntimeError("MasuDraw cull sites changed; private probe not applied")
    primary = cull + (
        "        if (getenv(\"MP6_TREETOP_QA\")) {\n"
        "            qaVisibleSpaces++;\n"
        "            qaVisibleHash = (qaVisibleHash ^ (uint32_t)(i + 1)) * 16777619u;\n"
        "        }\n"
    )
    contents = contents.replace(cull, primary, 1)
    marker = "    if (masuCapsuleDispF == FALSE || GWPartyGet() == FALSE) {\n"
    report = (
        "    if (getenv(\"MP6_TREETOP_QA\")) {\n"
        "        static long qaLastTick = -1;\n"
        "        if (qaLastTick != mp6_tick_count) {\n"
        "            qaLastTick = mp6_tick_count;\n"
        "            fprintf(stderr, \"[TREETOP-SPACE] tick=%ld model=%d visible=%d hash=%08x\\n\",\n"
        "                    mp6_tick_count, (int)(modelP - Hu3DData),\n"
        "                    qaVisibleSpaces, (unsigned)qaVisibleHash);\n"
        "            fflush(stderr);\n"
        "        }\n"
        "    }\n" + marker
    )
    if contents.count(marker) != 1:
        raise RuntimeError("MasuDraw capsule boundary changed; private probe not applied")
    return contents.replace(marker, report, 1)


def _instrument_frame_interp(contents: str) -> str:
    marker = "        if (gCur != gPrev || oCur != 1 || oPrev != 1) {\n"
    if contents.count(marker) != 1:
        raise RuntimeError("frame-interp pair guard changed; private probe not applied")
    inject = (
        "        if (getenv(\"MP6_TREETOP_QA\") && fi_diag() >= 3 &&\n"
        "            (gCur != gPrev || oCur != 1 || oPrev != 1)) {\n"
        "            static long qaDiagTick = -1;\n"
        "            static unsigned qaDiagLines;\n"
        "            if (qaDiagTick != cur->tick) { qaDiagTick = cur->tick; qaDiagLines = 0; }\n"
        "            if (qaDiagLines++ < 64)\n"
        "                fprintf(stderr, \"[TREETOP-FI-GROUP] simtick=%ld seal=%ld idx=%u camera=%d model=%d gen=%u sub=%u ordinal=%u cur=%u prev=%u reason=%s\\n\",\n"
        "                        mp6_tick_count, cur->tick, i, cur->posKey[i].camera,\n"
        "                        cur->posKey[i].model, cur->posKey[i].generation,\n"
        "                        cur->posKey[i].sub, cur->posKey[i].ordinal, gCur, gPrev,\n"
        "                        gCur != gPrev ? \"count\" : \"duplicate\");\n"
        "        }\n" + marker
    )
    return contents.replace(marker, inject, 1)


def build_private() -> dict:
    ns = _load_board_builder()
    output = PRIVATE
    output.mkdir(parents=True, exist_ok=True)
    # The private tree is disposable generated output, but avoid silently
    # removing a prior run; tools/build.py's incremental objects are useful.
    (output / "release").mkdir(exist_ok=True)
    original_collect = ns["collect"]

    def collect(*args, **kwargs):
        units = []
        for source, flags, name, flavor in original_collect(*args, **kwargs):
            source_path = Path(source)
            unit_name = Path(name).name
            if unit_name == "board_masu.o":
                text = _instrument_masu(source_path.read_text(encoding="utf-8"))
                source_path = output / "board_masu_treetop_probe.c"
                source_path.write_text(text, encoding="utf-8")
                obj = output / "obj" / unit_name
                units.append((str(source_path), flags, str(obj), flavor))
                continue
            if unit_name == "plat_frame_interp_aurora.o":
                text = _instrument_frame_interp(source_path.read_text(encoding="utf-8"))
                source_path = output / "frame_interp_treetop_probe.c"
                source_path.write_text(text, encoding="utf-8")
                obj = output / "obj" / unit_name
                units.append((str(source_path), flags, str(obj), flavor))
                continue
            units.append((source, flags, name, flavor))
        return units

    ns["build"].configure_windows = ns["configure"]
    ns["build"].collect_units = collect
    ns["build"].OUTPUT = str(output)
    # Prevent the imported builder from inheriting a caller's stale QA path.
    os.environ.setdefault("MP6_DISC_ROOT", "build/disc-cache/orig/GP6E01")
    os.environ.setdefault("MP6_DECOMP_INC_DATA", "build/disc-cache/split/include")
    ns["sys"].argv = [str(ROOT / "tools/build.py"), "--configuration", "release", "-j6"]
    result = ns["build"].main()
    if result != 0:
        raise SystemExit(result)
    exe = output / "release/mp6native.exe"
    ready = {
        "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "builder_sha256": hashlib.sha256(BUILDER.read_bytes()).hexdigest(),
        "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    (output / "build-ready.json").write_text(json.dumps(ready, indent=2) + "\n", encoding="utf-8")
    return ready


def _run_one(exe: Path, name: str, ao: int, fi: int, seconds: int, capture: int) -> dict:
    run_dir = RUNS / name
    run_dir.mkdir(parents=True, exist_ok=False)
    (run_dir / "gpu-cache").mkdir()
    env = {key: value for key, value in os.environ.items() if not key.startswith("MP6_")}
    env.update(
        SDL_AUDIODRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        MP6_LAUNCHER="0",
        MP6_BOOT_TO="mdparty",
        MP6_AUTO_START_TICKS="60,150",
        MP6_TICK_HZ="60",
        MP6_WINDOW_SIZE="848x480",
        MP6_DISC_ROOT=str(ROOT / "build/disc-cache/orig/GP6E01"),
        MP6_QA_CAPSULE="-1",
        MP6_QA_ROUTE="",
        MP6_GPU_CACHE_PATH=str(run_dir / "gpu-cache"),
        MP6_ENH_AMBIENT_OCCLUSION=str(ao),
        MP6_ENH_AA="0",
        MP6_ENH_SHADOW_QUALITY="1",
        MP6_UNLOCKED_FPS="1" if fi else "0",
        MP6_VSYNC="0",
        MP6_TICK_RATE_LOG="1",
        MP6_PRESENT_RATE_LOG="1",
        MP6_AO_DIAG="1",
        MP6_AO_XEGTAO="0",
        MP6_FI_DIAG=str(fi),
        MP6_FI_ANIMLOG="1",
        MP6_TREETOP_QA="1",
        MP6_FRAME_DUMP=str(run_dir / "frames"),
        MP6_FRAME_DUMP_COUNT=str(capture),
        MP6_FRAME_DUMP_STRIDE="4",
        MP6_FRAME_DUMP_DELAY="0",
        MP6_FRAME_DUMP_TRIGGER="ovl.start=w01dll",
    )
    (run_dir / "frames").mkdir()
    log_path = run_dir / "game.log"
    # Stop synthetic A presses when the W01 REL actually starts.  Continuing
    # to press through this point can skip the camera flythrough that is the
    # subject of this diagnostic.
    script = "period:30;timeout:119000;pressuntil:a/ovl.start/w01dll;wait:120000"
    isolate_background(env, exe)
    with log_path.open("w", encoding="utf-8") as output:
        process = subprocess.Popen(
            [str(exe), "150000", "--input-script", script],
            cwd=run_dir,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
            creationflags=background_process_flags(),
        )
        try:
            exit_code = process.wait(timeout=seconds)
            reason = "exited"
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                exit_code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                exit_code = process.wait(timeout=10)
            reason = "wall-clock limit"
    text = log_path.read_text(errors="replace")
    result = {
        "name": name,
        "ao": ao,
        "fi": fi,
        "exit_code": exit_code,
        "reason": reason,
        "w01_live": "[EVENT] w01.live=" in text,
        "w01_start": "[EVENT] ovl.start=" in text and "w01dll" in text,
        "space_lines": len(re.findall(r"^\[TREETOP-SPACE\]", text, re.M)),
        "fi_group_lines": len(re.findall(r"^\[TREETOP-FI-GROUP\]", text, re.M)),
        "fi_pair_lines": len(re.findall(r"^\[MP6-FI\] pair ", text, re.M)),
        "captures": len(list((run_dir / "frames").glob("*.mfd"))),
        "crash": any(marker in text for marker in ("[MP6-CRASH]", "[FATAL]", "[AURORA FATAL:")),
        "log": str(log_path),
    }
    (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true", help="build the private instrumented executable")
    parser.add_argument("--run", action="store_true", help="run the requested AO/FI matrix")
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--capture", type=int, default=180)
    parser.add_argument("--ao", type=int, choices=(0, 2), help="run one AO level instead of the full matrix")
    parser.add_argument("--fi", type=int, choices=(0, 3), help="run one FI diagnostic level instead of the full matrix")
    args = parser.parse_args()
    if args.seconds < 1 or args.seconds > 120:
        parser.error("--seconds must be between 1 and 120")
    if args.capture < 0 or args.capture > 1000:
        parser.error("--capture must be between 0 and 1000")
    if not args.build and not args.run:
        args.build = args.run = True
    if args.build:
        ready = build_private()
        print(json.dumps(ready, indent=2), flush=True)
    if args.run:
        exe = PRIVATE / "release/mp6native.exe"
        if not exe.is_file():
            raise SystemExit("private executable missing; use --build")
        ready_path = PRIVATE / "build-ready.json"
        if not ready_path.is_file():
            raise SystemExit("private build attestation missing; use --build")
        ready = json.loads(ready_path.read_text(encoding="utf-8"))
        actual_sha = hashlib.sha256(exe.read_bytes()).hexdigest()
        expected_builder = hashlib.sha256(BUILDER.read_bytes()).hexdigest()
        expected_probe = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        if (actual_sha != ready.get("sha256") or
                expected_builder != ready.get("builder_sha256") or
                expected_probe != ready.get("probe_sha256")):
            raise SystemExit("private executable hash does not match build-ready.json; use --build")
        RUNS.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        results = []
        if (args.ao is None) != (args.fi is None):
            parser.error("--ao and --fi must be supplied together")
        matrix = [(args.ao, args.fi)] if args.ao is not None else (
            (0, 0), (0, 3), (2, 0), (2, 3)
        )
        for ao, fi in matrix:
            results.append(_run_one(exe, f"{stamp}-ao{ao}-fi{fi}", ao, fi, args.seconds, args.capture))
        print(json.dumps(results, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
