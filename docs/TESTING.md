# Testing & verification gates

## Settings, minigame continuation, and layout regressions

After a normal build has prepared the generated headers:

```sh
python -m unittest discover -s tests -v
python -m unittest setup.lib.test_setup_regressions setup.lib.test_release_android setup.lib.test_release_windows -q
node packaging/web/test/run.js
```

The native tests exercise heap restart detection before/after allocation,
nonblocking minigame stubs, and shadow-buffer replacement with both successful
and failed allocations. Source checks ensure the unavailable upstream
`board/mgcall.c` is not compiled and the consolidated compatibility patch
retains the file-selection fixes.

In the Windows release, check AA mode transitions, 16/32 voice limits,
free-run/60 Hz timing and display selection during play. Restore original
settings afterwards. Capture and reload a fresh state from the same executable;
never bypass the build-stamp guard to load a state from an older build.

The native minigame test verifies the board's continue-in-place return contract;
it is not a full four-player, end-of-turn gameplay test.

### Engine performance regression

`tests/test_frame_resource_lifetimes.py` and `tests/test_audio_retirement.py`
compile the actual frame/cache/target and mixer/control functions against
deterministic ownership oracles. Run these when changing bindings, offscreen
targets, command storage, upload publication or audio PCM ownership.
See [the engine audit](ENGINE_PIPELINE_AUDIT.md) for coverage and limitations.
The final UI store test must preserve intermediate stencil and single-sample
color stores; command reuse must be bounded and never retain live payloads.
Audio frees and diagnostics must happen after releasing the mixer lock.

For a bounded real stream/SFX retirement stress test with isolated saves:

```sh
python tests/integration/run_engine_leakgate.py --name unique-audio-gate --duration 300 --warmup 60 --audio-stress
```

Do not report a leak rate below the gate's measured noise floor as certified,
and do not use concurrent stress/capture runs as FPS benchmarks.

`tests/test_engine_hot_paths.py` tests the actual mesh-buffer registry's hash
index, allocation bounds and lifetime operations, plus physical-window metrics
cache invalidation. For whole-frame comparisons, use the separate production-unit
benchmark in [Android and shared performance](ANDROID_PERFORMANCE.md). It aligns
runs to board-ready age and alternates baseline/current Release executables.
Do not use the board/capsule QA executable's extra diagnostic work as a proxy
for production CPU cost. Capture runs and timed runs are separate.

`tests/test_compact_uniforms.py` checks the real renderer's compact matrix upload
and index calculations against the original full palettes in 200,000 native
cases. It also requires identical Windows/Android renderer sources. The engine
benchmark accepts an audited `--renderer-checkpoint` for before/after renderer
archives and `--upload-stats` for completed-frame buffer byte counts; see the
compact-uniform section in the performance document. Never infer an Android
FPS improvement from the desktop measurements alone.

`tests/test_gx_pass_bindings.py` compiles the production GX draw routine and
checks 200,000 draws against independent draw semantics, including exact index
byte addressing, instancing, unavailable pipelines, pass resets and foreign
draws. It also checks the real dispatcher resets the local binding state around
clear, RmlUi and custom draws.

`tests/test_lazy_pipeline_factory.py` compiles the production pipeline lookup.
It requires zero allocations and configuration copies over 100,000 ready-cache
lookups, and exercises deferred ownership after the caller's configuration is
destroyed, pending-entry reuse, required-pipeline waits, threadless fallback,
compile budgeting and first-use persistence. Its worker schedule is deterministic;
real rendering smoke tests still cover the production worker and backend.
It also extracts the production ready-cache entry and GX layout lookup, checking
100,000 layout hits without repeated analysis, allocations or configuration
copies. Cases cover dirty output storage, alignment changes, cache recreation,
prewarmed pending entries, threadless compilation and shutdown wakeup. Layout
analysis is mocked here to isolate cache semantics; `test_compact_uniforms.py`
separately exercises the real layout calculations and upload indexing.

### Android touch-controller regression

`python -m unittest discover -s tests -p test_touch_controller.py -v` drives the
actual SDL adapter and ImGui draw code on the host. It checks independent sticks,
digital directions, all buttons, analog/click triggers, multi-device finger IDs,
fast taps, cancellation, lost focus, save-state input reset, menu/console blocking,
safe-area scaling, hidden controls, editor save/cancel and config round-trips.
The real renderer's triangles produce visual fixtures under
`build/touch-controller-qa/` for landscape, portrait, pressed controls and editing.
It uses the locally built Android SDL/ImGui headers and host Zig compiler.

