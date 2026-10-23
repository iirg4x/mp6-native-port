# XeGTAO implementation plan

This is the current implementation and decision index. The complete dated
experiment log is retained in
[`XEGTAO_IMPLEMENTATION_PLAN_lab-notebook-20261008.md`](archive/XEGTAO_IMPLEMENTATION_PLAN_lab-notebook-20261008.md).

## Decision boundary

- The renderer retains the port-owned custom GTAO path as its legacy path.
  The selected XeGTAO baseline is Intel revision
  `a5b1686c7ea37788eeb3576b5be47f7c03db532c`, under the MIT license. The
  upstream HLSL remains unchanged and is translated offline with pinned Slang
  `v2026.19`.
- Windows PC uses XeGTAO by default at High quality, two denoise passes,
  full-resolution working size and `R32_UINT` output. Android and other
  non-Windows targets retain their prior policy; Android production adoption
  is not accepted.
- The integrated path keeps upstream math, normal reconstruction, five depth
  MIPs, the main pass and spatial denoising. It starts without TAA, uses
  `NoiseIndex = 0`, and keeps FP32 resources. The final `R32_UINT` result is
  consumed directly by the existing RGB-only EFB continuation. This removes
  the old RGBA16F signal/conversion path, but the result is mathematically
  equivalent rather than promised bit-identical because the removed half-float
  rounding no longer occurs.
- The opt-in selector accepts
  `MP6_AO_XEGTAO_RESOLUTION=full|half`; `full` is its default. Half resolution
  uses ceil-halved dimensions and plane-aware reciprocal-depth reconstruction.
  Its final eight-test fixture passes on Vulkan and D3D12, including the former
  steep cases, but half remains opt-in with no production-quality or
  performance acceptance.
- Ultra uses exactly 9 slices × 3 steps × 2 sides in the bridge and three
  denoise passes. Ultra remains opt-in on all targets; High/2/full is the
  Windows default.

## Verified bridge and desktop results

- Rebuild the offline bridge with
  `C:/Python313/python.exe tools/compile_xegtao.py --timeout 30`. It writes
  local-only `build/ao-upstream-toolchain/xegtao`, compiles all eight stages with pinned
  Slang and keeps the Xe source hashes unchanged. Generated checked-in output
  is `src/gx/xegtao/generated_shaders.hpp`.
- The shared 64×64 Hilbert LUT is uploaded once for the `Pipeline` lifetime.
  Direct-shader versus LUT comparison passed all 90 exact parity cases per
  backend on Vulkan and D3D12; the cases cover sizes, qualities, phases, flat
  and clear inputs, AO, visibility, normals and all five depth MIPs.
- The authenticated Oct 6 Windows smoke receipt records the then-current
  binary below; it is historical, not a current-release identity:
  Local-only historical build path: `build/release/mp6native.exe`, built
  2026-10-06, SHA-256
  `d8191b2704a71da43aa261f9074b2400ec45a753edce8816a6be23616c742d35`,
  size 11,144,704 bytes. Its recorded smoke exited 0 with GPU validation and
  no panic, fatal or validation errors; this is bounded integration smoke,
  not full-gameplay or performance acceptance. The Oct 6 hash is historical.
  A subsequent decomp-update rebuild is recorded in
  local-only `build/decomp-52a0209-windows.log`. The old build path is now a moving
  artifact; use the Oct 6 smoke receipt, not that path, for identity.
- The retained desktop visual fixes include receiver-only protection for the
  exact retail ground-daisy identity, removal of overlapping `haikei10` and
  `haikei17` extensions, the current nonexpansive response, registered sky
  footprints and targeted tree-trunk receiver protection. AA0, AA1 and AA4
  receipts show the scoped trunk creases removed. These are identity- and
  scene-scoped visual results, not universal full-scene proof.
- Identity-first grounding is retained because it avoids unnecessary validation
  for unrelated meshes and is test-backed. Its private timing comparison was
  inconclusive under the recorded background load; no speedup or FPS claim is
  made.

## Android acceptance: targets, not results

Android execution was explicitly deferred. Historical S22+ and Tab S10 Ultra
smoke/component results, desktop shader timings and bounded native captures do
not satisfy the device protocol. No accepted matched sustained AO-on-device
evidence currently exists.

| Gate | Required target |
| --- | --- |
| Whole rendered-frame GPU span, AO on | Median ≤ 14 ms; p95 ≤ 16.67 ms |
| Complete AO-on minus AO-off GPU span | Median difference ≤ 1.5 ms; p95 difference ≤ 2.0 ms |
| Additional active CPU work | Median difference ≤ 0.25 ms; p95 difference ≤ 0.5 ms |
| Sustained 60 Hz presentation | ≥ 3,540 unique presents per 60 s; intervals >25 ms ≤ 1% |

Measure the complete game/overlay frame at a recorded display mode with AO2,
identical non-AO settings and a 1080-pixel short-axis target; report native
resolution tablets separately. Include preparation, depth copies/MIPs, AO,
denoise, foreground, composition, submission and presentation. Use matched
ABBA cycles after warmup plus an uninterrupted 10-minute AO-on session, actual
compositor timestamps and thermal records. A missing statistic is unmeasured,
not passed.

The full goal is blocked, not complete, until the deferred Android quality and
performance gates resume and pass. No result here authorizes a lower-quality
mobile default or supports a 2× claim.

## Archive navigation note

The archive retains the historical narrative; repository-relative links inside
it are rebased for docs/archive/, while build/ receipts remain local-only text.

## Evidence navigation

- Local-only bridge/LUT receipt directories (not web links):
  `build/ao-upstream-toolchain/xegtao/` and `build/ao-hilbert-comparison/`.
- Local-only Windows smoke receipt (not a web link):
  `build/board-qa-runs/ao-release-identity-fastpath-20261006/result.json`.
- Current AO contracts and legacy-path context:
  [`AMBIENT_OCCLUSION.md`](AMBIENT_OCCLUSION.md).
- Android/shared performance index:
  [`ANDROID_PERFORMANCE.md`](ANDROID_PERFORMANCE.md).
- Full dated narrative, including rejected experiments and exact historical
  receipts: [`archive/XEGTAO_IMPLEMENTATION_PLAN_lab-notebook-20261008.md`](archive/XEGTAO_IMPLEMENTATION_PLAN_lab-notebook-20261008.md).
