# Reusable GX command bundles — private experiment, 2026-09-12

Not adopted, staged, pushed or distributed. No Android FPS gain is established.
The production renderer remains at patch 0053 (direct uniform writer).

## Why this experiment exists

The prior S22 CPU sample implicated command recording and resource tracking,
including Dawn's `APISetBindGroup` / `AddBindGroup`. The native Release build
already disables validation. Removing Debug checks is therefore not a new fix.
The earlier pipeline memo saved little and was not adopted.

This experiment reuses WebGPU command bundles rather than recomputing every
binding/draw command. It does not reduce geometry, skip effects, reduce resolution,
alter shaders or avoid updating animated buffer contents.

The pinned Dawn implementation retains bundle references, merges their resource
usage and resets pipeline/buffer/bind-group state in
[RenderPassEncoder.cpp](https://github.com/encounter/dawn/blob/266c1cf8de969a364afa4fa49311631fc99a881e/src/dawn/native/RenderPassEncoder.cpp#L326).
The port-side state reset is required, not an optional optimization. See also the
[WebGPU API contract](https://gpuweb.github.io/types/interfaces/GPURenderPassEncoder.html#executeBundles).

## Boundaries and ownership

- Up to 32 consecutive GX draws per chunk. Do not cross viewport, scissor, clear,
  custom-draw, RmlUI, debug marker or render-pass boundaries. No draw reordering.
- Explicit destination-alpha commands use the existing path; bundles inherit
  the existing viewport/scissor/blend constant, not pipeline or buffer bindings.
- Only EFB passes carrying recorded attachment formats are eligible. Private
  offscreen and public/custom passes without that metadata remain unchanged.
- Every encoded parameter is in the exact key: pipeline, texture group, uniform
  dynamic offset, draw/index/instance counts and first index. Resource handles,
  attachment formats, samples and read-only state are also included. Buffer
  contents are deliberately not cached: every frame still updates them.
- 128 entries, four-way sets, at most 16 recorded frames of inactivity. First
  sightings only record keys; a repeat must occur before compiling a bundle.
  Hash matches still require full field equality, including collision cases.
- Bundles own immutable resources. Worker shutdown precedes clearing the cache;
  cache clearing precedes destruction of the renderer's resource caches.
- Resolve every pipeline before creating a bundle. Any pending pipeline uses
  ordinary draws; never store a partial bundle that would omit future draws.
- Executing a bundle resets Aurora's pipeline/texture/index shadows and restores
  static bindings. The next ordinary draw binds its own uniform range normally.

## Evidence

Private optimized Windows/Android archives replace only `common.cpp` in the
stock 0053 archive; no other rejected experiment comes along. Both use existing
`-O2 -DNDEBUG` flags and no fast-math. Source, artifacts and link inputs are hashed.

- Cache fixture: 30,014 checks pass natively on Windows and the physical S22.
- Twelve matched PC W01 frames are pixel-identical: six AO off, six Normal AO,
  FXAA, 1920x1080. A settled 600-recorded-frame window reused 99.186% and 91.812%
  of submitted GX draws respectively. These percentages are not FPS gains.
- A separate PC build re-enables Dawn API validation. Eighteen board captures
  complete with no validation error under FXAA, MSAA 4x and SSAA 1.5x, with AO on.
  Its six FXAA frames also match the ordinary private Release prototype exactly.
- The physical S22 candidate reaches W01; a screenshot of the welcome dialog
  and four players was visually reviewed. Logs confirm command reuse. This is
  AO-off/FXAA Fast Forward correctness coverage, not normal-speed FPS evidence.
- The strict cool baseline window reached thermal status 1 and is excluded.
  The later candidate run was explicitly admitted as correctness-only. There
  are zero eligible FPS comparison pairs; do not divide these runs' rates and
  present the result as an improvement or regression.

Evidence: `build/render-bundles-20260912/{r1-verification.json,api-validation.json}`
and `build/s22-render-bundles-20260912/{comparison.json,restoration.json}`.
The original APK and player files are restored/verified; only this session's
isolated device data and cache-test executable were removed. No Google submission.

## Reproduction and remaining gate

Run scripts from the port root using `rtk proxy`. `build_candidate.py windows`
and `android` build new private copies; choose a new `--revision rN` to preserve
earlier evidence. `board_window.ps1` runs the fixed capture workload.
`verify_renderer.py` authenticates inputs and compares pixels.
`validate_backend.py`, `validation_window.ps1` and `verify_validation.py` cover
the API-validated path, not performance. The device scripts own only the named
session and must not relaunch a session whose device root was removed.

Before adoption: obtain thermal-controlled, repeated Android timings with the
same native inputs, board, graphics settings and normal-speed/unlocked workload;
verify changing scenes, live AA/resize and Android AO. High command reuse alone
is insufficient because Vulkan still records and executes the underlying draws.
