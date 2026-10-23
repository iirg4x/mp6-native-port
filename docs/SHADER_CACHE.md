# Shader (pipeline) cache

One mechanism, in Aurora's `lib/gfx/pipeline_cache.cpp`
(`third_party/aurora/`, formerly patch 0088), builds and keeps
the game's shaders on every platform: the PC app, the native
Android APK and the iPhone/web PWA. It replaces three separate pieces:
Aurora's boot-time "prewarm everything ever seen" cache, the web branch's
per-scene shader lists (wasm-pwa `0086-scene-pipeline-sets`) and the AO
pipeline prewarm (ao-mobile, "build its pipelines on the first AO-enabled
frame").

A WebGPU render pipeline is a compiled shader plus its fixed state. Building
one costs 1-3 ms when the backend has the compiled result cached, and
40-110 ms when it doesn't (PC D3D12/DXC and Galaxy S22+ Vulkan, measured
below). A draw that needs a pipeline nobody has built yet waits for it,
which is a hitch in the middle of play.

## How it works

**Keys.** Every pipeline is keyed by an xxh3 hash of its complete POD config:
the GX shader config (TEV stages, texture/vertex layout), blend, depth, cull,
polygon offset and sample count. Clear and RmlUi pipelines are keyed the same
way. Port-side renderers register a *pipeline type* (`gfx::register_pipeline_type`,
a pure function from a small POD key to a pipeline) and get the same caching;
ambient occlusion uses this (`src/gx/ambient_occlusion.cpp`,
`create_cached_pipeline`).

**Scenes.** The game names the scene it is loading:
`src/os/dll_bridge.c` calls `aurora_set_pipeline_scene(omcurovl)` when an
overlay links (the boot, menus, each board and minigame). Every pipeline a
scene draws is recorded against it, with how many frames into the scene it was
first used and whether it drew to the main render target.

**Persistence.** `pipeline_cache.db` under the graphics cache folder holds the
configs (zlib-compressed, ~170 bytes each instead of ~2.8 KB) and the scene
sets. It holds keys only, never shader source or binaries. Compiled binaries
are the backend's job: Dawn's blob cache (`dawn_cache.db`, D3D12/Vulkan/Metal)
on native builds and the browser's own shader cache on the web. On the web
the cache folder is the IndexedDB-backed `/mp6` file system.

**Seed.** `res/initial_pipeline_cache.db` ships with every build (next to
the exe on Windows, an APK asset on Android, the preloaded `res/` tree on the
web). It holds every scene's set, recorded by
`tools/generate_shader_seed.py`, which plays the boot, every board, minigame
and mode in hidden windows with scripted input. What a computer party reaches
only by chance is forced: every scene is recorded with three character sets
(`MP6_TEST_CHARS`), so all eleven playable characters are seen everywhere, and
boards run `MP6_TEST_CAPSULE_SWEEP`, where each computer turn uses the next
capsule, so every capsule effect (the Metal Mushroom's metal characters, the
creatures and objects capsules bring in) is recorded. The seed stores what the
runs reached (`aurora_meta` `coverage`), and `tests/test_shader_seed_coverage.py`
fails when a playable scene, character set or capsule is missing. Recording
runs at most 2 games at a time (`--jobs`), below normal priority, and
`--resume` continues an interrupted recording. The seed is read in place
(never copied into the local cache), so it costs nothing at startup and a new
build's seed applies at once.

