#!/usr/bin/env python3
"""gate_android.py -- the per-merge Android cross-build gate, implementing
tier 1 and tier 2 as ONE documented command (docs/TESTING.md's Android
gate section):

  tier 1 (no hardware, every merge): BOTH Android artifacts build --
      headless  (build.py --target aarch64-android)          -> build/android/
      windowed  (build.py --target aarch64-android --windowed) -> build/android/aurora/
      Then Gradle assembles a fresh selected-configuration APK and the gate inspects its exact
      native-library/resource manifest. Catches NDK/bionic/API/Aurora/Gradle
      packaging drift. A failure here FAILS the gate.

  tier 2 (device smoke, only when hardware is attached): the headless
      600-tick boot on the device, diffed against the committed Windows log
      with tools/ua1_logdiff.py (the "identical game flow" comparator).
      Needs the disc tree already staged at /data/local/tmp/mp6
      (GP6E01/sys/fst.bin probe below). NO DEVICE (or no staged assets)
      => graceful SKIP with exit 0 -- absence of hardware must never block
      a merge. A device that IS present but produces a diverging log FAILS
      the gate.

  tier 3 stays manual by design (windowed screencap flow on a real device;
      per-device GPU variance makes it advisory, not merge-blocking).

Usage:
    python tools/gate_android.py                 # tiers 1+2 (2 auto-skips)
    python tools/gate_android.py --no-device     # tier 1 only, never touch adb
    python tools/gate_android.py --configuration release --no-device
    python tools/gate_android.py --serial SER    # pin a specific device

Exit 0 = every executed tier passed (skips are not failures).
Exit 1 = a build failed, or an attached device's smoke diverged.
"""
import argparse
import os
import re
import subprocess
import sys

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
BUILD_PY = os.path.join(NATIVE_ROOT, "tools", "build.py")
LOGDIFF_PY = os.path.join(NATIVE_ROOT, "tools", "ua1_logdiff.py")
WIN_LOG = os.path.join(NATIVE_ROOT, "docs", "ua1", "win_headless_600.log")
DEVICE_BASE = "/data/local/tmp/mp6"  # build.py ANDROID_DEVICE_BASE
OUT_DIR = os.path.join(NATIVE_ROOT, "build", "android")
ADB_QUERY_TIMEOUT = 15
ADB_TRANSFER_TIMEOUT = 120
ADB_SMOKE_TIMEOUT = 180
LOGDIFF_TIMEOUT = 60

if NATIVE_ROOT not in sys.path:
    sys.path.insert(0, NATIVE_ROOT)
from setup.lib import common, step_android


def find_adb():
    env = os.environ.get("ADB")
    if env and os.path.isfile(env):
        return env
    cand = os.path.join(PORT_ROOT, "android-sdk", "platform-tools",
                        "adb.exe" if os.name == "nt" else "adb")
    return cand if os.path.exists(cand) else None


def run(cmd, **kw):
    print(f"[gate-android] $ {' '.join(str(c) for c in cmd)}")
    sys.stdout.flush()
    return subprocess.run(cmd, **kw)


