# Android and shared port performance

## 2026-09-12: direct uniform serialization - adopted as 0053

Graphify's bounded source-tracing fallback followed the measured uniform-copy
cost into frame ownership and publication. `build_uniform` previously filled a
reused CPU scratch buffer and then copied its payload into the frame's mapped
uniform buffer. Patch 0053 removes that second copy and packs into the
unused frame span with constant-size writes. Projection arithmetic, matrix
selection, lighting, fog, texture state, shader code and graphics settings are
unchanged.

The recording thread owns this span. Frames borrow a fixed-size mapped staging
allocation, and pass submission captures a completed high-water mark; the writer
finishes before any pass is published. It reserves the aligned `ShaderInfo`
capacity but commits only the original raw payload length. Explicit shader
padding is initialized even on reused mappings. Bounds, invalid alignment,
interleaved appends, double completion and writes after completion are checked.
No `ByteBuffer` layout or shader/cache format changes are required.

The native fixture executes the complete unchanged and candidate serializers,
with production Aurora math/GX structures and helper arithmetic. Each of the
normal/reversed-depth configurations passed 50,000 cases on Windows and again
on the S22+: 117,450,416 payload bytes compared per configuration/platform.
Coverage includes compact/full palettes, line/point mode, all optional payload
groups, signed zero, infinities, NaN payloads, unaligned borrowed memory, six
alignments, rounded capacity, dirty-state behavior and prior-payload canaries.
Each run also checks 10,000 committed records across owned-buffer growth.
Deliberate byte corruption fails the oracle; invalid operations exit with a
diagnostic rather than invoking a Windows crash dialog.

A standalone ARM64 benchmark ran on S22+ CPU6, alternating baseline/candidate
order for twelve pairs per workload (the first two pairs excluded). Both thermal
checks were below status 2. CPU affinity was fixed, **CPU frequency was not**.

| Packed payload | Baseline median | Direct median | Paired median time reduction |
|---|---:|---:|---:|
| 384 bytes | 105.479 ns | 94.764 ns | 10.2% |
| 1,040 bytes | 133.782 ns | 116.361 ns | 13.0% |
| 2,048 bytes | 215.601 ns | 185.827 ns | 13.8% |

**These are component timings in ordinary CPU memory, not game FPS and not a
measurement of Dawn's GPU-mapped staging memory.** The absolute savings are only
about 11-30 ns per packed record in this hot microbenchmark. Do not extrapolate
them to a 10-14% whole-game improvement or a sustained 200+ FPS result.

The isolated Windows pair links the same 176 benchmark objects and changes only
the two renderer objects needed by this candidate. All twelve 1920x1080 FXAA
capture pairs with AO off and all twelve with AO on are pixel-identical at
matching board ages. AO-off sampled upload bytes/draw counts are identical;
AO-on uniform bytes are identical (277,048/frame), with tiny asynchronous
sampling differences in other counters. The capture runs are not a controlled
timing benchmark: the AO-on candidate run took 1.241 ms/frame versus 1.095 ms for
the baseline. Neither a gain nor the absence of an in-game regression is proven.
An initial baseline launch lacked its runtime DLLs and exited before rendering;
the isolated builder now stages the normal runtime DLL set and both completed
comparisons exclude that launch.

The subsequent real-mapping diagnostic runs both serializers inside the same
S22+ process, alternating AB/BA order. Both write valid spans in the actual
Dawn-mapped frame buffer; sparse payload comparisons run outside the timed
brackets. Across 36 warmed intervals (1,179,648 paired calls), the reference
median was 611.302 ns and direct packing was 357.461 ns. The paired median time
ratio was 0.5865: **41.35% less CPU time in this component**, with no reported
payload mismatches. All thermal samples for that paired test were status 0.
This deliberately doubles uniform traffic and adds clock/readback work, so its
FPS is not shipping-performance evidence. Only the single direct writer is
adopted; the paired diagnostic is not in either Release build.

The separate baseline/candidate/repeat-baseline Android runs used 2340x1080,
FXAA, AO off, 4x shadows, VSync off and Fast Forward. Each selected resumed
window lasted 20 seconds:

| Resumed median | Baseline | Direct writer | Repeat baseline |
|---|---:|---:|---:|
| Presents/s | 202.584 | 198.815 | 221.273 |
| Game phase | 2.093 ms | 2.208 ms | 1.930 ms |
| CPU submission phase | 1.243 ms | 1.171 ms | 1.147 ms |
| GPU span | 4.154 ms | 3.207 ms | 3.245 ms |
| Complete rate reports | 4 | 4 | 3 |

These later runs permitted thermal status 1; they are not a thermally or
frequency-matched cohort. Baseline drift exceeds the candidate difference.
**No whole-game FPS gain, sustained 200+ FPS result, or strict no-regression
claim follows from this A/B sequence.** Adoption is based on eliminating the
duplicate work, the in-process paired CPU improvement and exact serialization/
pixel checks, not on selecting the fastest separate run.

The candidate also booted with FXAA and AO enabled on the S22+ and recorded GTAO
passes through tick 3103. A later screenshot resume was refused above thermal
status 1 and the owned process was stopped. This is a boot/render smoke, not a
full-game or Mali validation. The AO-off phone screenshot was visually checked.

Both production Release renderers were rebuilt from the exact tested source;
the normal Android build verifies all 177 game/platform TUs at `-O2`, and the
Windows build reused its 175 verified game/platform TUs. Renderer source
payloads match the candidate and contain no paired-test instrumentation.
AO source/shader and version metadata are unchanged. The rebuilt Windows
Release reached the board and captured three 1920x1080 AO+FXAA frames in a
30-second bounded smoke; the harness then terminated it at its time limit.
That intentional termination is not a completed board-game result.

Adopted patch: `compat/aurora/base/0053-direct-uniform-writer.patch`.
Artifact/source hashes and production smoke evidence:
`build/uniform-direct-20260912/adoption.json`. Android staged native SHA-256:
`da3a5d72f8944b6c1d665d66f70bae50681518f40200b7b47824008dd1bc7a48`.
Windows Release SHA-256:
`c490c747b59ca2344ee54ff55f6515af812f8f588d8eaeaaba2aff8422481034`.

Evidence: `s22-native-benchmark.json`, `native-tests.json`, `provenance.json`,
`windows/provenance.json`, `uniform-direct-pixels-ao0-v2.json` and
`uniform-direct-pixels-ao1.json` under that experiment directory. The runner and
build helpers are under `build/recovery/*uniform*`; the native fixture is
`tests/native/uniform_direct_selftest.cpp`, now run against production sources
by `tests/test_uniform_direct.py`. The final full run passed 168 engine/native
tests, and all 55 setup/release tests passed. Two preceding attempts had an
intermittent Zig compiler exit 5 in the existing replay fixture before test
execution, without diagnostic output; retained verbose compiles and the later
full suite passed. That compiler issue is not claimed fixed. Compiler assertions
now preserve stdout as well as stderr.

Exact phone log/request/resume/thermal evidence is in
`build/s22-uniform-direct-20260912/report.json`. The local profiling wrappers
were replaced with the original APK, verified SHA-256
`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
Player settings/card bytes and file inventory match the pre-test backup. Only
the private profiling directory and temporary UI XML were removed; original
game content was untouched. USB stay-awake remains 2 for ongoing testing.
No APK was uploaded or submitted to Google; Play Protect prompts were declined.
No push or decomp modification occurred.

## 2026-09-12: live matrix-bit deduplication - not adopted

Graphify's bounded source-tracing fallback followed the fresh S22+ CPU profile
into FIFO matrix uploads, draw merging, and uniform/storage copies. An isolated
candidate compared incoming XF matrix bits against the actual live matrix and
only set `stateDirty` when a bit changed. It did not use floating-point equality,
a separate register cache, a tolerance, or reduced graphics settings. Existing
dirty barriers, partial texture writes, normal-matrix padding, light writes and
decoder return behavior were retained.

The native oracle executes the original and candidate decoder against Aurora's
actual matrix types. All 12,800 sequences passed, including both endiannesses,
unaligned input, signed zero, infinities, NaN payloads, partial writes, state
restoration and pre-existing dirtiness. The original unconditional-dirty decoder
fails the intended negative control with a diagnostic and exit code 1.

However, the fixed-seed introductory-board run showed **no workload reduction**:
both builds sampled 298.794 submitted and 192.880 merged draws/frame, with the
same vertex/index/uniform/storage bytes: 193269/84363/257080/609403. All twelve
1920x1080 FXAA, AO-off capture pairs were pixel-identical. This is a correctness
and workload check on PC, not an Android performance measurement or full-game
validation. A first attempt exhausted its 20,000-tick budget during real-time
startup and was excluded; the completed pair used a 120,000-tick startup budget.

The S22+ comparison used 2340x1080, FXAA, AO off, 4x shadows, VSync off and Fast
Forward. Each selected resumed window lasted 20 seconds. The exact resume
markers and log/request/thermal hashes are recorded in
`build/s22-xf-matrix-20260912/report.json`.

| Resumed median | Baseline | Matrix candidate | Repeat baseline |
|---|---:|---:|---:|
| Presents/s | 207.351 | 218.975 | 258.707 |
| Game phase | 2.061 ms | 1.971 ms | 1.750 ms |
| CPU submission phase | 1.231 ms | 1.165 ms | 1.060 ms |
| GPU span | 3.590 ms | 3.272 ms | 3.016 ms |
| Complete rate reports | 4 | 3 | 4 |
| Starting AP / skin | 36.9 / 36.3 C | 37.6 / 37.0 C | 39.5 / 38.0 C |

All selected windows sampled thermal status 1, not a cool/unthrottled cohort.
The cold candidate attempt reached status 2 and was excluded. Temperatures and
clocks were not matched or fixed; repeat-baseline variation exceeds the initial
difference. These results establish neither a gain nor a regression, nor a
sustained 200+ FPS result. Combined with unchanged draw/upload work, they do not
justify shipping extra per-matrix comparison work. **The candidate remains only
under `build/xf-matrix-20260912`; no 0053 renderer patch was added.**

Two measurement safeguards were retained instead:

- A backgrounded game is rejected before measurement starts; failed resume cannot
  silently produce a background-only timing window or navigate another app.
- Engine benchmark builds now own every object file, including uninstrumented
  units, and use a scoped link response file for long Windows paths. A candidate
  wrapper initially imported build configuration before setting data-include
  paths, which exposed the old benchmark's reuse of release-object destinations.
  The normal Windows Release recipe was rebuilt successfully before comparison;
  its current executable SHA-256 is
  `d63f18b9be85c1c23e44c6aaeb3138012aa79a7670a6aa51b22b0f7fc355dee0`.
  It does not contain the matrix candidate. Tests cover object ownership,
  response-file argument preservation, and rejection of unrelated commands.

All 167 engine/native tests and 55 setup tests pass. The 244 Android production
link/source/artifact inputs recorded by this experiment remain hash-identical.
The original non-debuggable phone APK was restored and hash-verified; player
settings, cards and save inventory are unchanged. The isolated device directory
and temporary UI dump were removed. Play Protect's repeat prompt was answered
**Don't send**, as requested. USB stay-awake remains 2 for continuing tests;
nothing was pushed, uploaded or submitted to Google. The next source-audited
cost is uniform serialization through a scratch buffer followed by another copy
into the frame-owned buffer; this experiment did not optimize that path.

## 2026-09-12: ThinLTO phone check — not adopted

The isolated O2 ThinLTO candidate now boots and reaches the board on the S22+.
The baseline and candidate screenshots show the same introductory scene without
obvious corruption; their animation ticks are not synchronized, so this is a
visual smoke check, not pixel-exact or full-game validation. Android's host
backend explicitly does not support savestates; no Android restore test is
claimed. The candidate retains the host-data sections, but this is not sufficient
to approve a Windows build or cross-file save-state compatibility.

`build/s22-thinlto-20260912/report.json` records the local APK identities, exact
resume markers, hashed input logs and measured thermal windows. Settings are
2340x1080, FXAA, AO off, 4x shadows, VSync off and Fast Forward. Each warmed,
resumed measurement lasts 20 seconds and includes four complete five-second
rate reports after the resume exclusion. Every sampled thermal status is zero.

| Short resumed median | Baseline | ThinLTO | Repeat baseline |
|---|---:|---:|---:|
| Presents/s | 170.487 | 173.472 | 218.043 |
| Game phase | 2.425 ms | 2.322 ms | 1.974 ms |
| CPU submission phase | 1.372 ms | 1.332 ms | 1.139 ms |
| GPU span | 4.241 ms | 4.216 ms | 3.256 ms |
| Starting AP / skin | 35.6 / 35.6 C | 36.2 / 35.8 C | 36.4 / 35.8 C |

The initial approximately 1.8% FPS difference is smaller than the repeat
baseline variation. Clocks are not fixed or sampled, and these short windows
do not establish its cause. **No reliable ThinLTO gain is established; do not
adopt or upload the candidate.** In particular, the repeat baseline's 218 FPS
is not a new optimization gain, sustained result, tablet result or normal-speed
interpolation result. All 358 production inputs/artifacts checked against the
candidate's provenance remain hash-identical. No renderer, shader, resolution,
sample count or quality setting was changed by this experiment.

A separate eight-second app-only CPU capture of the current baseline collected
6,342 samples with none lost. Its native build ID is verified before symbolizing.
Inclusive sampled CPU-cycle shares include Hu3DExec 33.75%, GX FIFO processing
20.03%, and Dawn queue submission 17.13%; these overlap and must not be summed.
Exclusive shares include memcpy 5.12%, FIFO processing 3.96% and C_MTXConcat
2.52%. This is CPU sampling, not GPU or wall-clock timing. The renderer already
has a worker queue and two frame slots; the trace supports investigating scene
processing, command decoding/copying and driver submission, not assuming a
single-threaded renderer or expecting compiler flags alone to solve the cost.

Profiling evidence is now reproducible with `--resume-marker resume-N.json`:
later screenshot resumes cannot silently select a different window, and summaries
record log/request/resume hashes. CPU sampling writes separate `cpu-logcat.txt`
and `cpu-thermal.txt` files instead of replacing an earlier clean FPS capture.
Regression tests cover both properties. Two Windows negative-control runs also
hit CRT assertion timeouts and temporary-file locks. The eight assertion-based
negative-control fixtures now use one test-only, noninteractive failure helper;
their runners require a diagnostic and exit code 1 instead of accepting arbitrary
crashes as success. Production assertions are untouched.
The final full rerun passes all 164 engine/native tests; all 55 setup tests also
pass, and `git diff --check` is clean.

The original non-debuggable APK is restored and SHA-256 verified as
`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
Player settings/card files and their inventory are unchanged. The private
profiling directory, temporary device profiler and two failed-run temporary
Windows fixture directories were removed; recordings remain locally for audit.
USB stay-awake remains `2` (original `0`) for continued testing. Nothing was
pushed, uploaded or submitted to Google.

## 2026-09-12: isolate composition cost; ThinLTO candidate (not uploaded)

Graphify's bounded source-tracing fallback confirmed that the EFB GPU row
includes the full-resolution AO composition draw. A local diagnostic now
isolates that draw without editing production source or replacing the verified
native libraries: `tests/integration/build_ao_composite_probe.py` recompiles
only the AO translation unit with an explicit, diagnostic-only omission switch.
Depth preparation, foliage/water coverage, GTAO and denoising remain scheduled.
The same APK is used for both cases; the helper verifies its identity before
enabling the omission and the summary requires confirmation from native code.
Diagnostic results are explicitly labeled as non-shipping performance evidence.

The S22+ comparison uses 2340x1080, FXAA, Strong AO, 4x shadows, VSync off and
Fast Forward. Both boards were loaded, backgrounded to cool, and resumed without
screenshots in the measured window. The complete resumed windows use the
existing additional 300-tick exclusion. Full AO starts at AP/skin 36.2/35.5 C;
the composition-omitted case starts at 37.0/35.8 C. Every sampled thermal status
in these resumed windows is zero. The phone reports higher thermal statuses
outside these windows, and clocks are not locked; this is not a sustained or
frame-paired benchmark. Asynchronous outliers are retained in the reports.

| Resumed median | Full AO | Composition omitted (diagnostic only) |
|---|---:|---:|
| EFB | 4.341 ms | 2.668 ms |
| GTAO | 2.179 ms | 2.175 ms |
| GPU span | 9.404 ms | 7.722 ms |

The approximately 1.67 ms EFB difference estimates AO composition plus its
associated draw setup. The remaining scene work is still about 2.67 ms, and
GTAO remains another 2.18 ms. This supports targeting both the full-screen
reconstruction and scene submission, not attributing all EFB cost to meshes.
The omission visibly removes AO and is **not** an accepted optimization, FPS
gain or 200+ FPS result. Evidence: `build/s22-composite-20260911/report.json`.

A separate four-tap composite-unrolling experiment was rejected before native
integration. It showed no convincing desktop GPU improvement and failed the
exact-pixel gate at one dense-scene pixel (three channels, maximum delta
1/2048). Its partial results and explicit rejection are retained under
`build/ao-composite-unroll-20260911`. Production shader bytes are unchanged.

