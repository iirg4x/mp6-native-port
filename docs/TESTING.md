# Testing & verification gates

Every behavior-affecting change is proven against the gates below.
Quote gate output lines verbatim in commits/reports — the point of a
gate is that its verdict is reproducible, not asserted.

## The automation contract (sacred)

Every automated gate depends on one invariant: **an automation
invocation boots the game directly, deterministically, and identically
to a build with no launcher at all.** The mode decision
(`mp6_launcher_decide_mode`, platform/gx/ui/launcher_core.cpp):

```
MP6_LAUNCHER=0                        -> automation mode (no config, no menu)
MP6_LAUNCHER=1                        -> launcher mode + menu, always
(unset) numeric tick-budget argv      -> automation mode
(unset) --input-script present        -> automation mode
(unset) MP6_AUTO_START_TICKS nonempty -> automation mode
(unset) plain interactive launch      -> launcher mode; menu unless launcher.skip
```

In automation mode the config file (`mp6_config.json`) is **never
read**, so no user setting can perturb a harness run, and **zero
`[LAUNCHER]` lines** appear in the log. Anything that weakens this
contract breaks every gate below at once — treat changes near it as
gate-affecting and re-prove.

Deterministic input for automation runs:

- `MP6_AUTO_START_TICKS=60,150,3000,...` — inject PAD START on exactly
  those ticks (focus-independent; drives the warning screen + title).
- `--input-script "wait:180;press:start;wait:60;press:a;stick:down"` —
  a full scripted session timed by the internal tick counter
  (`press`/`stick` latch for exactly one tick; steps without an
  intervening `wait` land on the same tick). Implies unlimited ticks
  unless a numeric budget is also given.
- A numeric argv is the tick budget (headless default without one: 60).

## Windows gates

**1. Both build modes compile.**

```
python tools/build.py && python tools/build.py --headless
```

**2. Headless 600-tick log diff** — the game-flow log must be identical
to a known-good baseline after normalization:

```
build\mp6native_headless.exe 600 > run.log
python tools/ua1_logdiff.py baseline.log run.log
# expect: [ua1_logdiff] PASS: ... normalized game-flow lines are IDENTICAL
```

This repository does not ship a committed baseline log. Capture one from a
known-good build the first time this gate is used
(`build\mp6native_headless.exe 600 > baseline.log`), keep it alongside your
checkout, and regenerate it only when a change legitimately alters the
boot flow -- say so loudly in the commit when you do.

**3. Straight-boot check** — a bounded windowed run is an automation
run: `build\mp6native.exe 600 > run.log` must contain **zero
`[LAUNCHER]` lines**.

**4. Leak gate** (`tools/leakgate.py`) — mandatory for any change
touching an allocation path (loaders, bridges, shims, caches, per-frame
code). Runs the exe for a fixed duration, discards a 60s warmup, fits a
least-squares slope to steady-state RSS; PASS iff slope under
threshold. Handle count is sampled as a diagnostic (never the verdict).

```
# headless (logic side, tight threshold)
python tools/leakgate.py build/mp6native_headless.exe --duration 300 --threshold-kb-min 200

# windowed (render side) -- run the scenario you changed
python tools/leakgate.py build/mp6native.exe --args --input-script "wait:300;press:start" \
    --duration 300 --threshold-kb-min 500 --lockfile ../.visual_test.lock
```

`--capture-stdout run.log` keeps the exe's own diagnostics from the same
run. Windowed RSS is noisy (OS working-set trims); a borderline verdict
deserves a same-binary re-run before being believed in either
direction. Design rules that keep the gate green: every cache has an
eviction story, every platform allocation has a named owner, no
per-tick allocation in steady state, and the in-process RSS watchdog
(`MP6_RSS_CAP_MB`, default 4096) turns a runaway into a loud, small
failure.

**5. Boot-chain screenshots** (windowed smoke) — boot with the standard
`MP6_AUTO_START_TICKS` chain, screenshot via `tools/winshot.ps1`, and
eyeball title/file-select/mode-select for visual correctness against your
own reference captures (e.g. from Dolphin or real hardware -- this
repository does not ship any reference images). A minimal smoke is "log
reaches `Call New Ovl` for the next overlay with no crash".

**6. Save integrity** — saves are Dolphin-interchangeable GCIs under
`saves/USA/Card A/`. `python tools/check_save_gci.py <gci>` validates
the box checksums; a change near the save path must not alter the
hash of an untouched save file.

## Lockfile protocol (windowed runs)

All windowed/interactive test runs on a machine share one mutex file,
`port/.visual_test.lock` (i.e. `../.visual_test.lock` from this repo, one
level above every sibling checkout) -- this is the real cross-checkout
isolation: only one windowed run happens anywhere at a time.
`leakgate.py --lockfile` takes and releases it automatically; manual
windowed runs during someone else's gate are what it exists to prevent.
If a crashed run wedges the lock, delete the file once you've confirmed
no test process is alive.

`tools/winshot.ps1 [-Process name] [-Out path.png]` (defaults:
`mp6native`, `build\winshot.png`) grabs the most-recently-started window
of a given process name via `PrintWindow`, not by matching a window
title -- it is only safe to use while the lockfile guarantees a single
windowed run exists.

## Android gate

```
python tools/gate_android.py              # tiers 1+2 (tier 2 auto-skips without a device)
python tools/gate_android.py --no-device  # tier 1 only
```

- **Tier 1 (every merge, no hardware):** both android artifacts build —
  headless (`--target aarch64-android`) and windowed (`--windowed`).
  Failure fails the gate.
- **Tier 2 (device smoke, only when hardware is attached):** the 600-tick
  headless boot on device, `ua1_logdiff`'d against the same baseline log
  used for the Windows gate above. No device / unstaged assets = graceful
  SKIP (exit 0); a present-but-diverging device FAILS.
- **Tier 3 (manual, advisory):** windowed screencap flow on a real
  device; per-device GPU variance keeps it non-blocking.