def run_bounded(cmd, label, timeout, **kw):
    try:
        return run(cmd, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        print(f"[gate-android] TIER2 FAIL: {label} timed out after {timeout}s")
        return None
    except OSError as exc:
        print(f"[gate-android] TIER2 FAIL: could not start {label}: {exc}")
        return None


def terminal_exit_problem(output):
    lines = [line.rstrip("\r") for line in output.splitlines() if line.strip()]
    markers = [(index, line) for index, line in enumerate(lines)
               if re.fullmatch(r"EXIT=[0-9]+", line)]
    if len(markers) != 1:
        return f"expected exactly one terminal EXIT marker, found {[line for _i, line in markers]}"
    index, marker = markers[0]
    if index != len(lines) - 1:
        return f"terminal EXIT marker is not the final non-empty line: {marker}"
    if marker != "EXIT=0":
        return f"on-device launcher reported {marker}"
    return None


def device_asset_probe_result(proc):
    """Classify only an actual absent file as a tier-2 skip.

    adb transport, shell, permission, and protocol failures are gate failures;
    treating every nonzero `ls` as "assets missing" used to hide those faults.
    """
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 0:
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        expected = f"{DEVICE_BASE}/GP6E01/sys/fst.bin"
        return "present" if lines == [expected] else (
            f"asset probe returned unexpected success output: {lines!r}"
        )
    lowered = output.lower()
    if "no such file" in lowered or "not found" in lowered:
        return "missing"
    return f"asset probe failed operationally (adb={proc.returncode}): {output.strip() or 'no output'}"


def tier1_builds(allow_dirty_decomp=False, configuration="debug"):
    for label, extra in (("headless", []), ("windowed", ["--windowed"])):
        override = ["--allow-dirty-decomp"] if allow_dirty_decomp else []
        profile = ["--configuration", configuration]
        proc = run([sys.executable, BUILD_PY, "--target", "aarch64-android"]
                   + profile + extra + override)
        if proc.returncode != 0:
            print(f"[gate-android] TIER1 FAIL: android {label} build exited {proc.returncode}")
            return False
        print(f"[gate-android] tier1: android {label} build OK")
    try:
        apk = step_android.build_apk(NATIVE_ROOT, variant=configuration)
    except common.SetupError as exc:
        print(f"[gate-android] TIER1 FAIL: {configuration} APK build/inspection: {exc.message}")
        if exc.hint:
            print(f"[gate-android]   {exc.hint}")
        return False
    print(f"[gate-android] tier1: fresh {configuration} APK compiled + inspected OK: {apk}")
    return True


def tier2_device_smoke(serial):
    adb = find_adb()
    if adb is None:
        print("[gate-android] tier2 SKIP: no adb (set ADB=... or install "
              "port/android-sdk/platform-tools)")
        return None
    base = [adb] + (["-s", serial] if serial else [])

    state = run_bounded(
        base + ["get-state"], "adb get-state", ADB_QUERY_TIMEOUT,
        capture_output=True, text=True,
    )
    if state is None:
        return False
    if state.returncode != 0 or state.stdout.strip() != "device":
        print(f"[gate-android] tier2 SKIP: no device attached "
              f"(adb get-state: {(state.stdout + state.stderr).strip() or 'none'})")
        return None

    probe = run_bounded(
        base + ["shell", f"ls {DEVICE_BASE}/GP6E01/sys/fst.bin"],
        "device asset probe", ADB_QUERY_TIMEOUT, capture_output=True, text=True,
    )
    if probe is None:
        return False
    probe_result = device_asset_probe_result(probe)
    if probe_result == "missing":
        print(f"[gate-android] tier2 SKIP: device attached but {DEVICE_BASE} has no "
              "staged disc tree (push the GP6E01 disc tree there first)")
        return None
    if probe_result != "present":
        print(f"[gate-android] TIER2 FAIL: {probe_result}")
        return False

    so = os.path.join(OUT_DIR, "libmp6game.so")
    launcher = os.path.join(OUT_DIR, "mp6launcher")
    for f in (so, launcher):
        if not os.path.exists(f):
            print(f"[gate-android] TIER2 FAIL: missing artifact {f} (tier1 should have built it)")
            return False

    pushed = run_bounded(
        base + ["push", launcher, so, DEVICE_BASE + "/"],
        "adb push", ADB_TRANSFER_TIMEOUT,
    )
    if pushed is None or pushed.returncode != 0:
        print("[gate-android] TIER2 FAIL: adb push")
        return False
    chmod = run_bounded(
        base + ["shell", f"chmod 755 {DEVICE_BASE}/mp6launcher"],
        "device chmod", ADB_QUERY_TIMEOUT,
    )
    if chmod is None or chmod.returncode != 0:
        print("[gate-android] TIER2 FAIL: device chmod")
        return False

    device_log = os.path.join(OUT_DIR, "gate_device_600.log")
    with open(device_log, "w", encoding="utf-8", newline="\n") as f:
        proc = run_bounded(
            base + ["shell", f"cd {DEVICE_BASE} && ./mp6launcher 600; echo EXIT=$?"],
            "600-tick device smoke", ADB_SMOKE_TIMEOUT,
            stdout=f, stderr=subprocess.STDOUT, text=True,
        )
    if proc is None:
        return False
    if proc.returncode != 0:
        print(f"[gate-android] TIER2 FAIL: adb shell run exited {proc.returncode}")
        return False
    with open(device_log, encoding="utf-8", errors="replace") as f:
        tail = f.read()
    exit_problem = terminal_exit_problem(tail)
    if exit_problem:
        print(f"[gate-android] TIER2 FAIL: {exit_problem} (see {device_log})")
        return False

    proc = run_bounded(
        [sys.executable, LOGDIFF_PY, WIN_LOG, device_log],
        "device log comparison", LOGDIFF_TIMEOUT,
    )
    if proc is None or proc.returncode != 0:
        print(f"[gate-android] TIER2 FAIL: ua1_logdiff divergence (device log: {device_log})")
        return False
    print("[gate-android] tier2: device 600-tick logdiff PASS")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-device", action="store_true",
                    help="tier 1 only; never touch adb")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"),
                    help="adb device serial (default: sole attached device)")
    ap.add_argument("--allow-dirty-decomp", action="store_true",
                    help="explicit development-only passthrough to build.py's reproducibility override")
    ap.add_argument("--configuration", choices=("debug", "release"), default="debug",
                    help="native/APK mode: debug=-O0 or release=-O2/-fno-strict-aliasing "
                         "plus lint (default: debug)")
    args = ap.parse_args()

    if not tier1_builds(args.allow_dirty_decomp, args.configuration):
        return 1
    if args.no_device:
        print("[gate-android] device smoke skipped (--no-device); native + APK gates ran")
        print(f"[gate-android] PASS (tier 1 {args.configuration} native/Gradle/APK inspection)")
        return 0
    t2 = tier2_device_smoke(args.serial)
    if t2 is False:
        return 1
    print(f"[gate-android] PASS (tier 1{'+2' if t2 else '; tier 2 skipped'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
