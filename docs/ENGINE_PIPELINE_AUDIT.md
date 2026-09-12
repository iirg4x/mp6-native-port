# Engine pipeline audit — 2026-09-10

## 2026-09-12: stop cache-first work; profile the actual Release workload

The user explicitly rejected further speculative optimization. The render-bundle
ABBA diagnostic reduced CPU command encoding by roughly 70%, but its only timing
window changed thermal cohort and is **not an eligible FPS comparison**. No
bundle/cache experiment is adopted. Do not keep polishing it without evidence
that it limits the complete frame.

New app-only S22 profiles use the unchanged production 0053 native ELF, not the
memo or bundle diagnostic. Settings match the saved Fast Forward/FXAA/AO-off
configuration; AO-on is a separate run. Resolution is 2340x1080, shadows 4x,
unlocked rendering, VSync off (the surface chooses Mailbox). Exact ELF build IDs,
APK/native hashes and raw samples are checked by
`tests/integration/profile_android_release.py`.

- AO off: 6,668 CPU samples. The producer owns 58.211% of sampled active CPU
  cycles; the renderer 35.905%. FIFO drain occupies 31.861% of producer cycles,
  and includes draw preparation, copies, configuration and bindings. This is
  not 31.861% parser overhead that can simply be removed. Hu3D drawing and
  native command preparation must be traced together.
- Producer and renderer each have over 99% of their sampled cycles on CPUs
  4–6. The current evidence does not support a little-core-placement fix.
- AO on: 4,745 CPU samples. Settled GPU reports are typically about 9.3 ms,
  including ~4.3 ms EFB (meshes **and AO composition**) and ~2.18 ms GTAO.
  There are outliers. The remaining depth/blur/present passes also matter.
  A 200 FPS frame budget is 5 ms; CPU command caching cannot eliminate this
  GPU work.
- CPU sampling affects timing. Inclusive CPU percentages overlap and are not
  wall time, critical-path proof, or before/after FPS evidence. Full-frame
  scheduling/wait attribution and matched complete-frame verification are
  required before adoption. Do not replace this with another desktop-only or
  per-component speedup claim.

Evidence: `build/s22-bundle-abba-20260912/{root-ff-off,root-ff-ao}/` and
`comparison.json`. The misleading normal-speed/AO-off workload used by the
abandoned ABBA run is kept separately. Production code remains unchanged.

The original installed APK is restored and hash-verified; player settings,
memory-card bytes and save inventory are unchanged. Only this session's private
device profiling root and temporary sampling tools were removed. USB stay-awake
remains enabled at the user's request. No upload, push or Google submission.

## 2026-09-12: reusable command bundles (experimental, not shipped)

An isolated render-submission prototype now reuses exact consecutive GX command
chunks through WebGPU bundles. It targets repeated binding/command encoding and
resource tracking identified in the S22 CPU sample, not shader quality or game
behavior. The production build already disables API validation; that is not a
new optimization. No decomp source or production renderer was modified here.

The bounded, worker-owned cache preserves draw order and all pass/state barriers,
retains resource ownership, keys every encoded argument and attachment mode, and
restores the state that bundle execution clears. Dynamic geometry/uniform data
continues to update normally. Unknown pass formats and unsupported draw state
use the existing renderer. See
[`tests/experiments/render_bundles/README.md`](../tests/experiments/render_bundles/README.md)
for implementation boundaries, reproducible tests and primary API references.

- Private Windows and Android Release builds succeed; production hashes stay
  unchanged. The cache fixture passes 30,014 checks on each physical platform.
- Twelve fixed-seed PC W01 pairs are pixel-identical with AO off/on. Settled
  command reuse is 99.186% / 91.812% respectively, **not an FPS measurement**.
- Eighteen additional PC captures pass with Dawn API validation enabled across
  FXAA, MSAA 4x and SSAA 1.5x. The six FXAA frames match the Release prototype.
- Android reaches W01 with FXAA/AO off; its screenshot and command-reuse logs are
  checked. The strict baseline thermal window failed, and the candidate was
  explicitly correctness-only. There are **zero eligible FPS comparison pairs**.
- The original S22 APK is restored, saves/settings are unchanged, and USB
  stay-awake remains 2. Only isolated profiling data was removed. No upload or
  Google submission occurred. Production remains at patch 0053.

Evidence is in `build/render-bundles-20260912/` and
`build/s22-render-bundles-20260912/`. Normal-speed Android FPS, sustained thermal
behavior, Android AO and dynamic scene/AA/resize coverage remain adoption gates.

## 2026-09-12 follow-up: copy removal and batching boundaries