**Prewarm.** When a scene starts loading, its set (this device's recording
plus the seed's, in first-use order) is queued. Native builds compile it on
background compile threads (2 on Android, 1-4 on desktop by core count,
below-normal priority) while the load screen runs. The web has no threads,
so the browser builds the set asynchronously (`createRenderPipelineAsync`) a
few per frame, only while the GPU is idle. Pipelines recorded at another
anti-aliasing setting are replayed at the current main sample count, so a
seed recorded with 4x MSAA serves FXAA or no-AA players too.

**Ubershader.** A draw whose specialized pipeline isn't built yet draws
through the GX ubershader instead (`lib/gx/ubershader.cpp`, base patch
0089): one WGSL module that interprets the draw's TEV stages, swap tables,
konst selects, indirect stages, alpha compare, fog, texgen, lighting and
vertex formats from data packed into its uniform block. It renders the same
pixels as the specialized pipeline (checked byte for byte, see below), and
the specialized pipeline replaces it once built. WebGPU has no texture
binding arrays, so it picks among the 8 texture slots with a switch, and it
needs one pipeline per fixed-function state (blend, depth, cull, sample
count, line mode): 50 states cover every pipeline in the seed. Compiling one
state costs ~1.5-12 s on PC D3D12 and ~4-5 s on the S22+, against 20-110 ms
for a specialized pipeline, so on desktop the states build in the
background at the lowest priority, most used first, and a draw that finds
neither pipeline ready builds its specialized one instead of waiting on an
ubershader state. Android and the web build none by default
(`MP6_GX_UBER_STATES=N` opts in): there the first-launch build already leaves
every recorded pipeline a few-ms cache hit, while the states' compiles halved
the S22+'s board tick rate for minutes, and a board drawn entirely through
the ubershader ran ~10x slower on its GPU (below).

**First-launch build.** The first launch on a GPU or driver, and the first
after the seed or the ubershader changes, builds every pipeline any recorded
scene uses (2256 with the current seed) behind one "Preparing shaders"
screen before the launcher. It runs on extra compile threads natively (half
the cores: 8 on PC, 4 on the S22+) and asynchronously on the web. Only the
boot set stays resident; the backend's cache (Dawn's blob cache, the
browser's own on the web) keeps the rest, so a scene's set later builds from
it in 1-3 ms a pipeline while the scene loads, and nothing compiles from
scratch mid-game. It is remembered in `pipeline_cache.db` per adapter and
driver, seed id and ubershader source. Players see it on interactive
launches; `MP6_SHADER_WARMUP=1` turns it on for scripted runs and
`MP6_SHADER_WARMUP=0` skips it.

**Black-screen hold.** On the web a module swap's black screen stays up
until the new scene's recorded set (first-use order, up to 300) is built, at
most 3 s, logged as `Pipeline scene first frames: N/M ready after X ms`. The
web has no compile threads to build the set during the load, so without the
hold its first frames would build most of it in view. Desktop and Android
don't hold: their compile threads build the set during the load, and the
ubershader (desktop) or an on-the-spot build from Dawn's cache covers the
rest. `MP6_PIPELINE_SCENE_HOLD_MS` sets the cap anywhere (0 turns it off).

**No skipped draws.** A draw whose pipeline isn't built never goes missing:
it draws through the ubershader (desktop, once its state is built) or builds
its pipeline on the spot, on every platform. The web used to skip such draws
while the browser built the pipeline asynchronously, which left new objects
and particle effects out of their first frames, so they popped in partway
through their animation (iPhone, 2026-10-21: 365-2200 skipped draws per 5 s
window, most at scene starts). After the first-launch build the browser's
shader cache already holds the pipeline. `?asyncshaders` restores skipping
for comparison; `skipped=` in the `[MP6-PIPE]` line counts them.

**Memory.** On Android and the web, a scene change releases pipelines the
new scene doesn't use and that nothing drew in the last 120 frames, and drops
prewarm still queued for the previous scene. The web prewarms at most 300 of
a scene's pipelines (Safari's GPU process died at ~500-950 live pipelines).
Desktop keeps everything.

**Safety.**
- Configs carry a version and are size-checked, so stale rows are skipped.
- A sentinel file exists while prewarm builds are outstanding. If the app dies
  during prewarm, the next launch clears this device's recording (the seed
  still applies). If it happens again, prewarm stays off until the bundled
  seed changes.
- Dawn's blob cache is cleared when the GPU adapter or driver changes (its
  entries can never be read again) or when it outgrows its budget
  (256 MB mobile, 1 GB desktop).
- Graphics caches live in `gpu-cache-v3/`, away from saves and settings.

**Offline.** The seed ships inside the preloaded `res/` bundle, which the
service worker precaches with the engine, so an installed PWA loads it with
no connection (verified in Chrome on the S22+ with the server stopped). The
boot log says `[MP6-PIPE] seed ok: <id>` or `seed MISSING: <why>`; the page
reports a missing seed as an error, and reports made offline are kept in
localStorage and delivered after the next one that gets through.

## Telemetry

With `MP6_TICK_RATE_LOG=1`, every 5-second window logs a line beside
`[MP6-TICKRATE]` (stderr; logcat tag `mp6` on Android):

    [MP6-PIPE] window=5.00s stalls=0 stall_ms=0.0 stall_max_ms=0.0 built=90 build_ms=63.5 prewarmed=90 queued=0 resident=129 tick=3254 uber_draws=0 skipped=0

- `stalls`, `stall_ms`, `stall_max_ms`: draws that waited for a pipeline (mid-frame compiles).
- `built`, `build_ms`: every pipeline created, on any thread.
- `prewarmed`: how many of those were built ahead of use.
- `uber_draws`: draws that went through the ubershader.
- `skipped`: draws left out because their pipeline wasn't built (only with `?asyncshaders`).

The first-launch build logs `[MP6-PIPE] first-launch build: N pipelines in X ms`.