The compiler audit also found that the O2 game/platform build does not perform
cross-translation-unit optimization. Following the
[Clang ThinLTO workflow](https://clang.llvm.org/docs/ThinLTO.html),
`tests/integration/build_android_lto_probe.py` builds a separate O2 ThinLTO
candidate from all 177 authenticated command records. It preserves
`-fno-strict-aliasing`, CPU requirements, normal weak/data symbol binding policy
and all existing renderer/dependency archives; it does not enable fast-math.
Compiler/linker jobs are bounded to three. Response-file linking avoids the
Windows command-length limit; a hashed compilation checkpoint supports safe
link retries. No production input or artifact is replaced.

The successful candidate is in `build/android-thinlto2-20260911` (the first
attempt stopped at the Windows command-length limit). Its stripped library is
30,479,520 bytes, SHA-256
`bf1da77692549dd6bcca6007b76afa49d4728f9a875d42ce41dec451c466ddb1`.
The dynamic-symbol check retains all strong exports and all surviving symbol
types/bindings/visibility; 133 weak C++ exports disappear. This is not a
complete runtime ABI proof. At this build checkpoint the candidate had not been
installed or measured; the subsequent phone check and decision not to adopt it
are documented above. Cross-file optimization has not been enabled for production
or shared with Windows.

Cleanup is complete: the original non-debuggable APK is restored and
hash-verified, settings/cards/save inventory are unchanged, and only this
session's private profiling files were removed. On the restoration prompt,
the user's explicit "Don't send" choice was applied; no APK was sent to Google
and no security setting was disabled. USB keep-awake remains `2`, originally
`0`, for ongoing testing. Nothing was pushed or uploaded. All 160 engine/native
tests (including the 13 targeted profiling tests) and 55 setup tests pass; this batch adds test/profiling tooling
and evidence, not a new shipping FPS improvement.

## 2026-09-11: read-only foliage depth (not uploaded)

Graphify-guided producer/consumer tracing found that foliage/water coverage
never writes depth: its GX state explicitly uses `GXSetZMode(..., GX_FALSE)`.
The previous optimized path still copied the final EFB depth into a private
single-sample depth attachment with a full-screen seed draw. Patch
`0052-readonly-foliage-depth.patch` now borrows the EFB attachment directly for
depth testing inside the coverage pass. The EFB producer is sealed first;
queued passes own their texture views, and EFB writes resume only after the
coverage bracket closes. The read-only attachment has undefined load/store
operations and cannot clear, overwrite or discard scene depth, following the
[WebGPU attachment contract](https://developer.mozilla.org/en-US/docs/Web/API/GPUCommandEncoder/beginRenderPass#validation).

This removes the seed draw and avoids the private depth allocation on the
single-sample Qualcomm/desktop path. It does not eliminate the coverage pass
or the mesh replay. MSAA still seeds sample zero into private depth, and Mali
and unknown Android vendors retain their verified compatibility schedule.
There is no claimed tablet gain from this change. AO shader source is unchanged
(`238d080d344cf84fbac76f2542ed31a71d1e1d02779b401ada09de6e6c0e49be`): no
resolution, sample count, radius, precision, coverage or geometry reduction.

Both native Release builds succeed (175 Windows and 177 Android translation
units; Android verifies exactly one `-O2` per unit). All 158 engine/native,
55 setup/release and 62 GPU tests pass. The new tests exercise actual C++ pass
admission, descriptor generation, no-allocation/resize ownership, restoration
and no-discard behavior. GPU tests compare blended foliage and water coverage
exactly for both depth directions, odd sizes and Depth32Float, Depth24Plus and
Depth24PlusStencil8. Scene depth is unchanged; subsequent writes still work.

Twelve matched 1920x1080 FXAA board frames are pixel-identical. The unchanged
MSAA fallback has 1-88 differing pixels/frame; repeating the same executable
also differs at 2-94 pixels/frame. Thus the MSAA scene capture is a bounded
non-determinism check, not a claim of bit-exact equivalence between runs. The
deterministic GPU coverage tests remain exact. Ground-level Windows and S22+
captures were visually reviewed.

The six alternating desktop timing runs reduce median offscreen GPU cost from
0.021197 to 0.004181 ms and GPU span from 0.356395 to 0.333858 ms. CPU frame
medians move from 1.215154 to 1.315720 ms, with overlapping run ranges; no
desktop FPS improvement is claimed. The image runs are not timing evidence.

S22+ measurements and exact artifact provenance are recorded in
`build/foliage-readonly-report.json`, with individual runs under
`build/s22-readonly-20260911`. The first candidate run includes a screenshot
and is diagnostic only for whole-game timing. Baseline sources and authenticated
renderer archives are in `build/checkpoints/foliage-readonly-20260911`.

The final clean S22+ pair uses 2340x1080, FXAA, Strong AO, 4x shadows, VSync
off and Fast Forward (`tick_hz=0`), with GPU timing enabled. Both boards were
loaded, backgrounded to cool, and resumed for 25 seconds; only full five-second
windows after an additional 300-tick exclusion are included. Neither timing
window contains a screenshot. The baseline starts at AP/skin 35.8/35.1 C,
the candidate at 35.5/35.4 C; all sampled thermal statuses during both resumed
windows are zero. GPU clocks are not locked.

| Clean S22+ median | Baseline repeated | Candidate |
|---|---:|---:|
| Offscreen GPU pass | 0.444 ms | 0.148 ms |
| GPU span | 9.648 ms | 9.322 ms |
| Presents/s (four full windows) | 102.079 | 105.543 |

The affected pass is about 67% cheaper, saving roughly 0.30 ms/frame. This
short pair is consistent with a modest ~3% overall improvement, not a 2-3x
gain or a 200+ FPS result. Whole-frame ranges still overlap and older colder
windows varied substantially, so this is not a general FPS guarantee. The
remaining EFB work (~4.3 ms) and GTAO (~2.18 ms) still dominate. Normal-speed
interpolation and Mali performance were not measured in this experiment.

Device cleanup: player settings/cards and save inventory verify byte-identical;
the owned profiling directory was removed. The user selected "Don't send" at
Play Protect's optional security-check prompt. The original non-debuggable
0.4.19 APK is restored and hash-verified (`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`);
`build/s22-readonly-20260911/cleanup.json` records the completed restoration.
No APK was sent to Google and no security setting was disabled. USB keep-awake
remains enabled (`2`, previously `0`) at the user's
request for continued optimization; restore its previous value when testing
is finished. No push or private-build upload was performed in this experiment.

## 2026-09-11: analytic open-horizon integral (not uploaded)

Graphify-guided source tracing identified redundant trigonometry in GTAO's
unoccluded normalization. For a viewer-facing projected normal the two open
endpoints are `n +/- pi/2`; their integral simplifies to `cos(n)+n*sin(n)`.
The optimized shader now evaluates that expression, with the original general
formula retained for floating-point excursions beyond the viewing hemisphere.
The existing exact zero-horizon branch remains essential. An initial experiment
without that branch introduced faint rounding-induced AO on a flat plane and
was rejected before installation. The Mali/compatibility shader is unchanged
apart from whitespace. Only the already-optimized Qualcomm/desktop path changes.

No samples, AO radius/strength, resolution, precision, geometry, water/foliage
coverage, or gameplay timing were reduced. Both native Release builds pass;
Android still verifies exactly one `-O2` on each of its 177 translation units.
The decomp dependency remains `3b0a20d3239d31f20921287d7ffa17f4219026f7`.

S22+ testing uses 2340x1080, FXAA, Strong AO, 4x shadows, VSync off and Fast
Forward (`tick_hz=0`), not normal-speed interpolation. Each build was loaded,
backgrounded to cool, and resumed for a bounded 25-second measurement. Four
fully warmed five-second windows survive the extra 300-tick resume exclusion.
All sampled thermal statuses during the resumed windows were zero; starting
AP/skin temperatures were approximately 36/35 C. No GPU-clock lock is claimed.

| Resumed S22+ run | Median GTAO ms | Median presents/s |
|---|---:|---:|
| Baseline | 2.223 | 82.338 |
| Candidate | 2.175 | 83.838 |
| Baseline repeated | 2.223 | 83.638 |

The approximately 2.2% GTAO-pass reduction repeats against both controls, but
whole-game FPS does **not** show a reliable improvement beyond run variation.
GPU-span reports contain large asynchronous outliers; do not use their differing
medians as an end-to-end speedup claim. This is a small reduction in GPU work,
not a 200+ FPS result and not evidence of a tablet speedup. The scene EFB pass
and remaining AO generation/composition still dominate steady GPU cost.

In the clean six-run Windows comparison, median GTAO time falls from 0.134938
to 0.125013 ms and GPU span from 0.359100 to 0.349295 ms. CPU whole-frame
medians move from 1.073396 to 1.087481 ms (1.3% higher), within overlapping
run ranges; no desktop FPS improvement is claimed. An earlier timing series
overlapped shader validation at startup and is retained as diagnostic only;
use `ao-open-arc-clean-timing-20260911.json` for the isolated comparison.

Validation: 156 engine/native tests, 55 setup/release tests, and 19 GPU tests
pass. A separate 200-case shader comparison covers raw/prepared depth,
single/MSAA attachment inputs, reversed depth, grazing surfaces, cutout decals,
and native/reduced odd dimensions. Flat planes and compatibility results stay
exact; normal and alpha channels stay exact. Maximum visibility/color drift is
one RGBA16F step (`1/2048`), not bit-exact equivalence. Twelve matched FXAA board
frames differ at only 6-9 pixels of each 1920x1080 image, by at most one 8-bit
channel level. Ground-level output was visually reviewed. Exact rejection and
filter tests now isolate the old general integral; the new integral has a
separate strict rounding bound rather than weakening those exact assertions.

The installed APK was restored and hash-verified, with the player's settings,
memory cards and save inventory unchanged. Only the owned device profiling
directory was removed. No push or upload was performed. Provenance, artifact
hashes, timings, image metrics and restoration evidence are consolidated in
`build/ao-open-arc-report.json`; the baseline lives under
`build/checkpoints/ao-open-arc-20260911`.

## 2026-09-11: borrowed foliage depth (not uploaded)

Graphify-guided producer/consumer tracing found an avoidable full-resolution
depth snapshot in the AO coverage pass. The old sequence copied EFB depth to
R32, then immediately sampled that R32 texture to seed the private foliage
depth target. Port-owned renderer patch
`0051-borrowed-foliage-depth.patch` seals the EFB and lends its depth attachment
only to the following offscreen bracket. The seed reads sample zero directly;
the private target, depth tests, premultiplied foliage and water coverage are
unchanged. EFB writes resume only after the offscreen pass closes.

This removes one depth-snapshot pass and one full-resolution R32 intermediate
per foliage camera. It does not remove the private depth target or the seed
draw, and it does not reuse a snapshot across intervening scene writes.
The API documents the borrowed view's strict lifetime, copies pass metadata
before vector reallocation, rejects invalid/nested calls, and leaves the
existing R32 route available. Mali/other compatibility devices retain their
previous schedule; this optimization is enabled on the tested Qualcomm path
and desktop. There is no claimed tablet improvement from this change.

AO shader source is byte-identical to the previous version
(`46883f1446303024cafe11017f6a27b17c07c96b2f2398ad71bdb89c3beba64b`).
The existing shader builder already supports depth-typed single/MSAA reads.
No radius, strength, sample count, precision, shading resolution, geometry,
water/foliage classification or simulation-rate reduction was introduced.
Seed pipeline keys distinguish snapshot, single-sample and multisample sources.

Verification:

- Both native Release builds succeed; all 178 Android translation units retain
  exactly one `-O2`. The baseline AO caller and renderer archives used for
  comparison are separately authenticated.
- 151 engine/native tests and 55 setup/release tests pass. A pre-existing fixture
  that extracted an entire source span was narrowed to its actual functions,
  so insertion of the new API does not pull unrelated dependencies into it.
- Six GPU tests pass: the new seed oracle covers odd dimensions, depth32float
  and depth24plus, single sample and 4x MSAA with intentionally different sample
  values, and later EFB writes; existing depth-preparation, water/alpha and
  compatibility-source checks also pass.
- 24 aligned FXAA board frames (12 Strong AO, 12 AO off) are pixel-identical.
  The separate MSAA whole-scene pair differs at 0–31 pixels per frame; the
  same-baseline-binary control also differs at 4–38 pixels. These are mostly
  character pixels, not proof of MSAA bit determinism. The synthetic seed
  output itself is exact. Ground-level captures were visually reviewed.
- A production Release normal-speed/FI smoke reaches W01 and exits normally
  after 4,200 ticks, with 84,996 AO encodes/composites/foliage passes. This checks
  retained-stream ordering, not a complete match or physical display refresh.
- The first six-run desktop GPU-instrumented pair reduces median GPU span from
  0.366694 to 0.358793 ms, but median whole-frame CPU wall time rises from
  1.013952 to 1.040916 ms (2.7%). Individual CPU windows overlap
  (baseline 0.998–1.034 ms, candidate 1.009–1.075 ms). A second six-run series
  reverses the whole-frame result: 1.065164 to 1.035017 ms (2.8% lower),
  while GPU span again falls, from 0.365696 to 0.358272 ms. The depth-copy
  saving repeats, but CPU-bound desktop FPS remains within the run-to-run
  spread; no desktop FPS gain is claimed. Both reports are retained.

The S22+ confirms the board GPU pass count drops from 14 to 13. A cold candidate
run reached thermal status 2 and is excluded from FPS comparison. A subsequent
load/cool/resume run completed its 20-second measurement at thermal status 0,
with three fully warmed five-second windows and a 9.778 ms median diagnostic
GPU span. The reinstated baseline used the same load/cool/resume procedure
and also stayed at thermal status 0. Its three warmed intervals have a median
80.174 presents/s versus 83.954 for the candidate (about 4.7% higher), while
median GPU span changes from 9.981 to 9.778 ms and depth snapshots from
0.756 to 0.524 ms. Both use 2340x1080, FXAA, Strong AO, 4x shadows, VSync off
and Fast Forward (`tick_hz=0`), not normal-speed interpolation.
Starting AP/skin temperatures were 36.1/35.3 C for the candidate and
35.7/35.0 C for the baseline. This is one short resumed pair; asynchronous
GPU windows include large outliers and do not prove a sustained 4.7% gain or
200+ FPS. The larger AO generation/composition cost remains.

Native SHA-256:

- Windows: `f4f9c9dc390ffa1cf30e717bab774940d2e62d700f4ade894d5555c187af5448`.
- Android unstripped: `b1c2528e04f126f9d3c0e2d021fe8879fc03215f6cf15797c03846180186e6ec`.
- Android staged: `435010ea9591a0af6f57cb806dad64f57c3026acb1f570c904c6bd9300cc32e6`.

Evidence: `build/checkpoints/foliage-depth-20260911/`,
`build/foliage-depth-verification.json`,
`build/foliage-depth-device-ledger.json`,
`build/engine-benchmark/foliage-depth-*.json`, and the
`foliage-depth-*-ao2` runs in `build/s22-performance-20260911/`.
The first 20,000-tick desktop startup attempt ended before the board and is
excluded; successful capture runs used a 120,000-tick startup ceiling and
exited at their fixed board-age measurement endpoint.
The original non-debuggable phone APK was restored; settings/card files were
verified byte-identical and only the owned profiling directory/tool were
removed from the phone. Host evidence remains available. No push or upload.

## 2026-09-11: direct Android function binding (not uploaded)

Source tracing and an authenticated S22+ CPU trace identified another Android
link-policy cost. The 4,214-sample `vertex-invalidation-ao2-cpu` trace spent
4.4831% of sampled user-space cycles in `libmp6game.so` PLT stubs: 3.5347% in
strong functions defined inside the same image, 0.5220% in weak functions and
0.4264% in imports. These are all-thread sampled cycles, not frame time or an
FPS prediction. `tests/integration/audit_android_plt.py` resolves each sampled
instruction against the exact ELF build ID; it refuses mismatched images or
an unrecognized PLT layout.

The game-image linker now uses `-Bsymbolic-non-weak-functions`. Internal calls
to strong definitions bind directly; weak functions, imported functions and
data keep their existing binding. No exports are hidden: JNI bootstrap,
`dlsym` entry points and `dladdr` diagnostics remain available. This is the
[LLVM-defined strong-function subset](https://reviews.llvm.org/D102570), not
the broader `-Bsymbolic` or `-Bsymbolic-functions` options. Android's
[symbol-visibility guidance](https://developer.android.com/ndk/guides/symbol-visibility)
describes the cost of exposing implementation symbols; hiding the whole image
was not used because this port also depends on dynamic symbol lookup.

The actual new-main game library goes from 20,224 to 6,703 PLT entries:
13,521 strong-function stubs removed, all 455 imports and 6,248 weak-function
stubs retained. The complete 39,843-entry named dynamic symbol inventory and
metadata are unchanged, as are 21,803 weak/import/data relocations. The scoped
change applies to the Android game image, not the bootstrap library, shared
decomp/Aurora checkouts, or Windows PE linking. It changes no shaders,
resolution, AO samples, textures, geometry or simulation policy.

The save-state link stamp now also hashes the actual ordered link policy and
all explicit archive inputs, including `libnod.a`. Previously flags, archive
order and nod-only changes could move code without changing the stamp. A
no-op rebuild produces the same stamp and identical native bytes. Older
quick states are rejected against the changed image; memory-card saves are
not converted or changed.

Verification: Android Release links successfully with the same 178 existing
`-O2` translation units (zero recompilations). All 149 engine/native tests and
55 setup/release tests pass. The AArch64 linker fixture verifies direct strong
calls/addresses, preserved weak calls/addresses, imported `puts`, public data,
JNI exports and 16-KiB page policy. The actual-image comparison additionally
checks every dynamic symbol and non-direct relocation category.

Baseline/new native hashes:

- Before: `7d03812cebb9e66ddbb43943e5fc7f0da4e1161e7469e27c2f3c402d5ea38c72`.
- After: `c1e53c1fe41cfd7fd202c968c525e2b6a94b1b9cca5b982b44c4c1338a951077`.
- Staged: `10f2da2f149db3aeda454d59d2a9521b8e5b06ee2d80bcc7c2e31880e30d4402`.

Evidence: `build/checkpoints/android-direct-binding-20260911/`,
`build/android-plt-{sampled,current,direct-binding}.json`,
`build/android-direct-binding-verification.json`, and the isolated local-only
wrappers/runs in `build/s22-performance-20260911/`. The comparison baseline
already contains decomp main `a15c000`; these results must not be attributed
to the upstream integration.

### S22+ verification and limits

The phone used FXAA, shadow quality 4, 2340x1080, VSync off and Fast Forward
(`tick_hz=0`, unlocked enabled). These are uncapped board-introduction
throughput tests, not normal-speed interpolation or a complete match.
After excluding incomplete warmup intervals, the no-capture AO-off results are:

| Run | Fully warmed five-second windows | Median presents/s |
| --- | ---: | ---: |
| Baseline before candidate | 6 | 195.192 |
| Direct-binding candidate | 5 | 198.119 |
| Baseline reinstalled after candidate | 4 | 193.281 |

This short sequence suggests a modest 1.5–2.5% gain, not a major speedup or
sustained 200+ FPS. All runs started at thermal status 0; candidate/control
reached status 1, while the first baseline remained at 0. Starting temperatures
and frequency behavior differ, and there is only one clean candidate run.
The separate candidate capture run (median 201.484) is visual evidence only,
not part of the comparison. Its ground-level board frame was reviewed.

The AO-on baseline has three fully warmed intervals at median 79.772 presents/s
and GPU span 10.044 ms. The candidate reached the board but hit thermal status
2, automatically stopping the run. Its one fully warmed interval is excluded
from comparison. No AO speedup is claimed, and the GPU bottleneck is still open.
No new candidate CPU trace was recorded; the PLT attribution above belongs
to the authenticated earlier trace, not to this APK.

The profiler summary now requires the **start** of each tick-report window to
be past warmup, rather than only its end. Tests cover partially warmed reports,
resume warmup and missing tick counts. All six logs above and the discarded
candidate runs were recomputed with that gate. Raw logs remain unchanged.
`build/android-binding-device-ledger.json` retains APK hashes, exact arguments,
thermal/capture exclusions and the numerical summaries.

The original non-debuggable 0.4.19 phone APK was restored after profiling;
settings and memory-card files are byte-identical, and only the owned test
directory and temporary profiler were removed. Local profiling wrappers were
not uploaded. No Git push or shared decomp/Aurora modification was performed.

## 2026-09-11: vertex invalidation and resumed S22+ measurements (not uploaded)

Graphify's source-tracing fallback connected the recorded vertex-upload copies
to a correctness gap: the FIFO dispatcher ignored `GX_CMD_INVL_VC`. An explicit
vertex-cache invalidation could therefore leave an uploaded snapshot alive
after an in-place CPU-array update. Port-owned renderer patch
`0050-vertex-cache-invalidation.patch` retires the cached ranges lazily and marks
draw state dirty, preventing a merge across old/new snapshots. It does not add
an eager copy or alter array addresses, layout, endianness or original data.
Both local renderer profiles receive the fix; shared Aurora/decomp are untouched.
Broader vertex-array reuse was not introduced without an ownership proof.

The native test compiles the actual FIFO dispatcher, invalidator and array type;
GPU upload/draw consumers are modeled. It checks same-address mutations,
retained earlier snapshots, unchanged reuse, repeated invalidations, both FIFO
byte orders and 1,000 update/draw cycles. The pre-fix dispatcher failed on stale
geometry; both new tests pass. The full 142-test suite passed in 99.1 seconds.
An earlier full run timed out in an unrelated expected-failure register-cache
test; its focused rerun and the subsequent full suite passed. This is recorded
as a test-run instability, not silently counted as a passing run.

Windows and Android Release builds succeeded. All 178 Android game/platform
translation units retain exactly one `-O2`. Current native SHA-256:

- Windows: `00fb15bf6021ceb120cbe62bf862fa12f3b069ff943ec476ab2f1601105a2035`.
- Android unstripped: `fe4e9bd0bd35e4803e84d90933fe9f0bb6ca32270d1979a193a914aa91c6006c`.

The pre-change archives/native binaries are retained under
`build/checkpoints/vertex-cache-invalidation-20260911/{windows,android}`.
The comparison executables use identical current port/FI source and differ only
in renderer archives. All 24 fixed-seed, board-age-aligned desktop frames were
pixel-identical (12 AO off, 12 Strong AO). Average vertex/index/uniform/storage
upload bytes were also identical before/after in each scene. Storage averaged
609,403 bytes/frame with AO off and 636,283 with Strong AO. Evidence:
`build/engine-benchmark/vertex-invalidation-pixels.json`. This covers the W01
intro, not every gameplay mutation, and does not establish Android speed gains.

After the user unlocked the S22+, isolated Release-native/FXAA/4x-shadow/Fast
Forward runs resumed at 2340x1080. `fi-gate-unlocked-ao0-shadow4` produced five
warmed five-second windows with median 196.087 rendered presents/s;
`vertex-invalidation-ao0-shadow4` produced five with median 204.690. Both started
at reported thermal status 0, but this is one ordered comparison with differing
clock/cache histories, not a repeatable 4.4% gain or sustained 200 FPS guarantee.
The capture-seal bucket is now 0.002 ms; the unused FI work is removed without
a demonstrated end-to-end gain against the earlier 0049 phone runs.

Strong AO remains the larger limitation. `fi-gate-unlocked-ao2-shadow4` completed
with three warmed windows, median 80.581 presents/s and 10.237 ms GPU span. Its
GPU medians included 4.440 ms EFB, 2.285 ms GTAO and 0.760 ms depth snapshots.
The corresponding 0050 run hit thermal status 2 and stopped; its 73.945 median
must not be interpreted as a regression or a fair before/after result. Both
started in the explicit status-1 cohort, which does not guarantee equal clocks.
No AO resolution, sample count, shader, image quality or thermal control changed.

The separate `vertex-invalidation-ao2-cpu` capture recorded 4,214 app-only CPU
samples over 7.999 seconds with zero samples lost. Its authenticated symbols
show FIFO processing at 13.81% inclusive, Dawn queue submission at 19.88%,
Vulkan render-pass recording at 11.95%, `resolve_pass` at 8.70% and grounding
object processing at 2.47%. These are overlapping sampled user-space cycle
shares across threads, not milliseconds or blocked-time measurements. Multiple
threads share the name SDLThread: do not misattribute all driver work to the
game thread. The recording's FPS is profiling/thermal-confounded and excluded
from speedup claims. The snapshot/pass/command-submission path needs further
investigation before making a larger Android throughput claim.

After these runs the isolated device root and owned temporary profiler were
removed, and the original non-debuggable 0.4.19 APK was restored. Settings,
memory-card bytes and save-file inventory remain identical to the backup.
Host recordings and local profiling APKs remain under
`build/s22-performance-20260911`; these APKs must not be uploaded. The tablet's
separate cleanup remains outstanding. No APK was uploaded in this pass.

## 2026-09-11: unused Fast Forward capture (not uploaded)

The authorized S22+ app-only CPU recording contains 6,073 samples over eight
seconds (`build/s22-performance-20260911/current-fast-cpu-shadow4-retry`). It
uses Release native code in a debuggable profiling wrapper, FXAA, AO off,
Fast Forward, and 4x shadows. These are sampled user-space CPU-cycle shares,
not frame times: FIFO processing was 16.51% inclusive, Vulkan render-pass
recording 11.48%, and `mp6_fi_note_frame_end` 5.31% (4.36% self). Inclusive
owners overlap and must not be summed. GPU timestamp instrumentation was on.

The profile led to a port-owned scheduling defect: Fast Forward returned from
the tick throttle without any interpolation window, but Unlocked FPS still
copied, walked and paired the complete FIFO every tick. The capture-begin hook
now consults the scheduler's resolved rate. It stops retention only when the
throttle has no replay window, without changing the saved Unlocked FPS setting,
game tick behavior, rendering or AO. Resuming pacing re-arms clean history;
environment overrides still take precedence over saved preferences.

The real scheduler/capture fixture covers startup free-run, 100-frame no-work
loops, both live switches, feature toggles, invalid/explicit environment rates,
scheduler fail-safe and save-state reset. The original capture policy fails the
regression test. All 140 CPU/native tests pass. Both native Release builds pass;
all 178 Android game/platform TUs still compile with exactly one `-O2`.

The fixed-seed desktop comparison produced 24 pixel-identical board-age pairs
(12 AO off, 12 Strong AO), with FXAA and 4x shadows. Evidence:
`build/engine-benchmark/fi-free-run-pixels.json`. This is an image-equivalence
check, not an Android performance measurement. A short unseeded 6,500-tick boot
attempt never reached the board and is excluded. The seeded runs reached the
board and completed their 4,000-tick measurement windows.
The separate production Release smoke (`fi-gate-paced-release`) reached W01
and completed 4,200 ticks at the normal 60 Hz setting, with 166,009 present
submissions including interpolation. That verifies the paced replay path still
runs; it is not a claim that the monitor displays that many distinct frames.

Native SHA-256 after the capture-policy fix, before 0050:

- Windows: `5c2c477f17fa3904c473e78b21c0a8e5516c34822345dd414762746151d6adf2`.
- Android unstripped: `c68ca6dc31ce6d39b174d741643a85792e52852a5433a8dbb79bebdee654f9f4`.

The original 0049 native binaries and two changed port source files are retained
under `build/checkpoints/fi-free-run-20260911`. No decomp or shared Aurora source
was changed. The AO algebra experiment below remains outside production.
The first post-fix phone attempt was paused before rendering when its screen
turned off. The subsequent unlocked measurements are documented above; a
repeatable post-fix S22+ speedup is still not established.

## 2026-09-11: coherent statistics and S22+ profiling (not uploaded)

`0049-thread-safe-stats.patch` fixes a renderer data race shared by Android and
Windows. The worker previously wrote a public statistics struct while the game
thread read it, including plain reads of fields updated through atomic wrappers.
The worker now publishes a complete frame under a short mutex; callers receive
a thread-local copy. Pipeline totals have independent atomic storage. GPU work
is outside the lock, and disabled Tracy builds add no Tracy snapshot reads.
This is a correctness fix, not a claimed FPS improvement.

The native fixture exercises 200,000 frame publications and 600,000 concurrent
reads, verifies field relationships and snapshot-pointer lifetime, and rejects
the old live-pointer implementation. It is not a ThreadSanitizer run. The engine
benchmark also records sampled texture-upload bytes: missing measurements stay
missing rather than becoming zero. Because the renderer is asynchronous, these
are samples of the last completed frame, not an exact sum over unique frames.

Both native Release builds succeeded after 0049. Pre-capture-policy SHA-256:

- Windows: `19fdbba40e6de7801a98bc05f094e10530d12402448ef26fb00e09d787e8f226`.
- Android unstripped: `1cfd9f5d712ba368f0deee3e6d6ed25cb1dec10b12238512a62e9f6bd02ec54f`.

The CPU/native suite passed 135 tests, including six profiling safety tests;
the expanded nine-test profiling safety suite subsequently passed as well.
The earlier 55 setup and 32 NVIDIA/Vulkan GPU tests passed before 0049; shader
code is unchanged. All 178 Android game/platform translation units retain `-O2`.

The authorized SM-S906E S22+ (Adreno 730) is now connected. Its installed 0.4.19
APK, settings and cards were backed up under `build/s22-performance-20260911`.
Local debuggable wrappers retain Release native code and the installed version
code, so restoring the original does not need a downgrade or data wipe. These
wrappers are not release assets. Tests use their own settings, saves and GPU
cache; the imported disc content is referenced through a symlink. The player's
settings/cards were verified byte-identical after the first runs.

Initial AO Strong/FXAA/2340x1080 runs were thermally confounded: the installed
baseline warmed the phone before the current build ran. Reported thermal status
reached 3 and skin temperature about 43.7 C. Those runs do not establish a
regression or speedup. The profiling tool defaults to status 0 at launch,
with an explicit status-1 cohort option (never status 2),
records thermal samples, and stops its owned process at status 2 or at the time
limit. It does not modify thermal controls or device settings. A locked-screen
attempt produced no frames; a 50-second attempt reached W01 but not the required
600-tick warm-up. Neither is a valid warmed-board result.

Short warmed-board Fast Forward/AO-off/FXAA/4x-shadow windows subsequently
measured 194-198 FPS for the installed baseline (two five-second samples,
median 196) and 184-204 FPS for 0049 (three samples, median 198). Start thermal
conditions differed (status 0 vs 1), and these small samples are effectively
flat, not a demonstrated gain. A single 204-FPS window does not establish
sustained 200 FPS and is not an AO-on result. The separate CPU-recorded run
must not be used as throughput evidence.

The saved S22+ configuration uses `game.tick_hz=0` (Fast Forward), FXAA and 4x
shadows. The initial isolation used 60 Hz and 1x shadows. Those workloads must
not be conflated. The helper now exposes explicit tick-rate and shadow-quality
arguments and records the installed APK hash at launch. Player preferences are
not edited. Owned-process background/resume can retain the loaded scene while
cooling; it checks the foreground activity before sending Home and refuses to
stop or navigate a replacement/unowned process. Resumed timing summaries discard
another 300 ticks and retain the original 600-tick board warm-up requirement.

The 0049 desktop texture-counter smoke exited normally after its 4,000-sample
window (`build/board-qa-runs/stats49-texture-upload-smoke`). Sampled texture-upload
bytes were zero in this steady introductory scene; geometry/uniform/storage
uploads remained nonzero. This rules out continuous image reuploads in that
specific desktop window, not in every Android/gameplay workload.

An isolated AO open-arc algebra experiment is recorded under
`build/ao-open-arc-probe-20260911`. Twelve synthetic comparisons found a maximum
RGBA16F difference of 0.00048828125, with flat planes unchanged. Resident desktop
GTAO timings improved roughly 4-5% in the initial experiment, with variable clock
state. The candidate has **not** replaced the production shader, is not a full
image/scene audit, and has no measured Android gain.

After the phone locked again, its isolated test root and the owned temporary
simpleperf executable were removed. The original non-debuggable 0.4.19 APK was
restored with a replacement install (no uninstall or data clear). The installed
APK's SHA-256 again matches the backup exactly:
`dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
Settings, card bytes and save-file inventory were verified unchanged after
restoration. Host-side recordings and wrapper APKs remain for reproducibility;
the wrappers must not be uploaded. The later unlocked runs are documented above.
The disconnected tablet's earlier cleanup obligation is separate and remains
outstanding. No APK has been uploaded during this pass.

## 2026-09-11: image/palette cache dependencies (not uploaded)

Graphify's source-tracing fallback (no index present) identified incomplete cache
keys in the port-owned renderer. `0048-texture-cache-dependencies.patch` applies
to both Android and Windows. The shared decomp/Aurora checkouts were not edited.

- The bound-image fast path now checks source pointer, dimensions, source format,
  mip count and content version, plus the selected palette's identity, pointer,
  version, format and entry count. A missing GPU image is not a successful hit.
- Source keys are stored separately from the resolved GPU format and the live
  sampler words. A sampler-only change still avoids image conversion/upload;
  changing the allocated mip range does not reuse a wrongly sized mip chain.
- Secondary static-image and GPU-palette caches validate the same dependencies.
  Missing, removed, empty or invalid-slot palettes cannot leave a stale image
  bound. Anonymous palettes have no persistent cache owner.
- A newly allocated dynamic palette-conversion target is initialized even when
  both source/palette revisions are zero. Retired palettes cannot recreate a
  persistent cache entry.
- Replacement-cache clear requests are consumed before the bound-image fast
  path on the render producer. The ordinary path uses an atomic load, not an
  atomic exchange for every static resolve. Requests may wait until the next
  unmerged draw (at latest the next drawing frame). Already-destroyed source
  objects retain their uploaded image; their CPU pointer is not reread.
- Creating a GPU TLUT no longer discards valid static conversions of that same
  palette. Only stale dependent entries are removed. The native shared-palette
  scenario removes one unnecessary conversion/upload; this is not a measured
  board FPS gain.

The new native fixture compiles the actual cache functions and texture types;
only GPU resources/uploads/conversions and the hash-container implementation are
mocked. It tests 26 scenarios, including 10,000 unchanged resolves with forced
secondary-cache lookups, source changes, palette selection/changes, late palette
arrival, zero revisions, missing-image retry, cache clears and object retirement.
The first 18-case pre-change probe failed 16 cases and passed unchanged reuse and
retirement. A separate probe reproduced the redundant shared-palette upload
after the correctness fixes; selective invalidation then passed it. The sampler
fixture now extracts the actual extended binding type rather than a hand-written
layout. Its old-policy mutant still reproduces stale sampling.

Evidence:

- `build/texture-cache-before49-independent/results.json`
- `build/texture-cache-shared-before49/results.json`
- `build/texture-cache-final49/results.json`
- `build/checkpoints/texture-cache-dependencies-20260911/{windows,android}/`

Both native Release builds succeeded. Android's 178 game/platform translation
units each retain exactly one `-O2`. All 125 native/engine, 55 setup/release and
32 GPU tests passed (212 total; the native cache test contains the 26 scenarios).
GPU shader tests ran on NVIDIA/Vulkan, not Mali. AO shader source, samples,
strength, resolution and filtering are unchanged.

0048-only native SHA-256 (superseded by 0049 above):

- Windows: `6f805be1064859fb2c4c4dff6687a81945c5c756dddc90f1b6c86e4026069daa`.
- Android unstripped: `b5fe108a9b09eaef5f892bbf3e6e32bd3f98d85845b2e22dcbfb4d4eb8c0c4ef`.

Desktop evidence: 12 AO-off and 12 Strong-AO aligned board frame pairs were
pixel-identical (`texture-cache-pixels49-ao{0,2}.json`). The first six alternating
Strong-AO runs showed 4.45% lower median frame time, but a repeat showed only
0.51% (effectively flat). This does not establish a repeatable speedup. These
results cover the stationary introductory board, not a complete match. No APK
was uploaded. The later S22+ session is documented above.

This closes the tested palette/image dependency holes, not every cache/lifetime
or performance issue. Content fingerprints still sample source bytes rather than
prove complete byte equality. Copy-texture destruction cannot safely be treated
as a generic "force reread CPU memory" event: the port calls it while releasing
shadow buffers. Normal copy writes already invalidate aliasing binds. Broader
copy-source reuse/lifecycle coverage and real-device sustained performance remain
open.

## 2026-09-11: sampler-state coherence and rejected metadata optimization (not uploaded)

Source tracing found another image-cache correctness hole. The unchanged-image
fast path in `resolve_sampled_textures` compares image identity/version and can
skip copying newer sampler words into the resolved texture binding. Consequently
BP filter/wrap/LOD changes could leave the previous sampling settings active.
`0047-texture-sampler-coherence.patch` propagates the two sampler words at their
BP write sites. Unchanged BP writes still return through the existing register
cache; no per-draw lookup, texture conversion, allocation or upload was added.
Queued draws keep their already-created immutable sampler handles. The change
is shared by Android and Windows and does not modify decomp code or AO quality.

The new native test compiles the production BP texture cases and image-resolution
fast path. It exercises 8,000 sampler changes across all eight slots, retains the
converted image format and handle, and verifies that actual image-version changes
still resolve. Removing the two new assignments reproduces the stale-sampler
assertion. Metadata decoder tests also cover 100,000 mixed loads, both byte orders,
all fields, zero-ID barriers, prior dirtiness and malformed input. Profiling now
has a tested, diagnostic-only texture/palette load census and strict parsing.

Both native Release builds pass; all 178 Android game/platform TUs are verified at
exactly one `-O2`. All 124 engine/native, 55 setup/release and 32 GPU tests pass
(211 total; GPU tests use NVIDIA/Vulkan, not Mali). Twenty-four
board-age-aligned FXAA frame pairs (12 Strong AO, 12 AO off) are pixel-identical to
the pre-fix baseline. The current ground-level capture was visually reviewed.
Those capture jobs overlapped and **their timings are not performance evidence**.
This is introductory-board coverage, not a full-match or Android lifecycle pass.

Evidence: `build/engine-benchmark/sampler-coherence-pixels48-ao{0,2}.json` and
`build/checkpoints/texture-sampler-coherence-20260911/{windows,android}/`.
The checkpoint archives/source are pre-fix; their binary copies were explicitly
replaced with the authenticated pre-experiment binaries after capture of the
checkpoint, avoiding contamination by the rejected metadata candidate.

Final native SHA-256:

- Windows: `9433385f8834a9f481dddfe25c94a05bb6bc39b74428469a30d35d3ecb3ff187`.
- Android unstripped: `b3656387a5f38d85cf49d3b8d154ec53c62ae9578aebd94cd04fa23d0297a36b`.

### What was measured, not assumed

Rechecking the previous register-cache change with six alternating runs gave
1.089999 ms baseline versus 1.087577 ms current, effectively flat. This did not
reproduce the earlier 4% slowdown and does not prove a speedup or identify the
cause of the run-to-run spread (`register-coherence-recheck48-ao2.json`).

The final sampler fix's separate six-run comparison measured 1.096727 ms before
versus 1.119364 ms after (2.06% longer), with substantial within-build spread
(baseline 1.080682–1.198393 ms; current 1.075217–1.137919 ms). No run was dropped.
`sampler-coherence-steady48-ao2.json` records those samples. This is a retained
correctness fix, not an FPS optimization or proof that the performance goal is
closed; the cause of this desktop spread and mobile cost remain unresolved.

The instrumented board trace found 194.739 texture metadata loads per frame,
including 28 identical payloads, and 12.818 palette loads including 3.818 repeats.
A candidate skipped identical versioned texture metadata. Its apparent first-run
7.14% frame-time reduction did **not** reproduce: a second six-run comparison was
1.075657 versus 1.075614 ms (0.004%). Draw calls and upload bytes did not improve.
A separate optimized CPU benchmark of synthetic 195-load batches with 28 repeats
increased decoder time from 1,215.600 to 1,401.090 ns (15.26% longer). This is not a
captured FIFO trace or an Android result, but there is no demonstrated benefit to
offset the extra decoding cost. The candidate was removed from the active patch
chain and both renderer sources; final native binaries were rebuilt without it.

Reports: `build/engine-benchmark/texture-metadata-{steady,repeat}48-ao2.json`,
`build/texture-metadata-cpu48/summary.json`, and the diagnostic log under
`build/board-qa-runs/texture-load-census48-ao2/`. The rejected source/patch remain
only in `build/checkpoints/texture-metadata-reuse-20260911/`; the CPU benchmark
accepts explicit `--candidate-source` and `--baseline-audit` inputs for replay.

ADB still lists no device. There is no measured Android FPS improvement from this
batch and no 200+ FPS claim. No APK was packaged, installed or uploaded. Remaining
work includes real-device GPU/thermal measurements and checking palette dependency
changes against the unchanged-image fast path; the sampler test does not certify
palette invalidation. The previous tablet profiling-wrapper cleanup obligation
still applies when the device reconnects.

## 2026-09-11: register-cache correctness audit (not uploaded)

`0046-register-cache-coherence.patch` fixes incorrect skip decisions in the
shared renderer, not decomp gameplay. Raw-register equality alone was unsafe:

- CP matrix-index writes could change the decoded matrix while XF's cached raw
  value stayed unchanged. A later matching XF write was incorrectly skipped.
- BP GENMODE and XF both write lighting/texture-generator counts. The two cache
  paths now also check the shared decoded values before skipping. BP raw words
  are not invalidated or rewritten: masked BP writes still merge correctly.
- The first zero XF matrix-index write must replace the C++ identity defaults.
  An explicit, state-owned validity mask distinguishes unwritten from zero.
  The first zero GENMODE write likewise creates one TEV stage, not zero.
- BP palette-load commands must execute even when the destination/count word is
  unchanged; the source register may have changed. Copy triggers remain uncached.

The native fixture executes the production cache predicates and scalar decoder
blocks against the same decoder with skips removed. It covers 100,000 mixed
commands, selective BP masks, state reset/copy, first-zero initialization,
matrix/count cross-writers, palette reloads and repeated copy triggers. Compiling
the previous cache policy fails each of six independent regression sequences.
These reproduce decoder defects, not a claim that each caused an observed game
crash or that fixing them increases FPS.

Both port-owned renderer trees and native Release builds succeed. All 178 Android
game/platform translation units are verified at exactly one `-O2`. The final
121 engine/native, 55 setup/release and 32 GPU tests pass. The final twenty-four
board-age-aligned 1920x1080 frame pairs are pixel-identical: twelve Strong-AO/FXAA
and twelve AO-off/FXAA. A ground-level capture from the preceding cache version
was reviewed. This is board-introduction coverage, not a full-match pass.
Evidence: `build/checkpoints/register-cache-coherence-20260911/{windows,android}/`
and `build/engine-benchmark/register-coherence-final-pixels-ao{0,2}.json`.
The two final capture jobs overlapped; their timing fields are not throughput
evidence. The separate six-run throughput jobs below ran sequentially.

Repeated XF writes now avoid rewriting the cache/validity words when those
words already match. The initial six-run desktop throughput check measured
1.036029 ms before versus 1.047178 ms after (1.08% longer). After that store
cleanup, the final six-run comparison measured 1.038808 versus 1.080895 ms
(4.05% longer), with current runs spanning 1.045642–1.201099 ms; the slowest
included a 0.141148 ms post bucket versus roughly 0.014 ms normally. No run was
discarded. This is not evidence of an FPS improvement, nor does it isolate the
cause of the whole-frame spread. The confirmed correctness fix is retained;
mobile cost remains unmeasured. Reports are `register-coherence-steady-ao2.json`
and `register-coherence-final-steady-ao2.json` under `build/engine-benchmark/`.

Final native SHA-256:

- Windows: `a1768586c31246b20aa3e3c9060812457913b11a373caa74cc3971b231c07237`.
- Android unstripped: `2ea25553c0521093334f9339a1bebbe8295d571a4186511a33ddbaf85dbb8063`.

No Android device is currently visible to ADB. Mobile speed, lifecycle and thermal
verification remain open; this correctness fix is not a 200+ FPS result. No APK
was packaged, installed or uploaded for this batch.

### Early water rejection experiment — rejected

Water is already excluded from AO at exact full-resolution receiver pixels.
Two experiments moved that check ahead of the costly reduced-resolution
reconstruction, first retaining RGBA and then only checking alpha. Both retained
exact output in their synthetic benchmarks; the first also passed the expanded
32-test GPU suite with colored foliage/filtered edges. Both regressed land-heavy
desktop composition. The existing production shader was restored byte-for-byte
(SHA-256 `46883f1446303024cafe11017f6a27b17c07c96b2f2398ad71bdb89c3beba64b`).

Using the Mali-compatible shader with the Android 960-pixel AO budget on an
RTX 4090, at 1920x1080 the water-heavy composite improved about 51%, but the
feet-on-land composite became about 23% slower. At 2960x1848, the land composite
rose from approximately 0.031 ms to 0.109 ms. These are six-round medians after
two warmup rounds, 1,024 resident repetitions with the final 32 timestamped.
The underlying compiler/occupancy cause was not isolated, and the runs are not
Mali hardware measurements. A scene-specific win does not justify shipping the
regression. Neither candidate entered a native production binary.

Reports: `build/ao-water-rejection-compatibility-20260911.json` and
`build/ao-water-rejection-alpha-compatibility-20260911.json`. Frozen baseline and
both candidates remain only under `build/checkpoints/ao-water-composite-20260911/`.
The reproducible benchmark is `tests/integration/benchmark_ao_water_rejection.py`
with explicit `--baseline`, `--candidate`, `--compatibility` and a new `--output`.
The broader exact-pixel water/foliage regression remains in the GPU suite.

## 2026-09-11: vertex-format register cache (not uploaded)

`0045-vertex-format-register-cache.patch` removes redundant decoding and vertex
size-cache invalidation from the shared renderer's pure VCD/VAT register writes.
The source audit found these fields have only one writer, the CP decoder.
Matrix registers have XF writers too, and array commands can refer to mutated
memory at the same address; neither is cached. The bounded cache lives inside
GXState (26 words plus a validity mask), so resets/copies carry it with the
decoded state. A first zero write still decodes, and a repeated write never
clears dirtiness left by a different command. No heap allocation is added.

The isolated board census found 913.970 format-register writes per frame, of
which 851.970 (93.2%) repeat the current raw value. These counters are test-only;
they do not add logging or profiling work to the shipping game. AO quality,
render scale, geometry, texture detail and simulation speed are unchanged.

An alternating optimized Windows helper benchmark reduced a synthetic
915-command batch from 1,882.434 to 1,581.950 ns (16.0%). This is a tiny CPU
component, not a 16% game speedup. Six-run alternating whole-board comparisons
measured these desktop median frame times:

| FXAA configuration | Before | After |
| --- | ---: | ---: |
| Strong AO | 1.053735 ms | 1.033583 ms |
| AO off | 0.691157 ms | 0.691911 ms |

The AO-on median improves 1.9%; AO-off is effectively unchanged within the run
spread. These are uncapped Windows diagnostic runs, not Android measurements.
They do not establish 200+ mobile FPS or close the remaining GPU bottlenecks.

Both native Release builds pass, with all 178 Android game/platform TUs
authenticated at exactly one `-O2`. The native decoder tests cover more than
200,000 mixed writes, every bit of each of the 26 registers, first-zero/reset/
copy behavior, cached vertex-size validity and matrix/array exclusions. The
old decoder fails the repeated-invalidation regression. All 24 tick-aligned
1920x1080 before/after FXAA frame pairs are pixel-identical: 12 with Strong AO
and 12 with AO off. Asynchronous draw/upload counters are not frame-aligned,
so tiny differences in their averaged totals are not a draw-loss test.
All 205 tests pass: 119 engine/native, 55 setup/release and 31 NVIDIA/Vulkan GPU.
The final production MSAA 4x + Strong AO build also completes 5,400 ticks with
60 Hz simulation and unlocked replay (94.4 seconds, exit 0, no crash/validation
marker). All four captured frames are real replay presents at ticks 2,588-2,590
and were visually checked. Evidence: `build/board-qa-runs/vertex-format-shipping-replay/`.
This is a bounded renderer/replay check, not full-match or capsule certification;
the previously noted MSAA edge-speckling issue is not fixed by this cache.

Evidence: `build/checkpoints/vertex-format-registers-20260911/`,
`build/benchmarks/vertex-format-registers-20260911/`, and
`build/engine-benchmark/vertex-format-{throughput,pixels}-ao{0,2}.json`.
Windows SHA-256: `608b6e049e9964eee28ac3664c90e28e93aa4b81f2abfb7ee6b8cd7846e2d973`.
Android unstripped SHA-256: `60bce078be6596aee5a6738d13a0cddea75be3730616dd096204b8f92ee5575f`.
ADB reports no connected device. No APK was packaged, installed or uploaded;
the disconnected-tablet profiling-wrapper cleanup obligation below still applies.

### Empty depth-task pass experiment — rejected

An earlier candidate removed a loading-only pass before AO's depth-reading
task. The recorded scene/offscreen pass count fell from six to five, with
unchanged geometry and upload counts. However, the six-run AO-on desktop
median regressed from 1.103491 to 1.144244 ms (3.7% longer). It was withdrawn
from the active patch chain and both renderer trees/binaries were recovered
from authenticated pre-experiment checkpoints before building the vertex cache.
Do not reintroduce it as an established mobile gain: no device result exists.

The rejected patch, native fixture and binaries remain recoverable under
`build/checkpoints/empty-depth-task-20260911/`. Its profiles are
`post44-frame-ao{0,2}` / `post45-frame-ao2`; that last profile belongs to the
rejected candidate, not the final vertex-cache build. Capture results are
`empty-depth-task-ao{0,2}-aa{0,1,2}.json` (only the tested combinations exist).
MSAA showed small pixel variations in both before/after and unchanged-build
control runs (`empty-depth-msaa-control.json`); no exact MSAA equality is claimed.
Non-capture timing results are `empty-depth-throughput-ao{0,2}.json`.

## 2026-09-11: draw batching and index spans (not uploaded)

Patch `0044-draw-batch-index-spans.patch` addresses the shared draw-submission
path in both port-owned renderer trees. The shared decomp/Aurora checkouts are
unchanged.

- Generate each index span with one buffer write rather than repeated
  per-index appends/capacity checks. Single quads and triangles have fixed-size
  fast paths; larger spans reserve once and overwrite every reserved byte.
- Use 32-bit index counts and loop counters. A 65,535-vertex strip now retains
  all 196,599 indices rather than truncating the count. Batches cannot exceed
  the 16-bit vertex-count limit.
- Merge only adjacent compatible vertex formats and triangle geometry.
  A single line/point instance no longer makes that draw eligible to merge
  with subsequent triangles. Existing draw order and index winding remain.
- Consume incomplete primitive tails without uploading/drawing them; retain
  the supported three-vertex quad tail as one triangle. Empty line strips
  cannot underflow their instance count.
- Validate ordinary, sized and indexed draw lengths before reading/uploading,
  using remaining-byte comparisons and 64-bit payload arithmetic. The
  display-list reader likewise rejects wrapped indexed payload sizes.
- Keep these draw guards active in Release. This renderer's `CHECK` macro is
  debug-only; its `ASSERT` is always active. Tests now extract the real macro
  definitions and exercise both configurations, with fixture assertions still
  enabled. The earlier always-checking test shim did not model that difference.

No AO sample count, resolution, shader quality, geometry detail or simulation
rate is reduced. This does not remove the GPU bandwidth/shading bottleneck.

The paired Windows CPU microbenchmark includes the active Release guards.
Representative median nanoseconds per complete index-generation call:

| Input | Before | After |
| --- | ---: | ---: |
| One quad / 4 vertices | 4.197 | 3.045 |
| Quads / 256 vertices | 149.673 | 19.775 |
| Triangle strip / 1,024 vertices | 1,306.279 | 214.962 |

These are warm-buffer helper measurements, not whole-game FPS or Android
speedups. They do not establish the requested 200+ mobile FPS.

Verification:

- Windows and Android native Release builds pass. All 178 Android
  game/platform translation units verify at exactly one `-O2`.
- 117 engine/native, 55 setup/release and 31 NVIDIA/Vulkan GPU tests pass
  (203 total). Native draw tests cover all seven supported primitive types,
  short/maximum-length inputs, index sequences, batch boundaries, empty draws,
  both byte orders and malformed payloads. Display-list tests exercise the
  actual reader/optimizer, including truncation and valid indexed passthrough.
- Seven targeted regressions reject the preserved pre-fix functions at
  runtime under Release macro semantics: count width, vertex-format merging,
  line-to-triangle merging, vertex limits, empty strips, ordinary headers and
  indexed payload overflow. The corresponding fixed cases pass.
- All 24 tick-aligned 1920x1080 before/after frame pairs are pixel-identical:
  12 with AO off and 12 with Strong AO, both using FXAA. Draw counts and upload
  byte counts also agree. The isolated benchmark now resets its fixed RNG
  seed before board creation, not only at startup; different intro durations
  had consumed different random values and moved falling leaves in the first
  comparison. No scenery is hidden or masked for the comparison, and the
  production game does not receive either benchmark seed override.
- Final production AO/MSAA 4x reaches the board and exits at 60,000 ticks
  (50.8 seconds). AO/FXAA with 60 Hz simulation and unlocked replay exits at
  5,400 ticks (94.5 seconds); all four captures are replay frames at ticks
  4,563–4,586. Ground-level captures from both runs were visually reviewed.
  Existing MSAA edge speckling remains; this batch does not claim to fix it.
  These are bounded rendering checks, not full-match/capsule certification.
- ADB reports no connected device. No APK was packaged, installed or uploaded.
  Sustained Mali/Snapdragon frame times, thermals and mobile visual checks
  remain open, as does the broader optimization goal.

Evidence: `build/benchmarks/index-spans-20260911-release/`,
`build/benchmarks/draw-batch-regressions-20260911-release/`,
`build/checkpoints/draw-batching-20260911/{windows,android}/`, and
`build/board-qa-runs/draw-batch-{release-msaa4,shipping-replay}/`.
Exact image results: `build/engine-benchmark/draw-batch-seeded-ao{0,2}.json`.
Capture-run timing fields are not used as a performance comparison.

Final native SHA-256:

- Windows: `7ad46ab8b88b628d4eb52860d309df38d92400ebc24c51068005ff9b47e7b10c`.
- Android unstripped: `7ee3287f0686334bcdb8577090b068eab5dcd6f662c34cdab011f788b7aaca1c`.

## 2026-09-11: reuse immutable replay endpoints (not uploaded)

The next source-tracing pass followed frame sealing, model pairing and camera
snapshots into the replay builder. Every extra presentation was decomposing the
same two model matrices, recalculating their motion gates and recomposing the
same current endpoint. Camera-cut checks also repeated for each model matrix,
despite reading unchanged camera snapshots.

- Prepare matrix endpoints only on their first eligible replay. Reuse the
  decompositions, current-matrix reconstruction and gate measurements until
  capture, walking or pairing invalidates that window.
- Keep alpha, scale-hold/residual policy, pairing decisions, camera cuts, draw
  filtering and normal-matrix reconstruction unchanged. No simulation work is
  cached or skipped.
- Retain at most 4,096 endpoint records (approximately 1.50 MiB plus 4 KiB of
  state bytes), for the single active replay window. No preparation happens
  without interpolation. Excess records or allocation failure use the original
  calculation, with identical output rather than missing interpolation.
- Evaluate each camera's unchanged cut predicate once on demand per snapshot;
  snapshot/reset invalidates both stable and unstable results.
- Save-state reset frees endpoint storage and drops camera history. Feature
  reactivation starts with a new capture, not the previous timeline.

The alternating Windows CPU microbenchmark compares against the immediately
preceding indexed/contiguous-copy builder, not the much older per-command-copy
reference. Across 40 synthetic stream trials, median microseconds per replay:

| Replays per freshly sealed window | Before | After |
| --- | ---: | ---: |
| 1 | 4.250 | 4.250 |
| 2 | 4.125 | 3.750 |
| 4 | 4.125 | 3.250 |
| 8 | 4.094 | 3.063 |

These are replay-builder CPU measurements, not whole-game FPS or Android
measurements. They show no median first-replay improvement and approximately
9–25% lower builder cost when a window replays multiple times. They do not
remove GPU shading/bandwidth cost, or establish the requested 200+ mobile FPS.

Verification:

- Windows and Android native Release builds pass; Android verifies all 178
  game/platform translation units at exactly one `-O2`.
- 114 engine/native, 55 setup/release and 31 NVIDIA/Vulkan GPU tests pass
  (200 total).
- Exact replay-byte, AO-marker and diagnostic comparisons cover cold/warm
  cache, uncached allocation-failure fallback, a reduced cache limit, alpha
  sweeps, scale/residual policies, shear/rotation/scale/translation, mirrored,
  degenerate and non-finite matrices, re-walk/re-pair, predecessor replacement,
  camera cuts and actual save-state cache cleanup.
- Camera tests execute the production snapshot, cut and reset functions for
  300 snapshot sequences, including invalid cameras, disabled/FOV-changing
  cameras, invalid/up-vector cuts and changes to the live camera after capture.
  Repeated queries perform at most one predicate calculation per snapshot.
- Production AO/FXAA with 60 Hz simulation and unlocked replay reaches the
  board and exits after 5,400 ticks / 94.8 seconds, with four captured frames.
  The final capture was visually reviewed. This is not a full-match test.
- A separate run captures a state at tick 5,000 and exits normally. A fresh
  process restores it at tick 300, then continues with AO/replay until tick
  5,900. Restore succeeds and the existing guarded restored-state exit returns
  zero; this does not certify third-party teardown after restore.
- A second restored-state run reaches tick 6,200 and captures four post-load
  frames at ticks 5,379–5,401. The capture index confirms three replay frames
  and one real-tick frame; both kinds were visually reviewed at the players'
  ground-level board view. The first restore run's text trigger did not arm
  frame capture, so it supplies log evidence only.
- ADB has no connected Android device. No APK was packaged, uploaded or
  installed. Sustained Mali/Snapdragon throughput, thermals and mobile visual
  verification remain open.

Evidence: `build/benchmarks/replay-endpoints-20260911/`,
`build/recovery/replay_pair_baseline.inc`, and
`build/board-qa-runs/replay-endpoints-{release,save,restore,restore-view}/`.

Native SHA-256:

- Windows: `313d08cda2b528c822d886bf5b4394a165c593e7619802a1795b352bbdccf57f`.
- Android unstripped: `d9c53ad4fda88585ac266fbab5a0a88344207aa67522b1e566d170e4859df3a0`.

## 2026-09-11: frame-lifecycle follow-through (not uploaded)

The follow-up review found caller paths missed by the earlier helper tests.
Patch `0043-frame-lifecycle-followthrough.patch` fixes both port-local renderer
copies, while `src/gx/frame_interp.c` fixes the retained-stream bounds checks.
The shared decomp and Aurora checkouts are not modified.

- Remove all three eager 2,048-command reservations: GX-copy continuation,
  replay-copy-clear, and EFB resume. They now reach the existing bounded command
  recycler through the real `push_command` path. Reused lists remain empty and
  cannot retain previous draw ownership.
- Blocking frame-slot, staging-slot and buffer-map waits capture the progress
  epoch **before** testing availability. Completion between the failed probe
  and the wait cannot be mistaken for an event already observed. Existing
  timeout bounds and simulation deadlines remain unchanged.
- Present usable `SuccessSuboptimal` surface textures, then request refresh
  after successful presentation. Timeout, lost/outdated surfaces, failed
  presents, missing textures and non-presentable windows retain their recovery
  behavior.
- Mark the final EFB pass explicitly. The render worker checks pending,
  unthrottled depth-peek demand before encoding attachment stores and discards
  final depth only when no peek, depth snapshot or GX depth-copy consumer needs
  it. Intermediate AO/task/continuation stores remain intact. Requests arriving
  after that check stay pending for the next frame; they cannot read discarded
  depth. The next frame clears the EFB depth attachment.
- Reject oversized sized/indexed draw payloads before arithmetic can wrap.
  Indexed payload arithmetic is widened to 64 bits; sized draws compare against
  the remaining bytes before addition. Empty/out-of-range walker inputs fail
  without reading the buffer.

No shader, AO quality setting, resolution, geometry or game-rate reduction is
part of this batch. These are implementation corrections, not a measured
Android FPS claim.

Verification:

- Windows and Android native Release builds pass. Android authenticates all
  178 game/platform translation units at exactly one `-O2`.
- 112 engine/native tests, 55 setup/release tests and 31 NVIDIA/Vulkan GPU tests
  pass (198 total). Run the GPU group from `tests/integration`; its sibling
  imports are not package-qualified.
- New compiled-source tests run all three actual continuation constructors
  and `push_command` for 300 reuse cycles per case; frame/map/staging completion
  races run 1,000 times each. Surface tests execute the actual acquire/present
  branches. Depth tests cover final/intermediate passes, every reader kind,
  throttled requests and late requests.
- The new allocation, wait, surface and malformed-length regression oracles
  reject the preserved pre-fix code at runtime, then pass the fixed code.
- Production 1920x1080 board-introduction runs with AO/FXAA, AO/MSAA 4x and
  AO off/FXAA each reach the board, capture four frames and exit normally at
  60,000 ticks. Ground-level captures were visually reviewed. No renderer
  validation failure was found. This is not a full-match gameplay certification.
  Fast-forward SFX voice-limit messages and the pre-existing MSAA edge speckles
  remain; neither is presented as fixed by this work.
- A fourth AO/FXAA run uses 60 Hz simulation and unlocked frame replay. It
  reaches the board and exits normally after 5,400 ticks / 94.4 seconds, with
  over 205,000 presentations logged before shutdown. This exercises sustained
  replay/backpressure, not a controlled performance comparison.
- ADB reports no connected device. Sustained Mali/Snapdragon FPS, thermals,
  Android surface lifecycle and image quality still need device verification.
  No APK was packaged, uploaded or installed in this batch.

Evidence: `build/checkpoints/engine-followthrough-20260911/{windows,android}`,
`build/board-qa-runs/followthrough-{fxaa,msaa4,no-ao,replay}`, and the
`tests/test_frame_followthrough.py` / replay regression fixtures.

Native hashes:

- Windows: `3e0dd95490846842e81d001149905657d1c36cbc728732da2523c543821dfa13`.
- Android unstripped: `0aa0c1b0b89855a982b14aa3443bbcc516d88d8b993ec9711498744321ef8f66`.

## 2026-09-11: remaining engine pass implemented (not uploaded)

This completes the four implementation areas from the connected-tablet review,
not a claim that all engine bottlenecks are solved or that 200 FPS is achieved.
All changes are in this port and its owned renderer build trees; the shared
decomp and Aurora checkouts are unchanged.

- **Retained replay:** validate and index GX commands once at frame seal.
  Replays reuse immutable command offsets and lengths instead of parsing the
  full stream again. Adjacent NOPs coalesce without crossing AO boundaries.
  Storage is reused, and save-state reset frees the index. Matrix interpolation,
  draw bytes, event markers, copy-clear handling, and game timing are unchanged.
- **Scene rendering:** patch 0041 removes enabled ONE/ZERO replacement blending
  from truly opaque written channels. Actual translucency, logic operations,
  alpha write masks, and destination-alpha overrides keep their blend states.
  This exposes opaque rendering to mobile tile/hidden-surface paths; a device
  speedup is not yet measured.
- **AO bandwidth/compositing:** an ordered encoder task reads the EFB depth
  attachment and produces prepared R32 depth directly. This removes the final
  intermediate raw-depth snapshot when decal cleanup is needed; without decals,
  the existing snapshot work becomes preparation rather than adding a pass.
  MSAA still selects sample zero, matching the previous snapshot shader.
  Decal before/after snapshots and the foliage/water protection pass remain.
  The full-resolution composite uses four scalar sample values rather than
  dynamically indexed sample/coordinate arrays. The original contribution
  arithmetic and loop order are retained for exact rounding.
- **Backpressure:** patch 0042 adds a nonmutating resource preflight before
  replay building and SDL pumping. It waits for actual frame-slot, map, or
  bounded-queue progress, using only surplus before the same simulation
  deadline. Admission rollback does not notify itself. Time and interpolation
  alpha are resampled after waiting; no FPS cap or additional game tick is
  introduced. Existing blocking GPU-progress waits can wake on resource progress.

No render-resolution, AO sample-count, strength, normal-precision, filtering,
foliage/water coverage, geometry, or simulation-rate reduction is used. The
previously rejected native-letterbox presentation fusion and short Android
spin-tail experiment remain disabled.

### Verification and limits

- Android native Release: all 178 game/platform translation units verify with
  exactly one `-O2` and `-fno-strict-aliasing`. Windows native Release also
  builds. Both owned renderer archive manifests were reauthenticated.
- 108 engine/native tests and 55 setup/release tests pass. Additional profiling
  source checks pass after moving AO dimension diagnostics to the worker.
- 400 exact replay comparisons cover AO markers inside NOP runs and filtered
  offscreen brackets, changing CP/VAT vertex formats, ordinary/sized/indexed
  draws, matrix rewrites, malformed tails/markers, and allocation reuse.
  The candidate performs zero walker calls during replay.
- Native tests cover queue notifications, wake-before-wait, no rollback retry
  loop, resource preflight, task order, empty-pass depth stores, dimensions,
  multisampling, and rejected task admission. All 6,144 supported blend-mode /
  factor / write-mask / destination-alpha combinations pass.
- 31 GPU tests pass: 18 AO-quality tests, five prepared-depth tests, four exact
  fast-path tests, three direct-attachment tests, and one opaque-blend test.
  Direct depth matches the old snapshot-plus-preparation path exactly, including
  deliberately different MSAA samples. Blend tests compare 32 format/sample/mask
  combinations on UNORM and sRGB targets. GPU tests ran on the Windows NVIDIA
  Vulkan adapter, not the Mali tablet.
- Production Windows board-introduction runs with AO/FXAA, AO/MSAA 4x, AO/SSAA,
  and AO off reached the board and exited normally at 60,000 ticks. A 60 Hz
  simulation/unlocked-replay run exited normally after 5,400 ticks and 186,790
  completed presents; AO encoded/composited counts agree at 112,210. These are
  bounded rendering checks, not full-board-cycle completion or mobile benchmarks.
- Captured ground-level FXAA, MSAA and AO-off frames were visually reviewed,
  along with the replay transition. Fine MSAA edge speckling also appears in
  the preserved pre-pass executable; this batch does not claim to fix it.
  Random leaf placement differs between native runs, so those screenshots are
  not presented as pixel-identical before/after evidence.

Evidence lives under `build/board-qa-runs/engine-pass-*` and
`build/checkpoints/engine-pass-20260911/{windows,android}`.
The run named `engine-pass-release-msaa` used AA mode 3 (SSAA); the actual MSAA
4x run is `engine-pass-release-msaa4` (mode 1).
The initial 14,000-tick FXAA run ended before board entry and is not counted as
a successful board check.

Native SHA-256:

- Windows: `ffb184d3f0eef65f0523c68d081ceafad9acf6a74346300065ac2f23e2b331af`.
- Android unstripped: `5e4b8bed62f50ee5ddbf54d56a8226ca764f2401a2edf4956b22044a01dc04b5`.

**No new APK was packaged, installed, or uploaded in this pass.** ADB lists no
connected device. Mali startup, thermals, memory use, and same-scene frame-time
regressions still need an on-tablet A/B before calling this a measured speedup.
The earlier profiling-wrapper restoration/cleanup obligation remains below.

## 2026-09-10: private 0.4.19 published — cross-cutting engine audit

See [the engine pipeline audit](ENGINE_PIPELINE_AUDIT.md) for the complete
finding/fix/verification ledger. The batch addresses AO/shadow target-cache
thrashing, repeated GPU binding and mip-view creation, redundant presentation
uniform uploads, quadratic upload-list publication, discarded command capacity,
callback-record copies, the shared cache clock, final UI stores, and heap/I/O
work inside the audio mixer lock.

All changes are shared by Windows and Android. No shader quality, resolution,
gameplay or filtering reduction is used. Both native Release builds pass;
97 engine/native and 55 setup/release tests pass. The 24 matched AO-on/off
captures are pixel-identical; these are correctness checks, not mobile timing.
The audit document records memory-test sensitivity and untested cases.

[Private Release APK](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-engine-audit-0419-20260910)
contains this batch and the prior private fixes. Android FPS gains remain
unmeasured without a connected device.

- Version: `0.4.19-preview-2a62946`, code `40019`.
- APK: `mp6-v40019-2a62946-dirty-f0b6a026-arm64-v8a.apk` (31,393,647 bytes).
- SHA-256: `dcf51f4500a9c4567c5b12503002c4c0c71c6eec31b329be6b688f41a226113d`.
- Native Android library retains the audited unstripped hash recorded in the
  audit ledger. All 178 game/platform units verify at Release `-O2`.
- Reran 97 engine/native and 55 setup/release tests. Packaging and lint passed
  with no new issues (existing baseline: 53 errors and 42 warnings).
- Same private-preview certificate; verified signature and 16 KiB page alignment.
  Downloaded draft assets matched local bytes before publication. APK version,
  exact native libraries and launcher-only assets were rechecked. No game/save
  data or source checkpoints were uploaded.

The 544-file checkpoint at `build/checkpoints/engine-audit-0419-release-source/`
predates this publication note. Previous local APK output was backed up there
before packaging cleanup.

## 2026-09-10: private 0.4.18 published

[Private Release APK](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-direct-snapshot-0418-20260910)
contains 0039 direct offscreen snapshot export plus all private 0.4.17 fixes.
The "not uploaded" statement in the development checkpoint below is historical.

- Version: `0.4.18-preview-2a62946`, code `40018`.
- APK: `mp6-v40018-2a62946-dirty-6519cf4c-arm64-v8a.apk` (31,389,551 bytes).
- SHA-256: `a92f7d6e3f0dd9238bb52d446ac49af5966f96034397026b10e799b1f1f2b1a7`.
- The native game library retains the tested `4576d21d...a35081` unstripped hash;
  all 178 native translation units verify at Release `-O2`.
- Reran 93 engine/native and 55 setup/release tests. Packaging and lint pass
  with no new issues (existing baseline: 53 errors and 42 warnings).
- Same private-preview certificate, verified signature and page alignment.
  Downloaded draft assets match local bytes; APK version, native libraries and
  launcher-only assets were rechecked before publication. No game/save data or
  source snapshots were uploaded.

No Android device was connected; S22+/Mali FPS improvement remains unmeasured.
The 538-file release-source checkpoint under
`build/checkpoints/direct-snapshot-0418-release-source/` predates this note.
Old local APK output was backed up there before the packaging clean.

## 2026-09-10: direct AO coverage export (after private 0.4.17)

The user identified the latest 48 FPS capture as **Galaxy S22+ Snapdragon,
FXAA, AO on**, not the Mali tablet. It reports 20.449 ms GPU span, including
8.016 ms EFB and 3.962 ms GTAO. Source tracing corrects the earlier interpretation:
the EFB timing includes the full-resolution AO composite, so it is not a pure
geometry timing. The separately named AO/depth/blur passes total 8.069 ms but
exclude the composite and coverage-mask rendering.

`0039-direct-offscreen-color-snapshot.patch` removes a redundant full-resolution
color copy from the AO foliage/water protection path. Public `create_pass` now
renders into a frame-owned snapshot texture. A color resolve exports that exact
texture instead of allocating another texture and copying into it. Exported
textures count as pass consumers and remain unique within the frame. Legacy GX
offscreen targets keep their copy fallback. Requested depth snapshots, depth
peeks and GX depth copies retain their required depth stores; otherwise an
offscreen depth attachment can be discarded because its next use clears it.
Suspended EFB attachment state is restored independently.

This changes attachment ownership and lifetime, not shader math, AO samples,
resolution, strength, geometry or foliage/water classification. Mali's
conservative snapshot policy is unchanged. It targets unnecessary memory work
on both Android and PC; it does not account for the whole measured GPU frame.

### Verification

- 93 engine/native tests and 55 setup/release tests pass. Native mock tests cover
  repeated exports, unique texture ownership, depth consumers, discarded passes,
  legacy copy fallback, and EFB restoration on desktop and Android/vendor paths.
- Twelve tick-aligned 1920x1080 Strong-AO/FXAA before/after captures are
  pixel-identical. Both runs exit normally with complete GPU timing windows.
- The production Release MSAA/Strong-AO smoke exits normally at 12,500 ticks;
  all four captured board-introduction frames were visually reviewed. This is
  not a full-match or capsule-event completion test.
- AO-off/FXAA runs also exit normally, retain identical draw/upload counts,
  and pass visual review. Their animation clocks differ by 60 ticks, so those
  captures are not pixel-identical and are not presented as a pixel proof.
- Windows and Android native Release builds pass; all 178 Android native
  translation units verify at `-O2`. These capture runs establish correctness,
  not an Android performance improvement. No Android device was connected.

Evidence: `build/engine-benchmark/direct-offscreen-pixels-ao{0,2}.json`, board QA
run `direct-offscreen-release-msaa`, and pre-change renderer checkpoints under
`build/checkpoints/direct-offscreen-snapshot-20260910/`.

Native SHA-256:

- Windows executable: `7cb45461d65cf92ca3451933825e8320b1cc9998091a318492fdd7fd2e05cc30`.
- Android unstripped library: `4576d21da0fc2cc915ab8583f74a26d63efb3b16ad4994605e50bf97a1a35081`.

**Not packaged or uploaded.** The latest private APK remains 0.4.17 below and
does not contain 0039. S22+ and Mali FPS gains, including the requested 200+ FPS
target, remain unmeasured.

## 2026-09-10: private 0.4.17 published

[Private Release APK](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-ao-scheduling-0417-20260910)
includes the post-0.4.16 renderer work (0033-0036) and depth-aware AO scheduling
(0038). The matrix-concat experiment remains disabled; frontend benchmark
instrumentation is not in the player build. Older sections' "not uploaded"
statements below describe their original checkpoints, not current publication.

- Version: `0.4.17-preview-2a62946`, code `40017`.
- APK: `mp6-v40017-2a62946-dirty-21654ee1-arm64-v8a.apk` (31,389,551 bytes).
- SHA-256: `a736417d2a39cbe0b51004203cd4c5f49da4088302b40242fab5d67d87c5bc91`.
- Native library remains the verified `01c57eb9...ca11c` build recorded below;
  all 178 native translation units verify at `-O2`. Same private signing certificate.
- Reran 93 engine tests and 55 setup/release tests. Release packaging and lint
  pass with no new lint issues (existing baseline: 53 errors, 42 warnings).
- Downloaded draft APK and metadata match local bytes; APK contents, native
  hashes and version were rechecked before publishing. No user game/save data
  or source files were uploaded.

No Android device was available: this publication does not establish a tablet
FPS improvement or full-match gameplay validation. The release-source checkpoint
predates this publication note and is retained under
`build/checkpoints/ao-scheduling-0417-release-source/`.

## 2026-09-10: AO-off frontend diagnosis

No production optimization or APK change in this follow-up. The Windows/Android
native hashes remain `2b2dec70ad81b8a302fa705dcfb266e45ab812537af5fd237fd4f1abf776b0d1`
and `01c57eb9a1e7fc1fc82ac52b77869daadfedcfcce17fe6afee9cf93bca7ca11c`.

The isolated full-frame profiler now measures CPU frontend stages as well as
render-worker work. Its timers are thread-local, report inclusive and exclusive
time, and count recursive calls without double-counting recursive inclusive
duration. The profiler also has fail-closed source anchors and strict row parsing.

Two complete 4,000-frame Windows/1920x1080/FXAA/AO-off runs:

| Instrumented cost, ms/frame | Stationary welcome scene | Warm-cache live board input |
| --- | ---: | ---: |
| FIFO processing, inclusive | 0.444299 | 0.388978 |
| FIFO exclusive of draw preparation | 0.108904 | 0.089170 |
| Draw preparation, inclusive | 0.335395 | 0.299807 |
| Draw preparation exclusive of named children | 0.133322 | 0.121058 |
| Pipeline configuration | 0.012713 | 0.011666 |
| Pipeline lookup | 0.048694 | 0.043998 |
| Texture resolution | 0.012316 | 0.012850 |
| Texture bindings | 0.048424 | 0.044114 |
| Uniform packing | 0.022988 | 0.020047 |
| Array bindings, inclusive of storage copies | 0.040875 | 0.032746 |
| Command recording | 0.016061 | 0.013328 |

**Do not sum inclusive rows.** This instrumentation has a measurable cost:
empty nested scopes at approximately this draw count take about 0.101 ms/frame.
The calibration is not an exact correction for code-generation/cache effects.
It prevents treating the raw exclusive draw bucket as entirely production work.
Command-list append, uniform packing and configuration population alone are not
large enough to explain the complete frame. The next investigation should
measure repeated state/configuration assembly across actual GX submissions,
including both the game's command producer and the renderer's consumer; these
measurements do not justify another matrix arithmetic micro-optimization.

The live-input run records 261.212 GX draws/frame and 20,038.151 submitted vertex
references/frame; the final 120 GPU frames report 0.063650 ms GPU span on the RTX
4090. That establishes desktop CPU-side cost, not the Mali bottleneck. No Android
gain is inferred. The cold live-input attempt overlapped the CPU test suite and
is excluded from timing comparisons; the warm run was repeated after that suite
finished. These are diagnostic windows, not full-board completion tests.

Evidence: `build/engine-benchmark/front-profile-summary.json`,
`front-profile-clock-calibration.txt`, and board runs
`root-front-record-profile-ao0` / `root-front-live-board-warm-ao0`.
All 93 CPU tests pass after extending the profiler.

## 2026-09-09: depth-aware AO scheduling implemented

The preceding whole-frame diagnosis led to a port-owned scheduling fix, not
a lower-quality shader. `0038-efb-depth-content-serial.patch` tracks potential
GX depth-writing commands on the EFB, including merged and indexed submissions
and excluding private offscreen targets. Reading the serial drains pending FIFO
commands but does not split a render pass, snapshot a texture or wait for the GPU.

The port's camera-start hook and the boundary after `Hu3DZClear` reset a
per-camera epoch. A camera with no subsequent depth writes skips foliage replay,
depth snapshot, GTAO, both blur passes and composite. The foliage registry is
still retired for that camera. Missing epochs are conservative; new admitted
frames invalidate old epochs. No camera number is disabled. Retained/FI playback
uses only the AO markers admitted by the real game frame, without rechecking
game-only epochs. Samples, resolution, strength, denoising and geometry are
unchanged.

### Verified work reduction and limits

In the same fixed-seed stationary Towering Treetop scene, Strong AO now schedules:

| Work per frame | Before | After |
| --- | ---: | ---: |
| GTAO evaluations | 3 | 1 |
| Blur passes | 6 | 2 |
| EFB depth snapshot requests | 5 | 3 |
| Scene/offscreen passes (not AO shader passes or final present) | 7 | 5 |
| Staging copy commands | 25 | 19 |

The same 322.794 GX draws/frame, 30,724.552 source vertex references/frame and
1,197,979.888 staging bytes/frame remain: this removes scheduling work, not meshes.
The diagnostic GPU span was 0.383266 ms in the new run (final 120 frames, no
drops); this is not a controlled Android speed comparison.

Six alternating uninstrumented-performance runs per setting, 4,000 board frames
each, Windows RTX 4090, 1920x1080/FXAA, no interpolation or visible stat panels:

| Median full CPU frame time | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Strong AO | 1.129987 ms | 1.091836 ms | 3.38% |
| AO off | 0.736518 ms | 0.739086 ms | -0.35% (essentially flat) |

These results do **not** establish 200–300 FPS on Android or solve the remaining
AO-off renderer cost. No tablet is connected. The redundant work is removed on
both platforms, but a Mali measurement is still required.

Validation: 93 CPU tests and 55 setup tests pass. Twelve board-age-aligned capture
pairs were visually reviewed; small animation differences remain (mean absolute
RGB delta 0.0174–0.1120/255), so pixel identity is not claimed. A real MSAA turn
completed with secondary cameras 1 and 2 admitted when they produced geometry.
The MSAA/unlocked-rendering run also admitted all three cameras, reaching 143,803
presented frames at tick 4,873, but hit its 90-second limit before turn completion:
partial runtime evidence, not a completed-turn pass. Both native Release builds
completed; Android verifies all 178 translation units at `-O2`.

Artifacts: `build/engine-benchmark/depth-schedule-ao{0,2}.json`,
`depth-schedule-ao2-captures.json`; `build/board-qa-runs/depth-schedule-frame-ao2`,
`depth-schedule-dice-msaa-fast`, `depth-schedule-dice-msaa-fi`.
The pre-change sources, renderer archives and baseline executable are retained
under `build/checkpoints/ao-depth-scheduling-20260909/`.
No APK was packaged or uploaded for this change.

## 2026-09-09: whole-frame diagnosis before further optimization

This earlier investigation stopped arithmetic and wait-loop candidates and traced
the full frame instead. **No new production optimization or APK is claimed by
this section.** The Windows and Android release sources/archives are unchanged;
the additional renderer instrumentation is compiled into copied archives under
`build/engine-benchmark/current-frame/frame-profile-renderer/` only.

### Confirmed scheduling defect

In the fixed-seed Towering Treetop board scene, the port submits **three GTAO
evaluations per frame**: cameras 0, 1 and 2. All three use full-screen viewport
dimensions and run both blur passes. The two secondary cameras have no scene
depth in this measured state:

- Camera 1 submits 48 GX draws, but only one enables depth writes.
- Camera 2 submits one GX draw, which enables depth writes.
- Both single depth writes are the `Hu3DZClear` quad issued by the corresponding
  board camera's layer-0 hook. That function disables color writes and writes a
  flat far-plane depth. The other camera-1 draws do not update depth.

The ownership error is in the port's **AO admission/scheduling**, not in the
GTAO integral: `src/hsf/mp6_ambient_occlusion.c::mp6_ao_camera_end` dispatches AO
for every valid camera after `Hu3DExec`'s camera loop, without checking whether
that camera produced AO-relevant depth. `src/gx/ambient_occlusion.cpp` then
snapshots depth and schedules GTAO, blur X, blur Y and a composite for it.

The resulting change (implemented above) is depth-content tracking across camera starts,
explicit depth clears and actual EFB depth-writing draws. **Do not disable
camera 1 or 2 by number**: another event can draw real geometry through them.
Retained/interpolated frames must preserve the admitted schedule. This issue
explains wasted AO work, **not the whole AO-off Android performance problem**.

### Full-frame evidence

Instrumented Release (`-O2`, `NDEBUG`), RTX 4090 Windows, 1920x1080, FXAA,
free-running fixed-seed board, no interpolation or visible stats panels:

| Work per frame | AO off | Strong AO |
| --- | ---: | ---: |
| Recorded scene/offscreen passes, excluding AO shader passes and final present | 1 | 7 |
| EFB depth snapshot requests | 0 | 5 |
| GX draw commands after merging | 298.794 | 322.794 |
| Submitted source vertex references | 29,989.552 | 30,724.552 |
| Draw elements including indices/instances | 42,677.688 | 43,664.688 |
| Staging buffer copy commands | 4 | 25 |
| Staging bytes | 1,144,115.888 | 1,197,979.888 |

The existing console's approximately 1,000-vertex reading is incomplete:
`mp6_console_note_prim` is called by the immediate `GXBegin` bridge, not by
display-list mesh draws. The new submitted-reference counts are **not** counts
of unique vertices or measured vertex-shader invocations.

The AO-off trace's frame/staging admission averages are 0.000161/0.000086 ms;
worker command encoding, queue submit and present average 0.110987, 0.146121 and
0.081549 ms respectively. CPU work on different threads overlaps: these rows
must not be added into a fabricated frame total. Likewise, `vi-post` includes
admission waits and the existing `game`/`submit` split moves FIFO-processing work
between buckets when AO forces earlier drains. Neither label alone identifies
the cause of a slowdown.

The camera-separated GPU run reports camera-0 GTAO at 0.135774 ms, while camera-1
and camera-2 GTAO plus their four blur passes total 0.060596 ms. This excludes
their depth snapshots, EFB continuations, composites and CPU submission cost.
These are the final 120 completed GPU frames; the CPU/work window is 4,000
frames. **These are desktop diagnostics, not Android FPS or predicted gains.**

Run artifacts under `build/board-qa-runs/`:

- `root-frame-profile-ao0-1080`: AO-off workload and wait baseline.
- `root-frame-profile-ao2-1080`: scene-pass/snapshot expansion.
- `root-camera-profile-ao2-1080`: separate GPU rows for each camera.
- `root-camera-depth-profile-ao2`: per-camera depth-writing draw census.

Reproduce the isolated build with:

```text
rtk proxy python tests/integration/build_engine_benchmark.py --name current --upload-stats --frame-profile
```

Use that `current-frame/release/mp6native.exe` with `run_board_qa.py`,
`--gpu-timings`, a fixed 1920x1080 window, and a 200,000-tick startup budget.
The benchmark terminates after its 4,000-frame board window; no user saves are
used. Its extra clocks/counters are diagnostic overhead, not a shipping speed
comparison. The three new profiling tests pass; the prior 87-test CPU suite also
passes.

Arm's [render-pass guide](https://documentation-service.arm.com/static/6560a5942c8b3557fee70a16)
explains why unnecessary framebuffer stores/reloads can be costly on tile-based
GPUs. That makes the observed pass expansion a relevant Mali target, but hardware
counters are still needed to distinguish bandwidth, shader and tiler limits on
the user's tablet. No tablet is connected; that limitation remains explicit.

## 2026-09-08: pipeline-owned draw layouts after private 0.4.16

Port-owned `0035-pipeline-owned-draw-layout.patch` removes repeated shader-layout
analysis from cached GX draws on both Windows and Android. The existing ready
pipeline entry now retains its immutable `ShaderInfo`; the required-pipeline
lookup returns both the pipeline and layout. There is no second hash table,
configuration copy or extra lock acquisition. Actual matrices, texture resources
and per-draw values are still read and uploaded normally. Entries refresh if
uniform alignment changes and are discarded with the device's pipeline cache.
Pending compilation, blocking first-use waits and persistence retain their
existing behavior. This does not change shaders, graphics quality or game logic.

This is distinct from the rejected separate shader-layout cache documented below:
that candidate added another full-configuration hash and lookup to every draw.

### Incremental timing evidence

The baseline already includes compact uniforms, pass-local bindings and lazy
pipeline callbacks (patches 0032-0034). Conditions: Windows RTX 4090, 1920x1080,
FXAA, AO off, warmed pipelines, free-running simulation, no interpolation,
statistics panels or readbacks, 4,000 frames at board ages 1800-5799. Each batch
has six alternating runs; neither batch ran alongside another build or QA game.

| Per-run mean, then median across variants | Before 0035 | With 0035 |
| --- | ---: | ---: |
| Whole-frame interval, first batch | 0.758211 ms | 0.736625 ms |
| GX submit, first batch | 0.375222 ms | 0.350185 ms |
| Whole-frame interval, repeat batch | 0.754009 ms | 0.724184 ms |
| GX submit, repeat batch | 0.372376 ms | 0.344239 ms |

These are 2.85% and 3.96% lower frame time, and 6.67% and 7.56% lower submit
time, respectively. There is still run variance, including in unchanged game
work; these are desktop measurements, not predicted Mali gains. Buffer bytes
remain identical in every timed run: 193,269 vertex, 84,363 index, 257,080 uniform
and 609,403 storage bytes/frame. Results are `pipeline-info-steady-ao0.json` and
`pipeline-info-final-ao0.json` under `build/engine-benchmark/`.

A separate six-run Strong-AO batch (`pipeline-info-steady-ao2.json`) measures
1.113364 ms before and 1.111451 ms after: only 0.17% less frame time, within
run-to-run variation. No meaningful AO-on throughput gain is established by
this CPU-side change. Do not generalize the AO-off improvement to AO-on play.

Reproduce with the retained, audited pre-change renderer checkpoint:

```text
rtk proxy python tests/integration/build_engine_benchmark.py --name baseline --renderer-checkpoint build/checkpoints/pipeline-draw-info-20260908/windows --upload-stats
rtk proxy python tests/integration/build_engine_benchmark.py --name current --upload-stats
rtk proxy python tests/integration/run_engine_benchmark.py --name new-pipeline-layout-comparison --ao 0
```

All 82 CPU/native and 55 setup/release tests pass. The production cache-code
harness now also exercises 100,000 layout hits without allocations, configuration
copies or repeated analysis, plus alignment changes, cache recreation, queued
prewarming, threadless compilation and shutdown wakeup. Both native Release
targets build; all 178 Android game/platform translation units are verified at
`-O2`. Native SHA-256 identities:

- Windows: `51bfa3eb78abeeb4aed8fdc005b2a4ffc28953cb3518fc4fd81b709776cbcd9b`.
- Android unstripped: `52fa2bcd3c197eadc64da30f73d6b86b05757c0d7267fe5f76cc98dc2ef0e189`.

All twelve paired AO-off captures are pixel-identical
(`pipeline-info-pixels-ao0.json`). Twelve Strong-AO pairs have a 30-tick absolute
animation offset: 1,154-4,580 pixels differ per 1920x1080 frame (under 0.23%).
Reviewed geometry, ground, characters, AO and HUD remain intact; this AO-on
comparison is visual smoke evidence, not pixel identity. Capture runs are not
used for timing claims. The production Release MSAA/Strong-AO smoke
`pipeline-info-release-msaa` completes 12,500 ticks with 23,330 matching AO
encode/composite callbacks and four reviewed captures. Existing sound-entry
warnings remain; the normal shutdown reports an aborted pending buffer mapping
and device destruction. This is not a full-board gameplay test.

The 300-second production run `pipeline-info-release-leakgate` passes its
500 KB/min steady-state gate after 60 seconds of warmup: RSS 598 to 597 MB,
slope -190.1 KB/min, handles 926 to 911. Private commit decreases from 1,548 to
1,545 MB. The sampled working-set variation gives a 343 KB/min resolution,
below this gate's threshold. No growing trend is observed in this stationary
scene; it does not exclude smaller leaks or replace full-board lifecycle testing.

This and patches 0033-0034 remain **local native builds, not uploaded APKs**.
The private uploaded release remains 0.4.16. No Android hardware timing or
200-300 FPS result is established.

## 2026-09-08: draw submission work after private 0.4.16

Two additional port-owned renderer patches apply equally to Windows and Android:

- `0033-reuse-gx-pass-bindings.patch` tracks the current GX texture binding and
  Uint16 index buffer within a render pass. Repeated bindings avoid API calls
  and texture-cache lookups. Indexed draws use the original byte offset divided
  by two as `firstIndex` into the same buffer; vertex indexing and instance
  counts are unchanged. State is local to the pass and invalidated around clear,
  RmlUi and custom draws. No cache survives a pass or retains resources.
- `0034-lazy-pipeline-creation-callback.patch` removes eager owning callbacks
  from ready-pipeline lookups. Previously every draw copied its large pipeline
  configuration into a heap-allocated `std::function` before checking the
  cache. A typed creation function now borrows the configuration for immediate
  creation; only a newly queued miss makes an owning copy. Compilation
  priority, required-pipeline waits, background prewarming, first-use metadata,
  configuration hashes and shader output remain unchanged.

These change CPU submission work, not resolution, AO/AA sample counts,
materials, geometry, particles, lighting or game logic. They do not restore
the previously withdrawn Mali AO fast paths.

### Timing evidence

The baseline is the compact-uniform renderer shipped in 0.4.16, with identical
production game units and benchmark instrumentation. Conditions: Windows
RTX 4090, 1920x1080, FXAA, AO off, warmed pipelines, free-running simulation,
no interpolation/stat panels/readbacks, board ages 1800-5799. Each batch has
six alternating runs.

| Per-run mean, then median across variants | Baseline | Both changes |
| --- | ---: | ---: |
| Whole-frame interval, first batch | 0.806637 ms | 0.752143 ms |
| GX submit, first batch | 0.415234 ms | 0.370374 ms |
| Whole-frame interval, repeat batch | 0.808763 ms | 0.722761 ms |
| GX submit, repeat batch | 0.412717 ms | 0.354791 ms |

The batches show 6.76% and 10.63% less frame time, respectively. The first
batch includes brief image-conversion work during its baseline run; the repeat
has no concurrent build, image conversion, or other agent-started QA run.
There is still appreciable run variance, including in unchanged game-side
work. Treat this as a promising desktop improvement, not a precise universal
percentage. Results: `lazy-pipeline-steady-ao0.json` and
`lazy-pipeline-final-ao0.json` under `build/engine-benchmark/`.

The binding-only candidate measured 0.65% less frame time
(`gx-bindings-steady-ao0.json`); most of the combined change comes from removing
eager pipeline callbacks. Buffer traffic is unchanged from 0.4.16: median
193,269 vertex, 84,363 index, 257,080 uniform and 609,403 storage bytes/frame.

Reproduce against the audited 0.4.16 renderer checkpoint:

```text
rtk proxy python tests/integration/build_engine_benchmark.py --name baseline --renderer-checkpoint build/checkpoints/gx-pass-bindings-20260908/windows --upload-stats
rtk proxy python tests/integration/build_engine_benchmark.py --name current --upload-stats
rtk proxy python tests/integration/run_engine_benchmark.py --name new-submission-comparison --ao 0
```

82 CPU/native tests pass. New production-code harnesses check 200,000 draws
against independent addressing/state semantics and 100,000 ready-cache lookups
with zero allocations or configuration copies. Deferred callbacks are invoked
after caller data is mutated and destroyed; tests also cover pending reuse,
blocking waits, threadless compilation, budgeting and first-use persistence.
The 55 setup/release regression tests pass as well. Both native Release targets
build, including verification of all 178 Android
game/platform translation units at `-O2`.

Native identities for these post-upload changes:

- Windows: `454345612cbdd849d8871ead9d50a6041e57f8fe0f51f6f8be5d8af5d1d541b2`.
- Android unstripped: `bbae5885119f66274388a439a111c8436593e8a7171a88f9f54018556c8364f0`.

The combined changes have twelve paired AO-off captures and twelve paired
Strong-AO captures (`lazy-pipeline-pixels-ao0.json` and
`lazy-pipeline-pixels-ao2.json`). Reviewed geometry, ground, characters and HUD
remain intact. Independent startup timings shift absolute animation ticks:
609-3,407 pixels differ per AO-off pair and 598-4,636 per AO-on pair, under
0.23% of each 1920x1080 frame. This is visual smoke evidence, not pixel identity.
Capture-run timing is excluded from the performance result.

The production Release MSAA/Strong-AO smoke `lazy-pipeline-release-msaa`
completes 12,500 ticks with 23,030 matching AO encode/composite callbacks and
four reviewed captures. Existing sound-entry warnings remain; there is no
runtime renderer fatal. Normal tick-budget teardown reports an aborted pending
buffer mapping and device destruction. This does not certify a full board cycle.

The 300-second production memory run `lazy-pipeline-release-leakgate` reports
PASS: RSS 597 to 593 MB after warmup, slope -526.0 KB/min, handles 926 to 911;
private commit also decreases. Its 4.3 MB working-set swing limits sensitivity
to about 1,105 KB/min, so this short run cannot independently certify the
500 KB/min threshold or exclude small leaks. No growing trend is observed in
the tested stationary-board scene.

These additional changes are local native builds, **not included in the
already uploaded 0.4.16 APK**. No Android hardware timing or 200-300 FPS result
is established. Source/archive checkpoints are retained under
`build/checkpoints/gx-pass-bindings-20260908/` and
`build/checkpoints/lazy-pipeline-factory-20260908/`.

## 2026-09-08: compact draw uniforms after private 0.4.15

Port-local Aurora patch `0032-compact-draw-matrix-uniforms.patch` reduces the
per-draw transform uploads on both Android and Windows. Fixed-matrix draws now
upload the active position/normal pair and only the texture matrices used by
their shader, instead of all 30 matrices. Draws with per-vertex PN or texture
matrix indices retain the original full palette. Post-texture matrices are
packed by usage, with the shader indices remapped to the same source matrices.
The shader's arithmetic, matrix values, graphics resolution, AA, AO samples,
geometry, particles, lighting and game logic are unchanged. Pipeline config
version 14 invalidates old uniform-layout configurations safely.

### Measured work reduction and performance limits

The isolated Release benchmark compares the current game code with the audited
0.4.15 renderer archives against the same game code with compact uniforms.
Conditions remain 1920x1080, FXAA, AO off, free-running simulation, no frame
interpolation, no statistics panels, warmed pipelines, board ages 1800–5799.
Six alternating runs produce these median values:

| Measurement | Previous renderer | Compact uniforms |
| --- | ---: | ---: |
| Uniform-buffer bytes/frame | 642,928 | 257,080 |
| Vertex bytes/frame | 193,269 | 193,269 |
| Index bytes/frame | 84,363 | 84,363 |
| Storage-buffer bytes/frame | 609,403 | 609,403 |
| Whole-frame interval | 0.847987 ms | 0.821517 ms |
| GX submit bracket | 0.431181 ms | 0.419032 ms |

This removes **60.0% of the measured uniform uploads**, not 60% of total GPU
traffic. The final PC timing comparison is **3.12% less frame time / 3.22% more
throughput**. An earlier six-run batch measured 6.34% less frame time, but run
variance is appreciable; do not present either result as a precise universal
gain. The repeatable byte counts are stronger evidence than the small timing
delta. No Mali speedup or 200–300 FPS result is established without the tablet.

Results: `build/engine-benchmark/compact-matrices-final-ao0.json`; the earlier
batch is `compact-matrices-steady-ao0.json`. Benchmark-only upload counters read
Aurora's completed-frame counters and do not perform GPU readbacks. Reproduce
while the audited local checkpoint is available:

```text
rtk proxy python tests/integration/build_engine_benchmark.py --name baseline --renderer-checkpoint build/checkpoints/compact-uniforms-20260908/windows --upload-stats
rtk proxy python tests/integration/build_engine_benchmark.py --name current --upload-stats
rtk proxy python tests/integration/run_engine_benchmark.py --name new-matrix-comparison --ao 0
```

The checkpoint option is restricted to isolated benchmark outputs. It checks
all original artifact hashes, the saved GX/core archive hashes and the normal
current toolchain/artifact checks; shipping build verification is unchanged.
Each benchmark records its actual renderer archive hashes separately from its
game-source hashes.

A second candidate, a bounded shader-layout cache, was tested and **removed**:
the combined median frame interval increased from 0.865322 to 0.913305 ms.
`matrix-layout-cache-steady-ao0.json` retains that rejected experiment's results.
That separate cache was not kept. The subsequently reused patch number `0033`
is an unrelated render-pass binding optimization. The later `0035` change above
instead reuses the existing pipeline cache entry, avoiding the rejected design's
second hash and lookup.

### Correctness and native builds

80 CPU/native tests pass. `tests/test_compact_uniforms.py` compiles the actual
matrix-selection, upload and shader-slot calculations into a native oracle
test. It checks 200,000 configurations, all ten active PN slots, position and
texture palette references, indexed fallbacks, sparse post-matrix sets and
unchanged matrix contents. Windows and Android copies of all four changed
renderer files must agree.

Twelve corresponding AO-off scene captures are pixel-identical before/after.
Twelve Strong-AO pairs have 779–3,848 differing pixels (under 0.19%), with
different absolute startup/animation timing; these are visually reviewed smoke
evidence, not an AO pixel-identity claim. Geometry, characters, ground, HUD and
lighting look intact. Comparisons: `compact-matrices-pixels-ao0.json` and
`compact-matrices-pixels-ao2.json`. Capture-run timings are excluded.

Both production native Release builds succeed. Android's native manifest
verifies all 178 game/platform translation units at `-O2`; the optimized renderer
archives and their source/patch fingerprints are verified too.

- Windows executable SHA-256:
  `f08ab0125d41e98c1680b33c34134d789d93de5e9aad44ee42831bf1857e92c6`.
- Android unstripped library SHA-256:
  `a2b72d2f7b8df7335b5e03f141ac0b8997300cc9b5d917ea24a187c1a09ec1e7`.
- Android stripped/staged library SHA-256:
  `c5c29d8a80ecec4f4c2fa8c5c9ed6809375c7e24f52f5ed41ebd055201381bbd`.

Previous production binaries, renderer archives and audited source are retained
under `build/checkpoints/compact-uniforms-20260908/`. The compact-matrix change
was subsequently published as private 0.4.16:
[verified release](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-compact-uniforms-0416-20260908).
The signed Release APK was downloaded and verified byte-for-byte before
publication. APK SHA-256:
`31bbc09159e4629702859a56b26960a0cc2fe39250bfb2eecf1013f054929f9c`.
The frozen 509-file source snapshot is
`build/checkpoints/compact-uniforms-0416-release-source/source/`.

The uninstrumented Release executable completed the standing 300-second,
Strong-AO memory gate (`compact-matrices-release-leakgate`). The gate reports
PASS: RSS 605 to 602 MB, slope -460.5 KB/min, handles 929 to 916; private commit
also decreased. Its 3.4 MB working-set swing limits resolution to 877 KB/min,
so this run cannot independently certify the smaller 500 KB/min threshold or
exclude small leaks. The test stayed in an isolated stationary board scene;
it is not a full-board or tablet gameplay certification.

The final production MSAA/Strong-AO smoke (`compact-matrices-release-msaa`)
completed 12,500 ticks with 23,150 matching AO encode/composite callbacks and
four reviewed 1920x1080 captures. No renderer runtime failure occurred. Existing
audio entry warnings remain; shutdown reports an aborted pending mapping and
device destruction after the normal tick-budget exit.

## 2026-09-08: engine hot paths after private 0.4.14

The user reported no useful FPS improvement from 0.4.14, including with AO off.
This pass changes shared engine CPU work, not graphics quality:

- `src/gx/gxarray_registry.c` indexes the existing bounded mesh-buffer lifetime
  table with hash chains. Each `GXSetArray` lookup no longer scans every live
  buffer. Allocation bounds, full-table fallback, direct/bulk frees, address
  reuse and restorable state retain their existing semantics. The index adds
  64 KiB of buckets; the dense table remains fixed-capacity and allocation-free.
- `src/gx/aurora_bridge.c` queries physical window dimensions once per event
  pump instead of repeatedly for models, cameras and HUD elements. Event
  dispatch, window replacement, aspect policy and live widescreen changes
  invalidate the cache. Failed or zero-sized surfaces are retried. The logical
  width calculation and its immediate camera/HUD publication are unchanged.

### Controlled AO-off Release comparison

`tests/integration/build_engine_benchmark.py` builds normal production game
units, with only a fixed initial RNG seed and phase accumulation added to the
isolated benchmark executable. There are **no board/capsule QA fixtures** in
this build. The baseline overrides only the two changed production files from
the frozen 0.4.14 source under
`build/checkpoints/water-present-0414-release-source/source/`; all other units
and renderer dependencies are shared with the current variant.

The runner stops pressing buttons when `w01.live` arrives and measures the same
stationary introduction scene, board ages 1800 through 5799 (4,000 frames per
run). This avoids different startup durations placing absolute VI-tick windows
at different camera phases. Settings: 1920x1080, FXAA, AO off, warmed pipelines,
VSync off, free-running simulation, no interpolated presentations, no on-screen
statistics panels and no frame readbacks during timing. The widescreen runner
now publishes `MP6_ENH_WIDESCREEN=1`, matching the interactive preference cache,
in addition to the legacy environment switch.

Six alternating runs (`baseline,current,current,baseline,baseline,current`)
on the Windows/RTX 4090 PC give these medians of per-run means:

| Frame phase | 0.4.14 source | Current source |
| --- | ---: | ---: |
| Game-side work | 0.504271 ms | 0.381822 ms |
| GX submit | 0.424071 ms | 0.423781 ms |
| Complete frame interval | 0.948312 ms | 0.851860 ms |

This is **10.17% less whole-frame time**, or about **11.3% greater throughput**
in this CPU-heavy desktop case; the game-side phase falls 24.3%. Phase timings
include any waits inside their brackets and are not GPU timestamps. Independent
per-function probes locate the main saving in mesh-buffer lookup: roughly
224 ns to 29 ns per call, including probe overhead. That lookup ratio is **not**
a game-wide FPS ratio. The complete-frame result, not the tiny-function probe,
is the performance evidence.

Repeat from the port root after a normal Windows Release build:

```text
rtk proxy python tests/integration/build_engine_benchmark.py --name baseline --reference-source build/checkpoints/water-present-0414-release-source/source
rtk proxy python tests/integration/build_engine_benchmark.py --name current
rtk proxy python tests/integration/run_engine_benchmark.py --name new-hotpaths-run
```

Results are in `build/engine-benchmark/hotpaths-steady-ao-off.json`; individual
logs, isolated settings and executable hashes are retained under
`build/board-qa-runs/hotpaths-steady-ao-off-*`. Earlier `hotpaths-ao-off-*` runs
used absolute VI windows and are excluded from the reported result. All fixed
seeds and added timing probes stay out of the production Release executables.

### Verification and limits

79 CPU/native tests pass. The new native registry test fills all 8,192 entries,
checks collisions and dense-table moves against an oracle, performs 20,000
random lifetime operations, exercises bulk frees, and restores a copied index
and table together. The window test makes 10,000 reads with one successful SDL
query, then checks resize/orientation, replacement, unavailable surfaces and
live-setting invalidation boundaries. This is a synthetic restore test, not a
cross-build quick-save compatibility claim.

Separate paired runs capture 12 corresponding 1920x1080 frames with AO off and
12 with Strong AO. Camera, geometry and HUD output look correct on review.
The live scene is not bit-identical between processes: 595-5,890 pixels differ
per AO-off pair and 1,135-4,219 with AO on (under 0.3% of a frame). These captures
are visual smoke evidence, **not proof of pixel identity**. Timing from capture
runs is excluded because readbacks perturb frame time. Retained comparisons:
`hotpaths-steady-ao0-pixels-verified.json` and `hotpaths-steady-ao2-pixels.json`.

Both native production Release builds succeed. Windows executable SHA-256:
`54361d0c43358ca307a6c3854d84479dc229311e5ba21a92b8163c480250fd25`.
Android native library SHA-256:
`abc5b2d8457195b52d031156f20a92c036b11cefa596be5c65f76772246cc9cd`.
The Android manifest verifies all 178 translation units use the Release `-O2`
profile. Previous native artifacts are preserved in
`build/checkpoints/engine-hotpaths-20260908/`. No APK is packaged or uploaded
in this pass; private 0.4.14 remains the uploaded version.

The uninstrumented production Windows executable also completes the isolated
`engine-hotpaths-release-smoke` run: 12,500 ticks with FXAA and Strong AO,
23,090 matching AO encode/composite callbacks and normal budget-triggered exit.
Teardown reports an aborted pending buffer mapping and destroyed device; there
is no runtime fatal error. This stationary-board smoke is not a full board-cycle
or Android gameplay certification.

The standing 300-second memory gate uses the same production executable with
Strong AO and isolated saves. `tests/integration/run_engine_leakgate.py` keeps
the free-running board scene alive past warmup and records all gate output.
`engine-hotpaths-leakgate/gate.log` reports:

```text
[leakgate] PASS: steady-state slope -448.0 KB/min over 25 samples (threshold 500 KB/min); rss 594 -> 591 MB; handles 926 -> 913
```

The gate also warns that the 3.6 MB working-set swing makes this four-minute
fit insensitive below 913 KB/min; its 500 KB/min threshold cannot be certified
at that resolution. Private commit also decreases (1545 to 1541 MB). This is
evidence against sustained growth during the tested scene, not a claim that
every small leak is excluded. The runner accepts `--duration`/`--warmup` for a
longer follow-up without changing the threshold.

The tablet is not attached. These CPU savings do not establish a Mali FPS
gain, much less 200-300 FPS. The user's earlier AO-off screenshot still had a
6.342 ms GPU span with statistics panels visible; 200 FPS requires a 5 ms total
frame interval. Scene rendering and UI/GPU costs still need device measurements.
No AO samples, resolution, effects, game rules or animation work were removed.

**Correction to the older QA timing evidence below:** the earlier isolated QA
executable also included numerous diagnostic checks in hot paths. The reported
14-to-8 ms game-phase change describes that QA executable, not production
Release frame cost. New production-unit benchmark runs exclude those checks;
do not use the older 44% figure to predict retail or tablet FPS.

## 2026-09-08: fullscreen bandwidth and continuous water coverage

After the uploaded 0.4.13, the user supplied Tab S10 Ultra timings at
2960x1848: approximately 81 FPS / 11.733 ms GPU span with AO and 128 FPS /
6.342 ms without it. These are different captured timing windows with large
statistics panels open, not a controlled benchmark. 200 FPS would require
about 5 ms per frame; the current work does not establish that target.

Port-owned Aurora patch `0031-fused-present-resample.patch` renders the existing
resample/FXAA shader directly into the presentation surface when its format and
fullscreen viewport match exactly. Previously it first stored an intermediate
full-size image and then copied that image to the surface. The new path removes
that intermediate write/read, about 41.7 MiB of nominal traffic per 2960x1848
RGBA8 frame (actual external memory traffic depends on the GPU). Offset,
letterboxed, fractional or different-format views retain the old path. Rml
replacement menus, premultiplied overlays and touch draw ordering are unchanged.
This is shared Android/Windows code, with no render-scale, AA or AO-quality cut.
The GPU profiler includes resampling in `Present + touch UI` on the fused path;
there is no separate `Present resample` pass there.

The water receiver replay now writes constant coverage over the authored
Towering Treetop water meshes. The old replay used animated ripple alpha as
coverage, leaving AO between ripples. Only the private AO mask uses the simpler
texture-free material; normal water rendering is unchanged. Final scene depth
still rejects water behind land and opaque actors. See
[AMBIENT_OCCLUSION.md](AMBIENT_OCCLUSION.md) for diagnostic evidence and scope.

`test_present_fusion_gpu.py` runs the actual renderer WGSL before/after fusion:
all tested pixels match, including RGBA/BGRA, FXAA, area filtering, up/downscale,
odd dimensions, two premultiplied UI layers and the tablet resolution. The
native eligibility test rejects non-fullscreen and mismatched formats. An
alternating, resident RTX 4090 diagnostic at 2960x1848 measured 0.0317 to
0.0154 ms without FXAA and 0.1946–0.3052 to 0.1372–0.1956 ms with FXAA for these
presentation passes only. Four paired FXAA runs reduce that pass sum by
23.1–38.1%; this is **not Android hardware**, game-wide speedup or 200 FPS proof.
Repeat with `python tests/integration/test_present_fusion_gpu.py --benchmark`.

77 CPU/native tests and 29 GPU tests pass. The production Release smoke run
`present-release-fullscreen` completes 9,300 ticks with FXAA and Strong AO,
13,670 matching encode/composite callbacks and reviewed board-introduction
captures. A second 9,300-tick MSAA run also exits normally (13,970 callbacks).
These runs contain existing audio diagnostic warnings; no renderer failure was
reported. Native framebuffer dumps precede presentation, so final-surface pixel
equivalence is established by the separate actual-WGSL GPU tests, not by those
dumps. The eligibility test covers the letterbox/fractional fallback gate.
`water-present-live-frames` also completes 6,900 ticks with MSAA, Unlocked FPS
enabled and six live AO changes (1,452 matching encode/composite callbacks).
Its twelve reviewed captures cover Subtle, Strong and Off. All captured rows
are real ticks: readback perturbs interpolation timing, so they do not prove
pixel equivalence on replay frames. Earlier capture attempts caught the startup
sequence or missed the event trigger and are not used as visual AO evidence.

Both native Release builds include these changes. Windows executable SHA-256:
`0d9134ac7658f25d4142c2da7a543309fdbc3f3d433089048149d44c912dc315`.
Android native library SHA-256:
`a0d456094f9ead32e417a24bca0f07a60f3f093acfaa4206a7e0e6a8be74cd3a`.
No new APK has been packaged
or uploaded in this pass. The tablet is not attached; cold/warm startup,
sustained performance and visual checks on Mali remain outstanding. Mali's
conservative AO kernel and depth snapshot schedule have deliberately not been
changed or re-enabled based only on desktop tests.

## 2026-09-08: first shared optimization pass (shipped in private 0.4.13)

Two changes are enabled on Android and Windows:

1. Board AO reuses full-precision linear depth prepared in the existing exact
   decal-removal pass. No added pass, reduced sample count, lower resolution or
   relaxed foreground/water rejection. Non-board cameras keep the original
   path. Mali's conservative kernel and depth-snapshot scheduling remain.
2. Enhancement lookups use a resolved host-side cache after startup argument
   parsing. Live settings publications refresh it immediately, retaining
   per-switch/preset/config precedence and leaving saved configuration alone.
   Tests perform 70,000 resolved reads with **zero** environment lookups.

The CPU change is relevant beyond AO: widescreen, interpolation, shadow
quality, AA and other enhancement consumers use the same accessors. It does
not change game rules, timing, simulation rate, geometry or audio behavior.

### Desktop evidence, not an Android FPS prediction

The isolated Release QA executable restored the same board state at tick 7500
and ran the original scripted input. Test settings: 1920x1080, FXAA, Strong AO,
Android's 960-pixel AO shading budget, VSync off, free-running simulation and
no interpolated presentations. Graphics use the PC's RTX 4090, not a simulated
Mali. The independent synthetic GPU benchmark excludes game rendering, UI,
depth snapshots and presentation; see [AMBIENT_OCCLUSION.md](AMBIENT_OCCLUSION.md).

`ao-opt-matched-before2` uses the old raw-depth conversion and uncached settings;
`ao-opt-matched-after` uses the new paths. Ten corresponding full-resolution
captures at ticks 7511 through 8321 have **zero changed pixels**. The stable
five-second timing windows after restore report approximately 14 ms game-thread
work before and 8 ms after. Do not count the timing window crossing restoration:
the logical tick jumps from 5000 to 7500, invalidating its throughput figure.
The test intentionally runs gameplay faster than real time; those throughput
numbers must not be presented as normal-speed Android FPS.

`ao-opt-repeat-after` and `ao-opt-repeat-before` repeat in reversed order without
image readbacks. The corresponding opening windows report about 13.9 ms before
and 7.8 ms after (roughly 44% less game-thread time); both runs exit normally.
Logs, executable hashes, settings and captures are retained in
`build/board-qa-runs/`. The starting AO source and production binaries are
preserved in `build/checkpoints/ao-performance-20260908-baseline/`.

Both native Release builds include these changes. Windows executable SHA-256:
`ffbc652297f5c6bcf24b4a6977cc98eb8ddb0e9100c13cf0643a5a01fa4ca6db`.
Android unstripped native library SHA-256:
`4fa1be888b8ae366771fbf494fd80bffb6c7a11891c52391fc57a0f795edc87d`.
These identify the first pass's native artifacts, before the fullscreen/water
follow-up above.

The uploaded 0.4.12 build was reported working on the user's Tab S10 Ultra
with AO enabled. This is useful compatibility evidence for that published
build, not device validation of the current optimization. No Android device is
attached to the build PC; Android performance, sustained thermals and visual
regressions still require a device run. No 2–3x Android gain is claimed.

### September 9: GX sampler lookup memo

Port-local Aurora patch `0036-gx-sampler-memo.patch` keeps the last sampler for
each of the eight GX texture slots. A hit avoids rebuilding its descriptor,
hashing/locking the shared sampler cache, and temporary handle refcount traffic.
The key contains both GX sampler mode words, current global anisotropy, and
replacement/generated/arbitrary-mipmap flags. Texture identity is deliberately
not a key: the sampler does not own image data. Initialize and shutdown clear all
eight entries. Draw order, shaders, filtering values and image resolution are
unchanged.

Six alternating fixed-seed Windows Release runs per batch, 1920x1080 FXAA,
4000 measured board frames (no overlays/readbacks):

| Batch | Baseline frame ms | Current frame ms | Reduction |
| --- | ---: | ---: | ---: |
| `sampler-memo-ao0` | 0.714888 | 0.715743 | -0.12% (flat) |
| `sampler-memo-repeat-ao0` | 0.746543 | 0.720277 | 3.52% |
| `sampler-memo-ao2` | 1.099157 | 1.079340 | 1.80% |

Results are in `build/engine-benchmark/`. The variation matters: this is a small
CPU optimization, not evidence for 200+ Android FPS. No Android device was
available. Both port-local renderer archives and native Release binaries were
rebuilt, with all 178 Android game/platform TUs verified at `-O2`.

83 engine tests and 55 setup/release tests passed. The production-body sampler
test checks mode bits, runtime anisotropy, mip flags, slot independence, texture
identity reuse and reset. Two 12-frame capture comparisons had different absolute
startup ticks and are not bit-identical proofs; the inspected contact sheet
showed no obvious rendering regression. The production Strong AO/MSAA smoke run
`sampler-memo-release-msaa` exited normally at 12500 ticks and its four captures
were inspected. This is not an end-to-end board completion test.

Windows native SHA-256:
`af5da5acd9629f647ce29801f44b60d654d2edcb68a0c5f3c6934a9b1f78c05c`.
Android unstripped native SHA-256:
`650586c88303cea8bb8c94a7e0c19d6a9b2f817664950b455b3d5801dc829405`.
These changes have not been packaged/uploaded as a new APK; the last private
upload remains 0.4.16.

### What a PC can and cannot test

Android Emulator is useful for Android UI, lifecycle and compatibility checks.
It renders with the host GPU or software, so changing emulator resolution or
CPU limits cannot faithfully reproduce a Mali/Adreno driver, memory hierarchy
or thermal throttling. See the [Android emulator acceleration documentation](https://developer.android.com/studio/run/emulator-acceleration).

Mali Offline Compiler can estimate shader instruction costs and register
pressure for an Arm target on a PC. Its static report does not know real draw
data or cache misses, and therefore cannot predict whole-game FPS. Arm explains
these [capabilities and limitations](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/accelerating-shader-programs-with-mali-offline-compiler-7).
The compiler was not installed on this PC during this pass; no offline Mali
cycle report is claimed.

### Next device measurements

Use the same board, camera, resolution and save-card setup across versions.
Record `stat unit` and `stat gpu` with AO off, Subtle and Strong, separately for
FXAA and MSAA. First test cold boot, then warm rendering and a sustained session.
Watch scene/EFB time, depth snapshots, foreground replay, GTAO, blur and present,
not just the FPS badge. CPU game work and GPU execution overlap: their costs
must not be added into an invented whole-frame speedup.

Further work should follow those measurements. In particular, the withdrawn
Mali snapshot/array fast paths must not be restored merely because they pass
on the desktop GPU. New quality tradeoffs require explicit evaluation rather
than silently lowering render scale or removing correct coverage.

### Connected Tab S10 Ultra profiling — 2026-09-11

The connected SM-X920 was tested against its installed 0.4.20 APK. Original
APK, settings and memory card were backed up before testing; automation uses
a separate app-private save/config/cache directory and the existing imported
disc through a symlink. No system clock, thermal, game-mode or resolution
override was applied. Source changes remain inside this port.

The native Release profile was authenticated at `-O2` for all 178 Android
game/platform translation units. Diagnostic APKs change only the manifest's
debuggable flag and the explicitly selected native library; the remaining
entries are byte-checked against the installed APK. These wrappers are not
private-release uploads. The final local APK restores the original manifest.

Measured work addressed:

- `simpleperf` attributed substantial game-thread time to small command copies
  in `fi_build_replay`. Adjacent untouched commands now copy as contiguous spans,
  flushing before every matrix rewrite, filtered command and end of stream.
  400 native comparisons preserve every output byte, AO marker and rewrite
  count. A single-process alternating tablet benchmark (40 trials, 200 replays
  per implementation/trial) measured **22.038 → 17.340 microseconds**, a **21.3%**
  reduction on its synthetic GX streams. This is not whole-game FPS.
- A declined replay could consume time before the throttle slept using its
  old remainder. The throttle now re-reads the absolute deadline first. No
  simulation rate or replay interpolation policy changes.
- The Mali compatibility blur now scans scalar visibility values and returns
  the original center when all seven taps are exactly fully lit. The original
  bilateral kernel still handles every nonzero signal. Unlike the withdrawn
  optimization, this introduces no dynamically indexed seven-vector array.
  Consecutive hot board runs `pair-c1-ao` / `pair-d1-ao` measured blur X+Y at
  **1.402 → 0.601 ms**. Their whole-game rates were **66.779 / 69.597 FPS**;
  clock/thermal variation prevents treating this pair as a precise global
  speedup. Strong AO, FXAA, render dimensions and all sample counts are equal.

Experiments not shipped:

- Native integer-margin presentation fusion removed a pass and passed pixel
  tests, but repeated tablet runs regressed overall performance. Both generated
  renderer trees were restored and rebuilt; the proposed patch is retained
  only with the local experimental evidence under `build/checkpoints/`.
- A 0.2 ms Android spin tail reduced idle CPU work, but did not demonstrate a
  reliable whole-game FPS benefit. The original tail is retained.

Evidence is under `build/tablet-performance-20260910/`, with each run's exact
arguments, PID, thermal snapshots, timing log and JSON summary. GPU rows sample
one completed frame per 120 presents; they are not an exhaustive frame-time
histogram. FPS rows are five-second windows after the board has settled for
600 ticks. `benchmark_android_replay.py` is a CPU microbenchmark, not a game
scene benchmark. No 200+ FPS claim follows from these component measurements.

Validation: 105 engine/unit tests; 18 AO quality tests, four exact-fast-path
tests and five prepared-depth tests. The frozen 0.4.20 compatibility shader is
the pixel-test reference, with its normalized source hash verified against the
local release checkpoint. Native-resolution color/decal/foliage/water coverage,
AO working resolution, samples, R32 depth and 60 Hz simulation are unchanged.
Device gameplay validation here is the boot-to-Towering-Treetop route, not an
entire match or all capsule branches.

Handoff state: the tablet locked before the final combined candidate could
complete its gameplay run, then disconnected before the non-debuggable APK
could be installed. The last installed APK is the local `candidate5` profiling
wrapper. The final non-debuggable APK is
`build/tablet-performance-20260910/apk-accepted-release/mp6-tablet-optimized-release.apk`
(SHA-256 `6a0d5982b630734e9dc590e6da61457efc3ca71d0b70fe4d19f1376e8b002c56`).
It has not been uploaded or installed. Player settings and save inventory were
last verified byte-identical before the disconnect. Reconnect/unlock, finish
the controlled comparison, restore this non-debuggable APK and retire the
isolated profiling files. Do not claim the remaining device verification done.