The shared direct-uniform writer is adopted as patch 0053. It removes the
per-draw scratch-to-mapped-buffer copy without changing draw order, uniforms,
shader code or quality. In-process paired S22+ packing time fell about 41%;
separate whole-game runs were too variable to establish an FPS improvement.
See `ANDROID_PERFORMANCE.md` for the exact scope, checks and artifact hashes.

The next batching work must address real state/geometry differences:

- `lib/gx/command_processor.cpp::draw_prim` already merges consecutive compatible
  triangles into contiguous vertex/index ranges. It requires unchanged GX state,
  matching vertex format, no line/point expansion and a 16-bit vertex-count limit.
  The fixed board capture already combines about 192.9 draws/frame, leaving about
  298.8 submitted GX draws; these counts are PC workload evidence, not Android FPS.
- `push_gx_draw` builds a different dynamic uniform range for each remaining draw.
  `pipeline.cpp::render` binds that range per draw. Reusing the pipeline or texture
  binding alone does not make different transforms/material uniforms mergeable.
- The only existing `instance_index` branch in the generated vertex shader is
  line/point quad expansion. Ordinary meshes use `instanceCount = 1`. True mesh
  instancing needs per-instance data and evidence of identical geometry/material
  groups; merely increasing this count would repeat the wrong transform.
- Passes, viewport/scissor changes, custom draws, texture changes and transparency
  order are correctness boundaries. Pipeline, texture and whole-index-buffer
  binding reuse already exists; do not count it again as a new optimization.

### Compact mesh instancing experiment (not shipped)

The draw census is now complete for 21 warmed, fixed-seed W01 snapshots. Each
contains 299 submitted GX draws, 29,996 display-list/immediate vertices and
14,230 triangles. Median adjacent pairs: 89 with equal pipeline/texture/alpha/
format, 30 also with equal geometry, and 19 with only model-transform changes.
Only two pairs have identical complete uniforms. The non-adjacent opportunity
count is an upper bound, not permission to reorder translucent surfaces.

An isolated implementation now instances **adjacent equal-topology draws**,
including different geometry addresses, rather than requiring identical meshes:

- Preserve pipeline, texture bindings, destination alpha, vertex format/count,
  index count/topology, viewport and all other uniform bytes exactly.
- Put each instance's vertex address, 12 attribute-array addresses and original
  position/normal matrix bits in a 160-byte storage record. No float conversion,
  matrix calculation, texture change or fragment-shader change is introduced.
- Limit a group to eight instances. Preserve primitive order and stop at pass,
  viewport/scissor, custom-draw, clear and copy boundaries. Line/point expansion,
  matrix palettes and unsupported uniform layouts use ordinary draws.
- Keep ordinary draws while the specialized pipeline is pending. Do not skip a
  visible draw to wait for an optimization. Instanced groups cannot enter the
  old ordinary-index append path.

For the Android Vulkan backend, instance ordering is consistent with the
specification's [primitive order](https://docs.vulkan.org/spec/latest/chapters/drawing.html#drawing-primitive-order)
and [ordered attachment operations](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-order).
This does not justify grouping non-adjacent objects or moving draws across a
render-pass boundary.

The first prototype was rejected: it compared uniforms/indices in the mapped
GPU upload memory. PC submit time rose from roughly 0.36 ms to 7.68 ms even
though the images matched. The revised prototype compares CPU snapshots and
an index shadow, only writing the upload buffer; submit returned to roughly
0.35 ms. Its two recent uniform snapshots, one active group template and one
frame's index shadow have bounded lifetimes. Native tests explicitly poison the
simulated GPU upload data and verify that comparisons read CPU snapshots.

Measured draw submissions for the fixed scene:

| Setting | Production 0053 | Experimental instancing |
| --- | ---: | ---: |
| AO off, FXAA | 298.794/frame | 241.950/frame |
| AO on, FXAA | 323.794/frame | 256.950/frame |

These are about 19% and 21% fewer submissions, **not measured Android FPS
improvements**. Final PC capture pairs are pixel-identical for 12 frames with
AO off and 12 with AO on, matched by board age at 1920x1080. Single capture-run
timings are mixed: the final AO-off pair was 0.690 versus 0.732 ms, while AO-on
runs varied in both directions. Fewer submissions alone does not establish a
speedup. The extra CPU snapshot work and per-instance storage reads must be
measured on the target device before adoption.

Validation and ownership:

- Windows Release experiment built; Android Release experiment linked against
  the verified 177-TU `-O2` profile, with no fast-math or quality reductions.
- 8,092 native byte/record cases plus eligibility, ordering, capacity and pending
  pipeline guards passed on Windows and physical S22+ (SM-S906E).
- A second native oracle passed 1,000 CPU-snapshot lifecycles, poisoned upload
  memory and reused frame/range IDs after renderer restart on both platforms.
- The incremental experimental patch replays in memory through the project's
  patch engine to the exact tested C++ text on both source trees. Git's check
  also passes with `--ignore-space-change`; source files mix LF and CRLF.