Aurora also logs one line per scene:
`Pipeline scene 0x7b: 91 recorded, 91 usable, 87 queued to build, ...`.

Levers:
- `MP6_PIPELINE_THREADS=N` sets the number of compile threads; 0 builds between frames.
- `MP6_GPU_CACHE_PATH=<dir>` uses a separate cache folder, for cold-cache tests.
- `MP6_GX_UBER=0|1|2`: no ubershader, fallback (default), every GX draw through it.
- `MP6_GX_UBER_STATES=N`: how many ubershader states to build.
- `MP6_PIPELINE_WARMUP_THREADS=N`: threads for the first-launch build.

## Ubershader check

    python tools/verify_ubershader.py                  # Treetop, m602, Hidden Mile

With `MP6_UBER_VERIFY=1` Aurora alternates whole presents between the
specialized pipelines and the ubershader. With the Unlocked FPS replay on and
its interpolation off, consecutive presents of one tick draw the same
recorded GX command stream, so each such pair is the same recorded configs
drawn both ways; the tool requires them to be byte-identical. (Two separate
runs can't be compared: async loads make the game's frames differ.) Result
2026-10-20: 67 of 67 pairs identical across the three scenes, with 1-1.6
million ubershader draws per 5 s.

## Regenerating the seed

    python tools/build.py --configuration release
    python tools/generate_shader_seed.py --fresh            # all modules, ~25 min, 5 at a time
    python tools/generate_shader_seed.py --scenes w02dll    # re-record some, then merge again

Re-record when shader generation changes (the seed's configs stay valid
otherwise) and before a release. Forced board loads set up a computer-only
party (`mp6_dll_bridge_selftest_board` in `dll_bridge.c`), so boards play on
their own.

## Measurements

Boot to Towering Treetop through Party Mode at 60 ticks/s, then a minute of
board (`MP6_BOOT_TO=mdparty`, `--input-script period:90;...;pressuntil:a/ovl.start/w01dll`,
`MP6_TICK_RATE_LOG=1`), on an empty graphics cache folder (a player's first
run) unless noted. "Stalls" are the summed `[MP6-PIPE]` stalls.

**PC (D3D12), 2026-10-20**

| run | stalls (all) | stalled | worst | board start: stalls | board start ticks/s |
|---|---|---|---|---|---|
| before (master + telemetry), first run | 129 | 4.8 s | 110 ms | 97 / 3.0 s | 31.5 |
| before, second run (warm) | 1 | 0.2 ms | 0.2 ms | 1 / 0.2 ms | 60 |
| unified cache, first run, no seed | 129 | 3.3 s | 71 ms | 94 / 1.9 s | 35.5 |
| unified cache, second run (warm) | 0 | 0 | 0 | 0 (90 prewarmed during load) | 60 |
| unified cache + seed, first run | 3 | 0.1 s | 54 ms | 0 (818 prewarmed during load) | 50.7 (loading) |

With the seed, a player's first boot-to-board compiles its shaders on the
compile threads during load screens: 818 pipelines for the board, 7.1 s of
compile spread over 4 threads, none of it on the game thread.

**Galaxy S22+ (Vulkan), 2026-10-20**

Before (master + telemetry): a first run's board start had 90 stalls costing
2.3 s, and the first board window ran at 33.9 ticks/s. A second run (warm
cache) had no stalls at all, yet the board-start window still ran at
48.5 ticks/s. That dip (also seen at ~45-47 ticks/s on the AO branch) is the
board loading, not shader builds.

One-minute runs, Rashed's saved settings, Towering Treetop started directly
(`MP6_TEST_LOAD_DLL=w01dll`) at the unlocked tick rate, so ticks/s here
measure throughput, not the 60 Hz pace. Each build got a throwaway launch
first, then a cold run on an empty cache folder and a warm rerun of it:

| run | board-start stalls | stalled | worst | first 5 s ticks/s |
|---|---|---|---|---|
| before, cold | 49 | 1.67 s | 61 ms | 256 |
| unified + seed, cold | 18 | 0.41 s | 72 ms | 153, then 281, then 400+ |
| before, warm | 0 | 0 | 0 | 444 |
| unified, warm | 0 | 0 | 0 | 282, then 400+ |

At 60 Hz in the earlier boot-to-board run, a cold board start with the seed
had 8 stalls (0.2 s) at 43.5 ticks/s, against 90 stalls (2.3 s) at 33.9
before.

