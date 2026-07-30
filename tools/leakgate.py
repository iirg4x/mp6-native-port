#!/usr/bin/env python3
"""leakgate.py -- the project's standing memory-leak acceptance gate.

Runs a port executable for a fixed duration, samples its RSS (and live
handle count -- diagnostic only, does not affect the verdict) on an
interval, discards a warmup window, then fits a least-squares slope to the
steady-state RSS samples. PASS iff the slope is under the threshold. This is
the mandatory verification step for ANY change that touches allocation paths
(loaders, bridges, shims, caches) -- see docs/TESTING.md's leak gate section.

Usage (the exe path is normalized before Popen, so either slash style works):
  python tools/leakgate.py build\\mp6native_headless.exe --duration 300
  python tools/leakgate.py build\\mp6native.exe --args "--input-script" "wait:300;press:start" \
      --duration 300 --threshold-kb-min 500 --lockfile ../.visual_test.lock
  # capture the exe's own stdout (e.g. an MP6_ALLOC_CENSUS_START_TICK log)
  # alongside the official verdict, instead of losing it to DEVNULL:
  python tools/leakgate.py build\\mp6native.exe --duration 300 --capture-stdout run.log ...

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

def mem_mb(handle):
    """(working set, private commit) in MB, from ONE GetProcessMemoryInfo call.

    WHY BOTH. WorkingSetSize is the *resident* footprint and Windows trims it
    whenever it likes: measured on the windowed build, RSS is a 12-24 MB
    peak-to-trough swing snapping between a handful of discrete levels, on top
    of a one-time load step. A least-squares slope over the 25-37 post-warmup
    samples this gate collects turns that swing's PHASE into a rate -- the same
    binary produced -447.5, +655.8 and +2766.7 KB/min against a 500 KB/min
    threshold. A longer warmup does not fix it (one captured run went +656 ->
    +1196 -> +3124 KB/min as the warmup grew), nor does a robust Theil-Sen
    slope (+184 -> +1238), nor a per-bucket RSS floor (+1672 -> +2779).

    PagefileUsage -- private COMMIT, charged at allocation and released only at
    free, never trimmed -- was measured as the candidate replacement and IS NOT
    ONE: on the headless build it swings 20.3 MB peak-to-trough (414.5 - 434.9
    MB) over 90s, the same order as RSS. Both series are bounded, mean-
    reverting oscillations, so neither is quiet enough for a short window and
    swapping the verdict onto commit would buy nothing while invalidating every
    RSS baseline already recorded in docs/history. The verdict therefore stays
    on RSS; commit is sampled from the SAME call and reported so a future
    investigation can see both without re-running anything.

    What actually decides a borderline verdict is WINDOW LENGTH -- see the
    resolvable-rate line printed with every verdict.
    """
    pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
    if not ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(pmc), pmc.cb):
        return None
    return (pmc.WorkingSetSize / (1024 * 1024), pmc.PagefileUsage / (1024 * 1024))


def slope_kb_min(pts):
    """Least-squares slope of (t_seconds, mb) in KB/minute."""
    n = len(pts)
    sx = sum(t for t, _ in pts); sy = sum(m for _, m in pts)
    sxx = sum(t * t for t, _ in pts); sxy = sum(t * m for t, m in pts)
    denom = n * sxx - sx * sx
    if denom == 0:
        return None
    return (n * sxy - sx * sy) / denom * 1024 * 60

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

    # Normalize the exe path before anything else touches it. A caller who
    # types the POSIX-looking `build/mp6native_headless.exe` -- which is what
    # docs/TESTING.md's own examples showed, and what a bash-shaped habit
    # produces -- hands CreateProcess a relative path with forward slashes and
    # gets a bare FileNotFoundError out of Popen with no hint that the path
    # spelling was the problem. Resolving it here means both slash styles and
    # both relative and absolute forms work, and a genuinely missing exe is
    # reported as one readable line naming the resolved path it looked for.
    exe_path = os.path.abspath(os.path.normpath(a.exe))
    if not os.path.isfile(exe_path):
        print(f"[leakgate] no executable at {exe_path} -- build it first "
              f"(python tools/build.py [--headless])")
        return 2
    if a.lockfile:
        if os.path.exists(a.lockfile) and time.time() - os.path.getmtime(a.lockfile) < 600:
            print("[leakgate] lockfile busy -- refusing to start a windowed run"); return 2
        open(a.lockfile, "w").write("leakgate\n")
    samples = []  # (t_seconds, rss_mb, commit_mb, handle_count)
    verdict, reason = None, ""
    proc, capture_fh = None, None
    try:
        # Popen (and the capture-file open) must live INSIDE the try/finally:
        # a Popen failure (e.g. a bad exe path) would otherwise raise past
        # the try block entirely, skipping the finally's own lockfile
        # cleanup and leaking a stale lockfile that then wrongly refuses
        # every subsequent run ("lockfile busy") until it's manually removed.
        capture_fh = open(a.capture_stdout, "w") if a.capture_stdout else subprocess.DEVNULL
        # The leak slope is a per-MINUTE fit, so the run must pace in real time.
        # Automation mode now defaults the tick throttle OFF (free-run) for speed,
        # so opt this gate back into 60Hz explicitly (setdefault: a caller who
        # passes MP6_TICK_HZ still wins).
        child_env = dict(os.environ)
        child_env.setdefault("MP6_TICK_HZ", "60")
        proc = subprocess.Popen([exe_path] + a.args + [a.ticks], env=child_env,
                                stdout=capture_fh, stderr=subprocess.STDOUT if a.capture_stdout else subprocess.DEVNULL)
        handle = int(proc._handle)
        t0 = time.time()
        while time.time() - t0 < a.duration:
            time.sleep(a.interval)
            if proc.poll() is not None:
                verdict, reason = 2, f"process exited early (code {proc.returncode}) at t={int(time.time()-t0)}s"
                break
            mem = mem_mb(handle)
            t = time.time() - t0
            if mem is None:
                verdict, reason = 2, "GetProcessMemoryInfo failed"; break
            m, commit = mem
            hc = handle_count(handle)
            samples.append((t, m, commit, hc))
            print(f"[leakgate] t={int(t):4d}s rss={m:9.1f} MB commit={commit:9.1f} MB handles={hc}",
                  flush=True)
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
            f.write("t_s,rss_mb,commit_mb,handles\n")
            for t, m, c, hc in samples:
                f.write(f"{t:.1f},{m:.2f},{c:.2f},{hc if hc is not None else ''}\n")
    if verdict is None:
        rss_fit    = [(t, m) for t, m, _c, _hc in samples if t >= a.warmup]
        commit_fit = [(t, c) for t, _m, c, _hc in samples if t >= a.warmup]
        if len(commit_fit) < 5:
            verdict, reason = 2, "not enough steady-state samples (raise --duration)"
        else:
            limit = a.threshold_kb_min
            rss_slope    = slope_kb_min(rss_fit)
            commit_slope = slope_kb_min(commit_fit)
            verdict = 0 if rss_slope <= limit else 1
            # RESOLVABLE RATE. A slope fitted over a window shorter than the
            # series' own peak-to-trough swing cannot resolve a rate below
            # swing/window: under that floor the fit reports where in the swing
            # the endpoints happened to land, not a trend. This does not change
            # the verdict -- it tells the reader whether to believe it, and how
            # long a run would have to be to settle the question. A borderline
            # verdict printed with a floor above the threshold is a run that
            # must be LENGTHENED, never one whose threshold should be raised.
            span_min = (rss_fit[-1][0] - rss_fit[0][0]) / 60.0
            swing_kb = (max(m for _t, m in rss_fit) - min(m for _t, m in rss_fit)) * 1024.0
            floor = swing_kb / span_min if span_min > 0 else float("inf")
            handles_fit = [hc for _t, _m, _c, hc in samples if _t >= a.warmup and hc is not None]
            handles_note = (f"; handles {handles_fit[0]} -> {handles_fit[-1]}" if handles_fit else "")
            reason = (f"steady-state slope {rss_slope:+.1f} KB/min over {len(rss_fit)} samples "
                      f"(threshold {limit:.0f} KB/min); rss {rss_fit[0][1]:.0f} -> "
                      f"{rss_fit[-1][1]:.0f} MB{handles_note}")
            if floor > limit:
                need = swing_kb / limit if limit > 0 else float("inf")
                reason += (f"\n[leakgate] CAUTION: rss swings {swing_kb/1024:.1f} MB peak-to-trough "
                           f"over this {span_min:.1f} min fit window, so the run can only resolve "
                           f"rates above {floor:.0f} KB/min -- the {limit:.0f} KB/min verdict above "
                           f"is not separable from swing phase. Settle it with a post-warmup window "
                           f"of at least {need:.0f} min (--duration {int((need*60)+a.warmup)}), not "
                           f"with a higher threshold.")
            else:
                reason += f" [window resolves down to {floor:.0f} KB/min]"
            reason += (f"\n[leakgate] commit (diagnostic): {commit_fit[0][1]:.0f} -> "
                       f"{commit_fit[-1][1]:.0f} MB, slope {commit_slope:+.1f} KB/min")
    print(f"[leakgate] {'PASS' if verdict == 0 else 'FAIL' if verdict == 1 else 'ERROR'}: {reason}")
    return verdict

if __name__ == "__main__":
    sys.exit(main())