- All production source/link-input/artifact hashes remain unchanged. During this
  initial native-oracle phase no APK was installed. The standalone S22+ test
  executables were removed. The subsequent isolated APK comparison is below.

Durable candidate and tests: `tests/experiments/compact_instancing/` (outside
the automatic `compat/aurora/base` patch set). Local evidence:

- `build/batch-census-20260912/report.json`
- `build/compact-instance-20260912/verification.json`
- `build/compact-instance-20260912/final-{off,ao}-comparison.json`
- `build/compact-instance-20260912/{provenance.json,android/provenance.json}`
- `build/compact-instance-20260912/android/s22-native-oracles.json`

Candidate hashes (not release artifacts):

- Windows EXE: `5bd28dfd9b693303a42816bc4a4e30a32bb8465ebcda993842ed72c56b77b167`
- Android stripped library: `8a1c46702c7daa8871f01eea3972ce86e6ceec8a10a12f1a24548db57af7e13c`
- Experimental patch: `d15b79d3b9e18fcd4eb544abbe9549f82bb3c62c1c3b05dc893a6a21ab7818cd`

Local reproduction helpers: `build/recovery/build_compact_instance.py`,
`build_compact_instance_android.py`, `run_compact_instance.ps1`,
`compare_compact_instances.py`, `run_compact_instance_oracles_s22.py` and
`verify_compact_instance.py`. Full builds use the already-verified fixed-seed
benchmark objects; they do not regenerate or modify the shared decomp.

### S22+ game comparison and deferred-upload revision (not adopted)

The physical S22+ comparison did **not establish an FPS gain**. These are three
complete five-second throughput windows per run, at the W01 welcome scene with
Fast Forward, FXAA, AO off, unlocked presentation and unchanged graphics settings:

| Run, in order | Median presents/s | Median CPU submit ms | Thermal statuses |
| --- | ---: | ---: | --- |
| Production 0053 baseline | 198.401 | 1.181 | 0 throughout |
| CPU-snapshot instancing | 196.133 | 1.323 | 0 throughout |
| Production 0053 repeat | 204.651 | 1.154 | 0 throughout |
| Deferred-upload instancing | 185.918 | 1.352 | 0, then 1 |

The fourth run is **not a clean regression estimate**: it began warmer (AP
38.5 C / SKIN 37.1 C, versus 36.6 C / 36.0 C for the preceding baseline) and
reported thermal status 1. Even a status-0 admission does not establish
equal temperatures or clocks. These are not normal-speed interpolation, AO-on,
sustained-load or Mali performance results. No prototype is enabled in production.

Source tracing also found redundant work inside the prototype: it uploaded the
complete uniform for every candidate, even when that candidate became an instance
of the preceding draw and never used that uniform. A second isolated revision:

- Defers compact uniforms in CPU storage until the batching decision, commits
  only unmerged draws and discards merged payloads without GPU allocation/copy.
- Keeps line/point and matrix-palette draws on the production direct writer.
- Resolves every pending token before publishing a draw, retains exact fallback
  bytes and never compares write-combined GPU upload memory.

This revision passes 8,092 eligibility/packing cases and 1,000 commit/discard,
direct-fallback, poisoned-upload and renderer-restart cases on Windows and S22+.
All 24 full-frame PC comparisons remain pixel-identical with AO off/on. Its
10-section patch replays exactly against both port-local renderer source trees;
production artifacts still match the adopted 0053 hashes. This proves correctness
only for the tested scope, not an Android FPS improvement or full-board coverage.

The profiling harness now supports explicit `--max-start-ap-c` and
`--max-start-skin-c` admission limits, refusing absent/nonfinite readings. Strict
comparisons can use `--max-window-status 0`; a status-1 transition ends the window
and marks its thermal evidence invalid. The summary binds thermal evidence to the
selected launch/resume, rejects over-limit or ambiguous windows, and records its
hash. Older logs lacking thermal evidence remain labeled unavailable rather than
being claimed thermally matched. All 22 profiling regression tests pass.

