#!/usr/bin/env python3
"""leakgate.py -- the project's standing memory-leak acceptance gate.

Runs a port executable for a fixed duration, samples its RSS (and live
handle count -- diagnostic only, does not affect the verdict) on an
interval, discards a warmup window, then fits a least-squares slope to the
steady-state RSS samples. PASS iff the slope is under the threshold. This is
the mandatory verification step for ANY change that touches allocation paths
(loaders, bridges, shims, caches) -- see docs/TESTING.md's leak gate section.

Usage:
  python tools/leakgate.py build/mp6native_headless.exe --duration 300
  python tools/leakgate.py build/mp6native.exe --args "--input-script" "wait:300;press:start" \
      --duration 300 --threshold-kb-min 500 --lockfile ../.visual_test.lock
  # capture the exe's own stdout (e.g. an MP6_ALLOC_CENSUS_START_TICK log)
  # alongside the official verdict, instead of losing it to DEVNULL:
  python tools/leakgate.py build/mp6native.exe --duration 300 --capture-stdout run.log ...

Exit code 0 = PASS, 1 = FAIL (leak), 2 = harness error (process died early, etc).
"""
import argparse, ctypes, ctypes.wintypes as wt, os, subprocess, sys, time

class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + \
               [(n, ctypes.c_size_t) for n in (
                   "PeakWorkingSetSize", "WorkingSetSize",
                   "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                   "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                   "PagefileUsage", "PeakPagefileUsage")]

def rss_mb(handle):
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    if not ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(pmc), pmc.cb):
        return None
    return pmc.WorkingSetSize / (1024 * 1024)

def handle_count(handle):
    """A second, orthogonal signal alongside RSS -- a HANDLE leak (GPU
    fences/sync objects, GDI objects, kernel events, ...) can grow the
    process's real footprint without ever touching this game's own
    HuMemDirect* heaps (which a census specific to those heaps would show
    as flat), so this is sampled every interval too, purely for diagnosis
    -- it does NOT affect the PASS/FAIL verdict, which stays exactly the
    RSS-slope contract."""
    count = wt.DWORD()
    if not ctypes.windll.kernel32.GetProcessHandleCount(handle, ctypes.byref(count)):
        return None
    return count.value

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--args", nargs="*", default=[])
    ap.add_argument("--ticks", default="999999999", help="tick budget passed to the exe")
    ap.add_argument("--duration", type=int, default=300, help="seconds to run")
    ap.add_argument("--interval", type=int, default=10, help="seconds between samples")
    ap.add_argument("--warmup", type=int, default=60, help="seconds excluded from the fit")
    ap.add_argument("--threshold-kb-min", type=float, default=200.0,
                    help="max allowed steady-state slope (KB/minute)")
    ap.add_argument("--hard-cap-mb", type=float, default=4096.0,
                    help="instant FAIL if RSS ever exceeds this")
    ap.add_argument("--lockfile", default=None, help="acquire/release this visual-test lockfile")
    ap.add_argument("--csv", default=None, help="write samples to this CSV")
    ap.add_argument("--capture-stdout", default=None,
                    help="write the exe's stdout/stderr to this file instead of discarding it, so "
                         "a root-cause session gets both the official PASS/FAIL verdict AND e.g. an "
                         "MP6_ALLOC_CENSUS_START_TICK diagnostic log from the SAME run. Default "
                         "(unset): discarded (DEVNULL).")
    # Everything argparse doesn't recognize is passed to the exe verbatim,
    # so dash-prefixed program flags (--input-script ...) just work:
    #   leakgate.py build/mp6native.exe --duration 300 --input-script "wait:300;press:start"
    a, extra = ap.parse_known_args()
    a.args = list(a.args) + extra

    if a.lockfile:
        if os.path.exists(a.lockfile) and time.time() - os.path.getmtime(a.lockfile) < 600:
            print("[leakgate] lockfile busy -- refusing to start a windowed run"); return 2
        open(a.lockfile, "w").write("leakgate\n")
    samples = []  # (t_seconds, rss_mb)
    verdict, reason = None, ""
    proc, capture_fh = None, None
    try:
        # Popen (and the capture-file open) must live INSIDE the try/finally:
        # a Popen failure (e.g. a bad exe path) would otherwise raise past
        # the try block entirely, skipping the finally's own lockfile
        # cleanup and leaking a stale lockfile that then wrongly refuses
        # every subsequent run ("lockfile busy") until it's manually removed.
        capture_fh = open(a.capture_stdout, "w") if a.capture_stdout else subprocess.DEVNULL
        proc = subprocess.Popen([a.exe] + a.args + [a.ticks],
                                stdout=capture_fh, stderr=subprocess.STDOUT if a.capture_stdout else subprocess.DEVNULL)
        handle = int(proc._handle)
        t0 = time.time()
        while time.time() - t0 < a.duration:
            time.sleep(a.interval)
            if proc.poll() is not None:
                verdict, reason = 2, f"process exited early (code {proc.returncode}) at t={int(time.time()-t0)}s"
                break
            m = rss_mb(handle)
            t = time.time() - t0
            if m is None:
                verdict, reason = 2, "GetProcessMemoryInfo failed"; break
            hc = handle_count(handle)
            samples.append((t, m, hc))
            print(f"[leakgate] t={int(t):4d}s rss={m:9.1f} MB handles={hc}", flush=True)
            if m > a.hard_cap_mb:
                verdict, reason = 1, f"hard cap exceeded: {m:.0f} MB > {a.hard_cap_mb:.0f} MB"
                break
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
        if a.lockfile and os.path.exists(a.lockfile):
            os.remove(a.lockfile)
        if a.capture_stdout and capture_fh is not None:
            capture_fh.close()
    if a.csv and samples:
        with open(a.csv, "w") as f:
            f.write("t_s,rss_mb,handles\n")
            for t, m, hc in samples: f.write(f"{t:.1f},{m:.2f},{hc if hc is not None else ''}\n")
    if verdict is None:
        fit = [(t, m) for t, m, _hc in samples if t >= a.warmup]
        if len(fit) < 5:
            verdict, reason = 2, "not enough steady-state samples (raise --duration)"
        else:
            n = len(fit)
            sx = sum(t for t, _ in fit); sy = sum(m for _, m in fit)
            sxx = sum(t * t for t, _ in fit); sxy = sum(t * m for t, m in fit)
            slope_mb_s = (n * sxy - sx * sy) / (n * sxx - sx * sx)
            slope_kb_min = slope_mb_s * 1024 * 60
            limit = a.threshold_kb_min
            verdict = 0 if slope_kb_min <= limit else 1
            handles_fit = [hc for _t, _m, hc in samples if _t >= a.warmup and hc is not None]
            handles_note = (f"; handles {handles_fit[0]} -> {handles_fit[-1]}" if handles_fit else "")
            reason = (f"steady-state slope {slope_kb_min:+.1f} KB/min over {len(fit)} samples "
                      f"(threshold {limit:.0f} KB/min); rss {fit[0][1]:.0f} -> {fit[-1][1]:.0f} MB{handles_note}")
    print(f"[leakgate] {'PASS' if verdict == 0 else 'FAIL' if verdict == 1 else 'ERROR'}: {reason}")
    return verdict

if __name__ == "__main__":
    sys.exit(main())