With the load-screen wait (same one-minute cold run, 2026-10-20 evening):
1 stall (0.1 ms) at board start, down from 18 (0.41 s). The board's first
frame waited 1.16 s for its 143 first-use pipelines, mode select's 0.55 s
for 70. The PC repeat of this run was taken while other sessions held the
CPU at 94-97% (single cold builds took seconds), so it isn't a fair number;
the wait hit its 1.5 s cap and the run carried on.

Before the wait: the board's 870 seed pipelines need two 5-second
windows on the phone's 2 compile threads, so a cold start still waits on 8-18
of them, and that build competes with the first seconds of play. Warm, the old
cache built everything at boot; the new one builds the board's set at board
load (Dawn-cached, about 1 s of CPU), which shows as the lower first-window
rate at the unlocked tick rate. At 60 Hz there is headroom for it.

**Pause menu.** Opening a board's pause menu the first time builds 7 new
pipelines (PC, warm Dawn cache, 1.4 ms).

**Ubershader and first-launch build, 2026-10-21.** Towering Treetop started
directly at the unlocked tick rate, ~40 s of board per run, on an empty cache
folder unless noted.

| run | first launch | board-start stalls | board ticks/s (5 s windows) |
|---|---|---|---|
| S22+ before (APK 40050), cold | none | 0 (held 0.67 + 1.45 s on black) | 15 (889 compiles on 2 threads) |
| S22+ build with all states, cold | 126 s (100 ubershader states in it) | 0 | 345-440 |
| S22+ ubershader states in the background, cold | 17.4 s | 27 (121 ms, max 7 ms) | 150-215 while states compiled |
| S22+ final (no states on Android), cold | 15.5 s | 0 | 195-430 |
| S22+ final, second launch | none | 0 | 180-380 |
| S22+ every GX draw through the ubershader | | 0 | 26-36 |
| S22+ specialized only (`MP6_GX_UBER=0`), warm | | 0 | 180-440 |
| PC (CPU at 90% from other work), cold | 13.3 s | 0 | 60 (paced) |

On the board the scene's 890 pipelines build from the cache in 1.0-1.4 s of
compile-thread time, and no draw went through the ubershader. The S22+
numbers are ticks at the unlocked rate, so they measure frame cost: the
ubershader costs roughly 30 ms a frame on that GPU against 2.5-5 ms.

## Merging the other branches

- **wasm-pwa**: merge, then drop `compat/aurora/base/0086-scene-pipeline-sets.patch`,
  drop the `lib/gfx/pipeline_cache.cpp` section of `compat/aurora/wasm/0001`
  (0088 now carries the web's async scheduling), keep one
  `aurora_set_pipeline_scene` call in `dll_bridge.c`, remove `build_wasm.py`'s
  separate seed preload (the seed ships in the preloaded `res/`) and
  `tools/record_scene_shaders.py`. Branch `shader-cache-web` is that merge,
  checked to apply and compile with the web build's flags.
- **ao-mobile**: drop "AO: build its pipelines on the first AO-enabled frame".
  AO's pipelines are a cache pipeline type now. Keep ao-mobile-web's d280c36
  (`AoKeyReducedDepth`/`AoKeyDebugView` in the AO key): the key is hashed as
  raw bytes, recorded and seeded as-is, and the first-launch build replays
  it, so each variant is cached separately with no format change. Older
  rows without the bits mean the full-size variant, which is what they
  built, so nothing needs invalidating. A pipeline type may register a
  `replay` callback that adapts a recorded key to the device; AO's sets its
  target formats, sample count and platform variants (compatibility mode,
  reduced depth), so the PC-recorded seed prewarms the phones' and the web's
  AO pipelines too.
- **Unlocked FPS / command processor**: 0089 touches `push_gx_draw` only
  (pipeline lookup, uniform builder choice); `end_frame` is untouched.
- **fps-retained-replay**, **code-cleanup**, **dev-unlock**: merge without conflicts.

**E. Gadd's Garage, first visit, Galaxy S22+, 2026-10-20 evening** (started
directly at the unlocked tick rate, ~1 minute):

- iPhone report that triggered this: the 8643 build then (before the
  shader-cache merge at 16:14Z) used wasm-pwa's old per-scene lists, whose
  seed held 1 Garage pipeline. The current seed holds 454.
- APK: the swap hold built 118 of 454 in its 1.5 s cap; the first board
  window then had 0 stalls (the rest finished in it on the compile threads).
- Web, Chrome, offline (server stopped): with the in-game trickle the hold
  built 22 of 300 in 3 s and the board then skipped ~3300 draws (0.5 s of
  stalls) while the rest arrived. With the hold's own budget: 300/300 in
  1.86 s on the black screen, 0 stalls in the board's first windows (13 small
  ones, 19 ms, later, for pipelines not in the seed). The 18 offline reports
  arrived when the server came back.
