#!/usr/bin/env python3
"""gate_android.py -- the per-merge Android cross-build gate, implementing
tier 1 and tier 2 as ONE documented command (docs/TESTING.md's Android
gate section):

  tier 1 (no hardware, every merge): BOTH Android artifacts build --
      headless  (build.py --target aarch64-android)          -> build/android/
      windowed  (build.py --target aarch64-android --windowed) -> build/android/aurora/
      Catches NDK/bionic/API/aurora-android drift for free. A failure here
      FAILS the gate.

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
    python tools/gate_android.py --serial SER    # pin a specific device

Exit 0 = every executed tier passed (skips are not failures).
Exit 1 = a build failed, or an attached device's smoke diverged.
"""
import argparse
import os
import subprocess
import sys

NATIVE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
BUILD_PY = os.path.join(NATIVE_ROOT, "tools", "build.py")
LOGDIFF_PY = os.path.join(NATIVE_ROOT, "tools", "ua1_logdiff.py")
WIN_LOG = os.path.join(NATIVE_ROOT, "docs", "ua1", "win_headless_600.log")
DEVICE_BASE = "/data/local/tmp/mp6"  # build.py ANDROID_DEVICE_BASE
OUT_DIR = os.path.join(NATIVE_ROOT, "build", "android")


def find_adb():
    env = os.environ.get("ADB")
    if env and os.path.exists(env):
        return env
    cand = os.path.join(PORT_ROOT, "android-sdk", "platform-tools",
                        "adb.exe" if os.name == "nt" else "adb")
    return cand if os.path.exists(cand) else None


def run(cmd, **kw):
    print(f"[gate-android] $ {' '.join(str(c) for c in cmd)}")
    sys.stdout.flush()
    return subprocess.run(cmd, **kw)


def tier1_builds():
    for label, extra in (("headless", []), ("windowed", ["--windowed"])):
        proc = run([sys.executable, BUILD_PY, "--target", "aarch64-android"] + extra)
        if proc.returncode != 0:
            print(f"[gate-android] TIER1 FAIL: android {label} build exited {proc.returncode}")
            return False
        print(f"[gate-android] tier1: android {label} build OK")
    return True


def tier2_device_smoke(serial):
    adb = find_adb()
    if adb is None:
        print("[gate-android] tier2 SKIP: no adb (set ADB=... or install "
              "port/android-sdk/platform-tools)")
        return None
    base = [adb] + (["-s", serial] if serial else [])

    state = subprocess.run(base + ["get-state"], capture_output=True, text=True)
    if state.returncode != 0 or state.stdout.strip() != "device":
        print(f"[gate-android] tier2 SKIP: no device attached "
              f"(adb get-state: {(state.stdout + state.stderr).strip() or 'none'})")
        return None

    probe = subprocess.run(base + ["shell", f"ls {DEVICE_BASE}/GP6E01/sys/fst.bin"],
                           capture_output=True, text=True)
    if probe.returncode != 0 or "No such" in probe.stdout + probe.stderr:
        print(f"[gate-android] tier2 SKIP: device attached but {DEVICE_BASE} has no "
              "staged disc tree (push the GP6E01 disc tree there first)")
        return None

    so = os.path.join(OUT_DIR, "libmp6game.so")
    launcher = os.path.join(OUT_DIR, "mp6launcher")
    for f in (so, launcher):
        if not os.path.exists(f):
            print(f"[gate-android] TIER2 FAIL: missing artifact {f} (tier1 should have built it)")
            return False

    if run(base + ["push", launcher, so, DEVICE_BASE + "/"]).returncode != 0:
        print("[gate-android] TIER2 FAIL: adb push")
        return False
    run(base + ["shell", f"chmod 755 {DEVICE_BASE}/mp6launcher"])

    device_log = os.path.join(OUT_DIR, "gate_device_600.log")
    with open(device_log, "w", encoding="utf-8", newline="\n") as f:
        proc = subprocess.run(base + ["shell",
                                      f"cd {DEVICE_BASE} && ./mp6launcher 600; echo EXIT=$?"],
                              stdout=f, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        print(f"[gate-android] TIER2 FAIL: adb shell run exited {proc.returncode}")
        return False
    tail = open(device_log, encoding="utf-8", errors="replace").read()
    if "EXIT=0" not in tail:
        print(f"[gate-android] TIER2 FAIL: on-device run did not exit 0 (see {device_log})")
        return False

    proc = run([sys.executable, LOGDIFF_PY, WIN_LOG, device_log])
    if proc.returncode != 0:
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
    args = ap.parse_args()

    if not tier1_builds():
        return 1
    if args.no_device:
        print("[gate-android] tier2 skipped (--no-device)")
        print("[gate-android] PASS (tier 1)")
        return 0
    t2 = tier2_device_smoke(args.serial)
    if t2 is False:
        return 1
    print(f"[gate-android] PASS (tier 1{'+2' if t2 else '; tier 2 skipped'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