These tests do not certify Android touch latency or ergonomics on real hardware.
On a device, verify three-finger button combinations do not open Settings, then
background/resume and change orientation while holding controls. No button should
remain held. Check layout editing and slider pressure with physical thumbs too.

### Capsule crash regression

`tests/test_capsule_effects.py` compiles the actual particle-system types and
animation-grid setter from the port-patched `capevent.c`. It verifies that grid
setup changes only the intended fields. The old GameCube-sized padding put
`gridNum` over the high half of the native draw callback; four frames produced
the reported `ev_CapEffDraw` crash at `0x400000000`. Compile-time offset and size
assertions now keep both views of that allocation compatible on every target.

For a Windows GPU integration run, first build the normal release, then run
these commands with the same disc/include environment as that build:

```sh
python tests/integration/build_capsule_route.py
python tests/integration/check_capsule_route.py
```

The fixture adds one Red Mushroom to the first human player's inventory. The
real menu, consumption, effect rendering and cleanup must finish, arm double-dice
mode, and continue another 600 ticks. This does not assert the completed roll,
movement, camera validity or turn end. Generated fixture source and the separate
executable live under `build/capsule-route/`; play uses a temporary save folder.
Neither normal release executables nor dependency sources are changed.

Adding `--unfixed` to both commands builds a separate negative control using
only the original `capevent.c` in place of the fixed one. That run must reproduce
the exact callback crash. This is an intentionally crashing test, never a build
to distribute. Android compiles the same fixed structs; the Windows GPU result
does not substitute for testing the APK on a device.

### Board-cycle and capsule outcome diagnostics

The current capsule sweep separates self-use, all 13 placements, landing,
pass-through traps, longer trap movement, Bullet Bill's real post-roll handler,
Boo/flashlight/night house, ownership transfer, and special events:

```sh
python tests/integration/run_capsule_matrix.py --name capsule-audit --frames 120
python tests/integration/render_capsule_matrix.py capsule-audit place
```

Build `build_board_qa.py` first using the normal Release build environment.
There are 46 diagnostic cases; `--only use-00,place-21` selects a subset.
Each run retains captures and four-player state snapshots. Automated gates check
the requested handler and its return, camera validity, completion, captures, and
completed turns for movement cases. They do **not** certify every random branch,
all character animations, or visual equivalence to original hardware. Review
captures and coin/star/inventory/space/dice changes as described in `BOARD_QA.md`.
The undecompiled minigames remain explicitly stubbed; their return is not a
minigame-play pass. Empty capsule slots are not fabricated into active effects.

An optional `--seed-state` must be a same-executable isolated QA state captured
**before any turn begins**, with its companion log/result. A fixed save tick does
not guarantee this: inspect the log. The runner rejects mid-turn audit seeds,
which can retain the prior run's fixture-local host pointers. Never bypass the
save-state build guard or use a player's save to manufacture test success.

See [Board QA](BOARD_QA.md) for current failures, coverage and reproduction
commands. `tests/integration/build_board_qa.py` builds a separate Windows GPU
executable with turn, dice, inventory, coin, space and camera probes. The normal
release and read-only dependencies are not edited. `run_board_qa.py` uses a fresh
per-run directory under `build/board-qa-runs/`, retains the log and records the
executable hash and termination reason in `result.json`.

These are diagnostic runs, not automatic gameplay approvals. The runner's exit
status reports whether the harness ran; `result.json` contains the game process
exit status. An event returning, a normal process exit, or a completion marker
does not imply its effect was correct or the camera remained valid. A timeout
can mean the A-only script needs directional input; inspect the window before
calling it a deadlock. Do not run these alongside a performance gate.

Every behavior-affecting change is proven against the gates below.
Quote gate output lines verbatim in commits/reports — the point of a
gate is that its verdict is reproducible, not asserted.

## The automation contract (sacred)

Every automated gate depends on one invariant: **an automation
invocation boots the game directly, deterministically, and identically
to a build with no launcher at all.** The mode decision
(`mp6_launcher_decide_mode`, src/gx/ui/launcher_core.cpp):

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