Player settings and save inventory were verified byte-for-byte unchanged before
cleanup. The exact owned private profiling directory and standalone test binaries
were removed. The original installed APK was restored and SHA-256 verified as
`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
The fresh MP6 Play Protect restore dialog was answered **Don't send** as explicitly
requested. No security setting was disabled and no APK was uploaded or pushed.
USB stay-awake remains 2 while this optimization work continues; the game is closed.

Evidence and reproduction:

- `build/s22-compact-instance-20260912/comparison.json` freezes the exact log,
  request, resume and thermal-window hashes, APK/native identities and limitations.
- The same directory contains `restoration.json`, `player-data-verification.json`
  and `play-protect-choice.json` for device handback.
- `build/compact-deferred-20260912/verification.json`, `renderer.patch` and
  `{off,ao}-comparison.json` preserve the unadopted revision and image checks.
- `build/recovery/build_compact_deferred{,_android}.py`,
  `test_compact_deferred.py`, `verify_compact_deferred.py` and
  `report_compact_phone.py` reproduce the local checks without modifying decomp.
- Deferred Android library SHA-256:
  `1ffc797b86fbbaea045a4cfcebabeb96f20a29e20bf44336cd5805379afc828d`.

Remaining: materially reduce the renderer's CPU state preparation and/or shader
cost before adopting instancing; retest under explicit matched-temperature limits
and broaden scene/transparency coverage. Production remains 0053. Fewer draws or
fewer uploaded bytes alone are not sufficient evidence to ship a change.

### FIFO padding and upload census (candidate, not adopted)

Two isolated, fixed-seed W01 censuses cover 3,300 warmed frames each. Both use
the current renderer and authenticated benchmark game objects. Instrumented
timings are not performance evidence. Six census captures match the production
reference exactly; native tests verify the counters detect identity reuse,
mutation, invalidation epochs, size changes and capacity limits.

- Median indexed-array uploads: 446 / 534,042 bytes per frame, with 172 explicit
  invalidations. There were no repeated uploaded addresses within a frame in
  this window, even across invalidations. An address cache was therefore not
  added. This does not rule out other scenes or cross-frame static-data reuse.
- Median command dispatches: 19,558 per frame, including 6,172 no-op padding
  bytes, 6,874 BP writes and 3,725 XF loads. Of the BP writes, 6,125 were already
  rejected by the existing repeated-state check; every measured non-mask BP
  write used a full write mask. Those repeated-state checks are not a new fix.
- Offline instruction-level analysis verifies the existing S22 recording's
  native build ID before decoding sampled addresses. It identifies dispatch
  and BP-mask/equality processing among the FIFO self-costs. This recording
  predates 0053; it is not a fresh FPS or GPU measurement.

A narrow candidate now skips consecutive NOP bytes in eight-byte blocks,
followed by a checked tail, instead of dispatching every padding byte. It uses
bounded `memcpy` loads at any alignment and preserves the existing 0x00..0x07
NOP semantics. The first real command and all command payloads remain untouched.
No state write, draw, transparency ordering, shader or quality setting changes.

The candidate builds in Windows and Android Release and passes 2,060,196
byte-oracle / protected-page-boundary cases on Windows and physical S22+.
Six 1920x1080 capture pairs match production exactly with AO off and Normal AO.
An initial Strong-AO capture was incorrectly compared to the Normal-AO reference;
that comparison was rejected, rerun at matching settings, and the verifier now
checks request settings before comparing pixels. Capture-run timing is not used
as a speedup claim, especially against instrumented census executables.

The patch remains outside the automatic production patch set in
`tests/experiments/fifo_nop/`. Both source-profile replays and all production
artifact/input hashes pass. The S22 standalone test was removed after execution;
the installed APK, player files and settings were not changed in this phase.
Nothing was submitted to Google, packaged for distribution, uploaded or pushed.
This standalone phase is distinct from the subsequent APK comparison below.

Evidence:

- `build/array-census-20260912/report.json`: exact counts, source/log hashes and
  census image checks; `build/fifo-census-20260912/provenance.json`: FIFO build.
- `build/s22-thinlto-20260912/baseline-off-repeat/cpu-instruction-hotspots.json`:
  recording hash, native build ID/hash, weighted sample addresses and disassembly.
- `build/fifo-nop-20260912/verification.json`: candidate replay, native tests,
  matching capture settings/images and unchanged production inputs.
- Candidate Windows EXE SHA-256:
  `4a020b3dcb4593d98da96303856c24b9acf3ff2e51623b2dd1b85b1b5f168aed`.
- Candidate Android library SHA-256:
  `7484da9e52ea43e8fa0d9d21669a4d58e79441db825843695b1a2154611e0d2d`.

#### Subsequent physical S22+ comparison: not adopted

The isolated baseline/candidate/baseline APK sequence used the same FXAA,
AO-off, shadow-quality-4, uncapped Fast Forward settings. Each measured window
started at thermal status 0, AP <=37.3 C and SKIN <=36.3 C, and all sampled
statuses within admitted 20-second windows remained 0. Warm-up and cooling were
separate from the frozen measured logs. These are short throughput tests, not
sustained normal-speed interpolation or Mali results.

| Run | Original median presents/s | Original mean | Complete rate reports |
| --- | ---: | ---: | ---: |
| Baseline 1 | 199.872 | 201.475 | 3 |
| NOP candidate | 196.348 | 217.634 | 4 |
| Baseline 2 | 179.120 | 179.525 | 4 |

The candidate's 285.524 presents/s report overlaps an earlier part of the board
opening than the first baseline's admitted reports. The original 600-tick
warm-up cutoff is not sufficient evidence of a stationary scene. A separate,
explicitly post-hoc sensitivity analysis requires complete five-second windows
starting at least 1,800 ticks after `w01.live` (and after the resume cutoff).
Mean tick rates then become 195.121 / 195.004 / 179.792, with 2 / 3 / 3 reports.
Both analyses are retained. The stricter threshold is not independently proven
to define a fully stationary scene, and baseline variation still prevents a
precise speedup/regression conclusion. Do not adopt on a favorable isolated mean.

`build/s22-fifo-nop-20260912/comparison.json` verifies the hashes of the frozen
logs, launch settings, exact resume markers and thermal windows. The original
APK (`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`)
was restored after testing. Player settings/card files and save inventory were
verified unchanged, the private session root was removed, and USB stay-awake
remained 2 as requested. `restoration.json` records handback. A fresh MP6 Play
Protect prompt during restoration was declined with **Don't send**. No upload,
push, decomp modification, or production artifact adoption occurred.

### Whole-register FIFO writes (prototype, not adopted)

Graphify's bounded source-tracing fallback (no index present) followed the
register producer through `__gx.h` into `fifo.hpp`. BP, CP and scalar-XF commands
are currently emitted as two or three inline field appends. The prototype in
`tests/experiments/fifo_packets/` reserves complete 5/6/9-byte commands once,
reducing repeated capacity checks and cursor updates. It changes neither
register values nor command order. Short-buffer cases retain the original
scalar fallback, including partial display-list overflow behavior. It does not
skip GX register writes or lower rendering quality.

Actual source-header and macro extraction tests pass 433,312 packet writes on
Windows and physical S22+: alignment, canaries, overflow, allocation growth,
mixed streams and independently specified endian bytes. The Android binary was
standalone; it did not replace the restored APK and was removed after testing.
An alternating-order 12-pair writer microbenchmark was faster in all pairs:
S22+ median 9.462 -> 7.526 ns/packet, median paired reduction 20.46%; Windows
paired reduction 3.66%. These are synthetic-order writer-kernel measurements,
not whole-game FPS results; most engine/rendering work is absent. Thermal
reports, exact compiler/input/binary hashes and raw pairs are retained under
`build/fifo-packets-20260912/{native-tests,kernel-comparison}.json`.

Private Windows and Android Release builds now recompile all 16 GX translation
units consuming the changed macros with the original `-O2 -DNDEBUG` flags. The
callsite audit found precomputed values/fields, not calls or increments whose
evaluation order could change. Twelve fixed-seed PC W01 frame pairs (six AO off,
six Normal AO) are pixel-identical, including recorded/replayed rendering.
`renderer-verification.json` binds the compiler commands, source/input/artifact
hashes and capture provenance, and verifies production files stayed unchanged.
This is not exhaustive display-list, Android visual, or performance proof.

The subsequent integrated S22+ test used normal 60 Hz simulation plus unlocked
rendering (not Fast Forward), FXAA and AO off. One short baseline/candidate pair
measured median 190.766 versus 185.684 rendered presents/s, four complete rate
reports each. Warm-up and cooling were excluded; both admitted windows started
at AP/SKIN 35.8 C and sampled thermal status 0 throughout. Temperatures/clocks
were not locked. This does not establish a repeatable FPS gain or a precise
regression estimate; the candidate remains unadopted. Android screenshots show
the same welcome scene but are not tick-aligned pixel proof.

Frozen source/APK/log/settings/resume/thermal evidence and handback are in
`build/s22-fifo-packets-20260912/{comparison,restoration}.json`. The exact original
APK was restored; settings/card bytes and save inventory are unchanged. The
private device root was removed. USB stay-awake remains 2. The fresh restore
prompt was declined with **Don't send**; nothing was submitted, uploaded or pushed.
The production patch set still ends at 0053. Small arithmetic-only BP-mask
fast paths and producer-side state suppression were inspected but not added:
display-list replay and BP/XF state aliases require explicit coherence proof.

### Ready-pipeline lookup memo (prototype, not adopted)

Source tracing followed `push_gx_draw -> find_gx_pipeline -> find_pipeline_impl`.
GX recording requests blocking readiness and layout information, so the existing
non-blocking last-hash shortcut is ineligible. Every warmed draw still visits
the shared pipeline/compiler mutex and hash map. Blindly enabling that shortcut
would lose readiness and `ShaderInfo` guarantees.

`tests/experiments/ready_pipeline_memo/` adds an isolated 64-slot, 8 KiB,
recording-thread-owned ready-result memo. It reuses the already-computed hash;
there is no second configuration hash, heap allocation or map pointer. Earlier
frame requests and alignment changes go through the authoritative cache.
Shutdown wakes without a compiled result do not populate the memo. Initialization
and shutdown clear it. Ready pipeline eviction does not currently occur before
shutdown; adding eviction would require re-auditing this design.

The model oracle passes 433,422 checks across 200,000 requests. Windows and
Android private Release renderers build with the production optimized flags.
Twelve W01 PC frames (six AO off, six Normal AO) match the reference exactly.
Artifacts and source hashes are under `build/ready-pipeline-memo-20260912`.
GPU/device lifecycle coverage and a reliable Android FPS benefit remain. A second native
fixture now compiles the actual original/candidate lookup and layout-publication
functions with deterministic worker/compiler/persistence stand-ins. It passes
1,042,177 checks, including a shutdown wake with no compiled pipeline and an
earlier-frame persistence update. Synthetic repeated-ready requests acquire
10,000 locks before versus 32 after; this is not a game hit rate or timing.
Both standalone native oracles also pass on physical S22+ (with MP6 closed),
with unchanged APK identity and temporary executable cleanup recorded in
`android-native.json`. The integrated renderer is separate from the FIFO packet
experiment and is not in production.

#### S22+ same-process diagnostic: real reuse, no admitted FPS gain

The private diagnostic in `build/ready-pipeline-diagnostic-20260912` was installed
and run on the physical S22+ in session `build/s22-ready-memo-20260912`. It uses
the optimized renderer/game objects, not a Debug native build. It alternates
baseline/memo/memo/baseline every 600 recorded frames. Frame-packet-owned counters
are evaluated after the worker's actual present callback; skipped/failed presents
are not counted. A native accounting test rejects missing frames, failed presents,
nonpositive intervals, pause gaps and partial phases. The first 60 frames settle
each phase, leaving 540 presentations and 539 intervals.

Settled-board diagnostic rows show approximately 94.2% of ready lookups avoiding
the shared map/mutex. The warm-up ABBA groups put baseline lookup cost around
0.21 ms/frame versus 0.17-0.18 ms with the memo: a saving of only about 0.03-0.04
ms/frame. These are instrumented lookup timings, not an FPS improvement or a
shipping-build measurement. Both modes include timer/counter overhead.

The first nominally cool measurement reached Android thermal status 1 and was
excluded. A shorter retry was refused by the status-0 admission guard before
resuming. `comparison.json` therefore contains **zero eligible complete ABBA
groups** and explicitly records no proven FPS gain. The candidate stays out of
production; the small component saving does not justify claiming the requested
large frame-rate increase. Warm/partial rows remain in the frozen evidence and
are not silently relabeled as cool results.

A separate eight-second MP6-only CPU recording collected 5,950 samples after the
FPS measurement. Its native ELF build ID is verified before resolving addresses.
In this normal-speed/unlocked diagnostic workload, inclusive sampled user-space
cycles include queue submission 21.06%, Vulkan command recording 18.64%, FIFO
drain 20.21%, bind-group submission 5.50%, and GX pipeline lookup 2.40%.
Inclusive costs overlap and cannot be added or converted directly into frame
milliseconds. About 11.37% of sampled cycles stop unwinding in the clock vDSO;
their callers are unknown, and the diagnostic itself adds timing calls. Do not
attribute that bucket to a production clock/pacing bug. Fresh evidence points
toward command submission/replay work, but still requires source attribution and
an uninstrumented device comparison before changing those paths.

`normal-off/cpu-report.json` records the full stacks, build/record hashes and
instrumentation limits. The Android screenshot was visually checked at the
welcome board; this is not a complete-board gameplay test. Production hashes and
the existing twelve exact PC frame pairs remain verified. The original APK was
restored, player config/card bytes and inventory match the backup, private device
data/tool files were removed, and requested USB stay-awake remains 2. No APK was
submitted to Google, distributed, or pushed. `restoration.json` binds handback
checks to the comparison report.

## Scope and result

Cross-cutting review of the port's Android/shared frame lifecycle: game-thread
recording, render-worker submission, upload ownership, render targets, AO,
texture conversion/mip generation, presentation, RmlUi and real-time audio.
This is not a claim that every gameplay subsystem or Android driver is certified.
Work stayed in the port and its local renderer copies; the decomp checkout and
shared Aurora checkout were not changed.

The audit followed source owners and call sites rather than treating the GPU
overlay's rows as independent features. No Graphify index was available, so its
source-tracing fallback was used. In particular, EFB includes the AO composite;
it is not a pure geometry measurement. The immediate-primitive vertex counter
also omits display-list meshes and cannot establish total GPU workload.

Renderer changes are reproducible in
`compat/aurora/base/0040-frame-resource-lifetimes.patch`. Audio/AO callers live
in the normal port sources. Release optimization remains `-O2`; shader math,
AO samples/strength/resolution, filtering, draw order and game rules are unchanged.
No performance fallback was re-enabled on Mali.

## Fixed in this batch

| Area | Finding | Correction and lifetime boundary |
| --- | --- | --- |
| AO and shadow targets | Public AO passes first acquired the generic one-entry offscreen target and then replaced its color. Alternating shadow/AO sizes could evict each other every frame; the generic color was unused for AO. | Select the pooled AO color before starting the pass and keep its depth separate from the GX shadow cache. Resize/format change and renderer reset replace it; queued passes retain strong references. |
| AO bindings | Bind groups and the clean-depth view were recreated for every job. A fresh view also prevented stable binding reuse. | Cache immutable bindings and retain the depth view with its texture. Dimension changes and shutdown invalidate the view. |
| Conversion and depth snapshots | Stable textures/layouts still created fresh bindings every conversion/snapshot. | Reuse the existing expiry-managed binding cache through a strong-handle API. Resources remain owned after an entry expires. |
| Generated mip chains | Repeated updates recreated two views and a binding for every adjacent mip pair. | Each texture owns one view per mip; bindings are cached. Texture replacement retires all of its views. |
| Presentation | The same uniform block was uploaded and the same bindings created every frame. | Upload only when resampler, width, height or AA changes; buffer recreation resets validity. Resource identity remains part of binding lookup. |
| Upload publication | Every operation copied the entire prefix of texture-upload pointers, then copied that list again into its worker closure. | Publish only new uploads at each ordered operation; move the operation into the closure. Absolute upload high-water marks remain unchanged. Later texture updates are not moved ahead of earlier draws. |
| Frame command storage | Resetting frame packets discarded every command vector's capacity. An extra deque retained operations already owned by closures. | Remove the redundant deque; recycle empty command storage only after encoding. Bound retention to 32 lists / 2 MiB per frame slot, release oversized lists and clear all contents/handles. |
| Callback dispatch | Each custom draw/task copied its runtime record, including its label string. | Copy only the callback and user-data pointer under the existing registry lock. Invoke outside that lock. |
| Cache clock | The producer wrote a plain frame counter while the worker used it for expiry. | Atomic frame counter; expiry samples it once before its sweep. |
| Final UI attachments | Final stencil contents were stored even though the next UI frame clears them. | Discard final stencil; discard multisample color only when a resolve target exists. Intermediate passes retain stores and single-sample output remains stored. UI MSAA was already disabled: no removal of 4x UI rendering is claimed. |
| Audio callback | Natural endings and fade-stop freed PCM inside the mixer callback/lock; routine callback logging also performed I/O. | Deactivate immediately, retain inactive PCM until a bounded control-thread collection, detach under lock and free outside. Callback logging is latched for shutdown instead. Opt-in WAV capture remains diagnostic I/O. |
| Audio control transactions | Stops, retriggers, key-group release, voice-limit changes and restore could free PCM while holding the same mixer lock. SE stop also formatted diagnostic output under it. | Bounded transaction-local retirement lists detach ownership under lock and release after unlock. Preserve immediate/fade behavior and base-group filtering. Snapshot SE stop diagnostics before unlocking; format afterwards. Restore also retires any inactive destination PCM before replacing it. |

No global unbounded PCM retirement queue or shared decoded-audio cache was added.
The retirement list cannot exceed all voice/channel slots plus one replacement
in a control transaction. Mixer callback retirement is slot-owned until collection.

## Reviewed without a speculative change

- Depth peek is request-gated/throttled, not an unconditional synchronous
  readback every frame. Blocking framebuffer readbacks are diagnostic captures.
- Staging uploads use recorded high-water ranges, not every buffer's capacity.
  Resource ownership and ordered submission remain necessary on both platforms.
- Empty UI frames are already lazy. Filters and populated overlays can require
  real full-screen work; they were not removed or fused across dependency boundaries.
- Pipeline readiness, layout caching, compact uniforms, sampler memoization and
  repeated GX binding suppression already have targeted regression tests.
- Renderer queue callbacks are moved through the queue. Frame slots cannot be
  recycled while their encoding callbacks still reference them.
- SFX decode happens outside the mixer lock. It still decodes per play; a decoded
  sample cache needs explicit memory/ownership limits and profiling, not a blind
  unbounded cache.
- Polling/backpressure, shader/geometry work, overlay cost and GPU bandwidth still
  need Android measurements. Prior desktop-regressing wait/matrix/shader experiments
  remain disabled. Neither low visible vertex count nor PC FPS proves mobile cost.
- Release provenance verifies every Android game/platform translation unit at
  exactly one `-O2`. This review does not reclassify the native code as Debug
  because private APKs use the private-preview signing certificate.

## Verification

- 97 engine/native tests and 55 setup/release tests pass.
- New native oracles compile the actual edited frame/cache/target functions.
  They exercise 1,000-operation upload order and delta ownership, bounded command
  reuse without stale handles, alternating AO/shadow dimensions, resize/format
  invalidation, final versus intermediate stores, binding expiry/strong ownership
  including counter wrap, and steady/changed presentation uniforms.
- Audio tests compile the actual mixer, retirement helpers and control functions.
  They check exact deterministic PCM samples, natural/fade endings, pause/loop/mute,
  stop-all base-group filtering, key-group retrigger release, lowered voice limits,
  restore success/failure, and require heap release/formatting outside the mixer lock.
  Platform/audio backends and decode are mocked in these ownership tests.
- Both port-local renderer archives and both native Release builds succeed.
  Android verifies all 178 game/platform translation units at `-O2`.
- Twelve Strong-AO/FXAA and twelve AO-off/FXAA matched 1920x1080 frames are
  pixel-identical to the audited baseline. Both before/after runs exit normally.
  The current contact sheets were visually reviewed. Capture timing is not used
  as performance evidence. These captures precede only the additional audio
  control-lock cleanup, not any further renderer/shader changes.
- The production Strong-AO/MSAA/8x-shadow smoke at 2340x1080 exits normally after
  12,500 ticks with four visually reviewed captures and no renderer validation
  failure. It is a board-introduction rendering test, not a completed match.
- The five-minute rendering memory gate reports a decreasing RSS/commit trend
  (616 to 611 MiB RSS, 988 to 967 handles). Its measured swing limits sensitivity
  to approximately 1,443 KiB/min: the nominal 500 KiB/min threshold cannot certify
  smaller leaks. This renderer gate precedes the final audio control cleanup.
- The final-build five-minute concurrent audio stress gate completes with 942
  successful stream starts and 992 SFX starts, no crash/error markers, and
  decreasing fitted RSS/commit trends. RSS changes from approximately 626 to
  624 MiB and handles from 998 to 987. Its 10.6 MiB swing limits sensitivity to
  about 2,708 KiB/min, so this does not certify the nominal 500 KiB/min threshold
  or rule out small leaks. The gate terminates its isolated process at the
  duration limit; it is not a normal game-exit/shutdown test.

Native final-build SHA-256:

- Windows: `24b572e14cd2fd7e00e75bb102e9cd774a01e1e57b91586fd43977902ae25f16`.
- Android unstripped: `5ae3b8f86c97aab483799c9ac3aa42f5a569bad222560ae81771182a507b2461`.

Evidence is retained under:

- `build/checkpoints/pipeline-audit-20260910/{windows,android}/`
- `build/engine-benchmark/pipeline-audit-pixels-ao{0,2}.json`
- `build/board-qa-runs/pipeline-audit-msaa-shadow8/`
- `build/board-qa-runs/pipeline-audit-leakgate/`
- `build/board-qa-runs/pipeline-audit-final-audio-leakgate/`

The isolated baseline builder accepts a partial audited source checkpoint and
compiles those units into separate objects, including the old AO caller. It never
links a new public API caller against the old renderer or overwrites production
objects. Fixed seeds and timing instrumentation remain test-only.

## Remaining verification and next measurements

No Android device was connected. S22+/Tab S10 Ultra FPS improvement, thermal
behavior, mobile driver compatibility and the requested 200+ FPS remain
unmeasured. Full-match gameplay, every overlay/filter interaction, Android
background/resume and live resolution/AA sweeps were not certified by the
stationary-board tests. These are explicit coverage limits, not silent passes.

Use the same board/camera/resolution on each device, without visible stat panels
for throughput measurements; capture separate diagnostic windows with `stat unit`
and `stat gpu`. Compare AO off/on and native/high-quality shadows, first cold and
then warm/sustained. Separate scene/composite, AO, UI and presentation costs.

This batch is published as
[private Android 0.4.19](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-engine-audit-0419-20260910).
The APK is `mp6-v40019-2a62946-dirty-f0b6a026-arm64-v8a.apk` (31,393,647 bytes),
SHA-256 `dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
Packaging reran all 152 tests, verified Release `-O2` provenance, signature,
16 KiB page alignment, exact native libraries and launcher-only assets. Lint
reported no new issues against its existing baseline. The three downloaded
draft assets matched local bytes before publication. No source checkpoint,
game assets or saves were uploaded. Android device FPS is still unmeasured.
The 544-file release-source checkpoint predates this publication note.

## External guidance

The resource-lifetime review is consistent with the primary
[Khronos descriptor-management guidance](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html),
[command-buffer reuse guidance](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html),
and [Qualcomm mobile best practices](https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html?product=1601111740035277).
These sources motivate avoiding redundant allocation/submission work; they do
not establish a speedup for this port or either Android device.
