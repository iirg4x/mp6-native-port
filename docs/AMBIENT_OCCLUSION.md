# Ambient occlusion (port-owned)

Optional port-owned GTAO; no decomp gameplay or shared Aurora checkout changes.
The Video setting is Off/Subtle/Strong, live on Windows and Android. All preset
defaults are Off, so older configs and ordinary automation retain original lighting.

The companion placement enhancement is now **static scenery only**. The removed
actor, guide and board-space offsets could detach event effects and bury moving
characters. All of those dynamic transforms now stay identical with AO Off,
Subtle and Strong; no gameplay/decomp workaround or weaker AO shader is involved.
Measured log/tree-base and vegetation repairs remain. The W01 lawn and starting
path are raised to the authored start-space floor, closing their gaps without
lowering actors, effects or space artwork. Existing static contacts recalibrate
to that raised surface; the white connector layer is kept above it and below
space icons. AO Off retains the original geometry. See
[GROUNDING.md](GROUNDING.md) for the draw-path regression and matched native
geometry evidence. Authored asset gaps are not universally repaired by AO.

## Rendering

### Continuous water receiver protection (2026-09-08 follow-up)

Towering Treetop's day/night `mizu3` model is excluded as an AO receiver. Its
`grid98`, `suimen2` and `test` meshes form continuous water surfaces; their
animated texture alpha describes ripples, not holes in the water. The former
mask replay copied that alpha, which left a ripple-shaped pattern of unprotected
pixels even with private-pass depth rejection disabled. Model identification
was working; receiver coverage was wrong.

The water-only private draw now uses one texture-free TEV stage with constant
one alpha, RGB writes disabled, and no ripple alpha test. It retains the mesh,
camera, viewport, scissor, culling and final-scene depth rejection, with depth
writes disabled. Overlapping water layers cannot clear previous coverage.
Ordinary foliage still uses its original material/filtered alpha and preserves
its premultiplied color. No water/game geometry, material, texture or actual
EFB depth is changed. This identified-asset rule is not a blanket classifier for
every blue texture or all water assets on other boards.

The isolated 2960x1848 Strong AO/FXAA diagnostics `water-0413-mask-world` and
`water-mask-always-fresh` show ripple-shaped holes in the old mask with depth
testing respectively enabled/disabled. `water-continuous-mask` shows continuous
coverage after the change; `water-continuous-normal` reviews twenty normal game
captures through movement, dice, space and foreground effects. Fresh runs have
different RNG/camera poses and are not claimed as pixel-matched A/B evidence.
An attempted same-state comparison was refused by the existing coroutine-address
guard; no guard was bypassed. These are Windows renderer tests, not Mali captures.

The GPU suite also seeds final depth, draws the continuous water mask with both
depth conventions, and proves opaque foreground remains unmasked. Composition
leaves covered water RGB and scene alpha unchanged while still shading the
unmasked receiver. The new shared presentation bandwidth optimization and
the limits of the 200 FPS target are documented in
[ANDROID_PERFORMANCE.md](ANDROID_PERFORMANCE.md).

### Scene and AO passes

- `src/hsf/mp6_ambient_occlusion.c` captures the real camera's perspective,
  viewport, scissor and depth range at the end of each Hu3DExec camera, before
  front sprites. Board camera 1/2's original depth clears separate the world,
  dice/items and foreground effects; AO respects that existing ordering.
- `src/gx/ambient_occlusion.cpp` uses Aurora's public depth snapshot, encoder
  task and custom draw APIs. No renderer dependency patch is required.
- The WGSL shader reconstructs view-space positions/normals and analytically
  integrates the visible horizon of three hemisphere slices (four samples on
  each side, 24 depth taps). Desktop shading runs at native resolution through
  1920 pixels on the longest axis; Android uses a 960-pixel shading cap. Higher
  resolutions/SSAA use the platform working cap with
  full-resolution depth-aware reconstruction. Horizontal/vertical seven-tap
  surface-aware filtering removes sampling steps. Native-resolution composition
  directly reads the matching pixel when the sizes match, without redundant bilinear softening.
  RGB composition shades the background while preserving captured foreground
  foliage color; it leaves alpha, depth and stencil untouched.
  Clear-depth pixels receive no shading.
- Board-space artwork is treated as a decal for AO only. The port's MasuDraw
  bracket captures depth immediately before and after the original space draw.
  At camera end, a full-resolution R32 preparation pass substitutes the earlier
  depth **only** where the space changed depth and the final visible depth still
  equals the post-space depth. The real alpha/depth tests therefore determine
  the coverage, including low-alpha filtered borders; later foreground geometry
  is retained. This does not replace textures, change alpha thresholds, disable
  the game's depth writes, or modify its actual depth buffer.
- World-space radius does not grow with SSAA or window resolution. Subtle and
  Strong change intensity, not sample count. Off issues no AO GPU work.
- Depth stays R32 throughout snapshot/decal cleanup. The two ping-pong RGBA16 working
  textures carry visibility and normal, **not half-precision linear depth**.
  Positions are reconstructed at the exact sampled depth pixel centers.
  Normals use reciprocal-depth extrapolation (a wider footprint when reduced) to avoid
  selecting silhouette discontinuities. Filtering measures distance from the
  tangent plane, so sloping ground is not split into absolute-Z contour bands.
- No checkerboard or temporal sample rotation: there is no TAA history to
  remove that noise. A 32-unit radius and smooth 2-to-6-unit contact tolerance
  suppress tiny terrain joins; the separate depth preparation removes space
  decals regardless of their authored height. Finite-slice integration is
  normalized against the same unoccluded hemisphere, avoiding false darkening
  of perfectly flat sloping ground. The artistic response multiplier is 1.7.
  Maximum darkening is bounded at 20% (Subtle) / 35% (Strong) per camera pass.
  This deliberately favors the original light artwork over physical realism;
  it does not alter object placement, meshes, or original shadows.
- Slice samples are concentrated near the receiving pixel, rather than mostly
  near the radius boundary, so small feet/props retain visible contact shading.
  Intensity is normalized independently of the maximum-darkening safety cap.
- Horizon integration is adapted from [GTAO](https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion)
  and [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO), with Intel's MIT notice
  embedded in the shader and shipped in `res/licenses/XeGTAO.txt`. This is not
  the entire XeGTAO implementation: MP6 keeps its R32 reconstruction, fixed
  sampling, dual-tangent-plane denoiser and bounded artistic response.
  Screen-space limitations remain: invisible geometry contributes nothing;
  translucent objects still have no separate depth layer. The decal bracket
  covers board spaces; the foreground protection below covers ordinary
  depth-tested alpha-blended FaceDraw batches, not custom hooks or special blends.

### Foreground foliage protection

Scenery and temporary event props can use ordinary alpha blending without depth
writes, leaving the background surface as the AO receiver.
`include/mp6_ao_foliage_draw.h` records
eligible original FaceDraw batches and replays their mesh/material submission
into a private, full-resolution foreground-color target at camera end. No model,
event or material hooks are rerun. Additive/inverse blends, reflections, Z-off
draws, hooks and depth-writing cutouts are excluded.

The private pass starts black and copies the final scene depth. Original texture
color/alpha, lighting, shadow/projector transforms, filtering, animation, alpha
test and culling are retained. Ordinary alpha blending accumulates the foliage's
premultiplied RGB contribution `F` in its original submission order. The final
depth test rejects foliage hidden behind later opaque geometry. Composition uses
`scene * V + F * (1-V)`, with ONE / SRC_ALPHA blending (`V` is AO visibility).
Since `scene = background * transmission + F`, only the background contribution
is shaded. This also covers overlapping leaves and filtered, partly transparent
edges. The old `scene * mix(1,V,transmission)` approximation could tint green
edges with the background's AO and over-brighten other channels; an alpha-only
coverage test did not catch that color error. The game's actual depth target
and original material draws are not rewritten.

The camera viewport/scissor must be mapped from logical EFB units into the
private target's actual pixels. Aurora's offscreen passes do not apply the EFB's
resolution scale. `mp6_ao_foliage_camera` performs that mapping after the camera
setup, using `GXSetViewportRender` / `GXSetScissorRender`; these commands are
retained by the interpolation stream. The ordinary GX scissor cannot carry HD
pixel coordinates (its 11-bit fields include a 342-pixel hardware bias).
Scissor edges use the same outward rounding and clamping as the EFB renderer.
The original camera setup is restored after closing the private pass.

This is receiver protection, not physically separate AO for every transparent
layer. The mask is single-sample at full scene resolution, including when MSAA
is enabled; it is not exact per-sample transparency. Eligibility follows the
original FaceDraw depth/blend branch via `mp6_ao_foreground_policy.h`, independent
of board, model name or a scenery registration list. Up to 2,048 batches are
retained per tick and consumed per camera, only while AO is enabled.
Grass painted into opaque log
textures remains part of the log surface; measured log-base gaps are handled
separately in [GROUNDING.md](GROUNDING.md).

## Lifetime and retained frames

### Android shading budget (2026-09-07)

`mp6_ao_resolution.h` caps only GTAO and the two denoise buffers to 960 pixels
on Android's longest framebuffer axis, preserving aspect ratio with integer
ceiling and leaving smaller targets native. A 1920x1080 framebuffer therefore
uses 960x540 for these three passes (75% fewer shading pixels). This is not a
75% whole-frame cost claim: scene depth, exact decal cleanup, foliage color/depth
protection and final composition all remain full resolution. The same 24 horizon
taps, world radius, intensity and two-surface rejection remain in use.

Reduced-resolution composition first reads its four shading samples. If every
sample is exactly unshaded, it returns the identity blend before reconstructing
the receiver normal, loading neighbor depths or sampling foliage. No epsilon or
darkening threshold is introduced. Otherwise it performs the original bilateral
reconstruction. Same-budget synthetic output is bit-identical to the earlier
shader, including the foreground/decal regressions.

The 16-test GPU suite adds 1080p and odd-sized ultrawide mobile-budget fixtures
with visible shoe contacts and exact low-resolution decal coverage. The 57-test
CPU suite compiles both desktop and Android resolution policies. Matched native
captures under `terrain-slide-mobile` exercise that budget on the real desktop
renderer. `benchmark_mobile_ao.py` measures five resident GPU passes, excluding
copies, foreground replay, game work and presentation. The initial RTX 4090
sample reduced their 1080p total from 0.4263 ms to 0.1905 ms, but 720p results
were inconsistent/slower and same-pass timings varied with GPU clock state.
These are diagnostic desktop samples, not Android-device speedup or FPS claims.
No Android device was attached. Android Release native compilation succeeds;
the published 0.4.7 APK does not yet include this optimization.

Aurora can consume streamed passes before the game has finished recording the
frame. Callback completion is **not** command-buffer submission. AO slots must
be both consumed and from a different frame before reuse; otherwise a later
camera's Queue.WriteBuffer replaces uniforms used by an earlier camera. The
initial implementation's occasional shading drop was caught in matching
captures and repaired with this per-frame ownership rule.

GPU resources are bounded, host-owned and excluded from savestates. Before normal
renderer teardown, synchronize the worker, unregister callbacks and release GPU
objects. The existing restored-session guarded exit remains unchanged.

Custom GPU passes are not GX FIFO commands. Frame interpolation therefore retains
camera values plus exact drained FIFO boundaries, including both decal snapshots
and private foliage begin/end markers. Auxiliary foliage matrices have a distinct
stable object identity, so their interpolation does not collide with normal draws.
If the private pass cannot open, replay skips its masked GX segment instead of
submitting black mask geometry to the game target.
Snapshots are scoped to the camera and current admitted frame; failed or
mismatched-size brackets are discarded, not reused from another view. Its command walker maps those
boundaries into the filtered replay stream; replay submits each segment and
inserts AO before the same HUD boundary. No game logic or render hooks are rerun.
Both real and interpolated frame admission advance AO resource ownership.

## GTAO and board-space verification (2026-09-07)

The runs below precede the log/foliage revision unless stated otherwise.

- 52 CPU/native tests pass, including original space alpha/depth-state contracts,
  camera identity, headless no-ops, retained decal boundaries and resource lifetime.
- 13 production-shader Vulkan tests pass on RTX 4090. A coarse cutout with holes
  and an 18-unit offset produces exactly unoccluded ground after decal removal,
  at full/half working resolution and forward/reversed depth. A foreground shoe
  over the decal produces pixel-identical AO to the decal-free reference, with
  visible contact shading retained. Previous slope, far-depth, foreground-leak,
  scissor, intensity, odd-size and 4K tests remain passing.
- `gtao-first-board`: normal Windows release, 14,500 ticks, exit 0, 29,390 matching
  AO encode/composite callbacks. This run did not capture frames (trigger missed).
- `gtao-board-state`: QA build, 1920x1080, 10,500 ticks, exit 0; 48 captures through
  board setup/intro/first turn reviewed. The renderer and scene source are normal;
  QA adds observation/live-setting hooks, not production gameplay changes.
- `gtao-matched-*-capture` and `gtao-matched-subtle`: restored the same isolated
  state, captured ticks 7511..7516. Off/On also toggles the existing grounding
  enhancement, so its raw RGB delta is **not** a pure shading measurement.
  Subtle/Strong preserve placement; the first frame has no pixels brightening
  by more than one channel value, and stronger contact shading. Later frames
  have a few differing effect pixels; no exact all-frame equality is claimed.
  No status panels exist in this scene, so the comparison reports HUD coverage
  as zero / delta unavailable rather than claiming a HUD pass.
- `gtao-live-fi-native-aa`: live Subtle -> Strong -> Off -> Subtle at 1920x1080
  with Unlocked FPS, 36 captures including replay frames, exit 0. Two earlier
  MSAA state-load attempts hit the existing arena-address guard and were
  excluded; the guard was not bypassed.
- `gtao-msaa-fresh`: fresh MSAA 4x board, 11,000 ticks, 24 reviewed captures,
  exit 0. Windows graphical/headless and Android release native libraries build.
  Android device rendering/performance and APK packaging/upload are not verified
  by these checks.
- `benchmark_ao_gpu.py` uses GPU timestamps and 256 resident-resource repetitions
  per batch to avoid measuring idle P8 clocks during CPU image generation.
  On this RTX 4090, median pass sums were about 0.19ms at 1280x720 and 0.38ms
  at 1920x1080. These are synthetic AO-only costs, **excluding** depth snapshots,
  game rendering, driver submission and presentation. They also predate and
  exclude the new foliage coverage pass and its extra snapshots; they are not
  a performance measurement of that revision. Sparse readback-separated
  timings are not representative and were discarded. Whole-game board samples
  with Strong AO at 1080p ranged roughly 263-285 presents/s; there is no 1000-FPS
  or mobile performance guarantee.

Run shader quality and optional timestamp benchmarks separately:

```powershell
python tests/integration/test_ao_gpu.py
python tests/integration/benchmark_ao_gpu.py
```

### Log/foliage revision verification (2026-09-07)

The later tree/slide-base extension is documented and measured separately in
[GROUNDING.md](GROUNDING.md#main-tree-and-slide-base-2026-09-07). It changes only
52 supported bottom vertices on four explicitly registered main-tree meshes;
all logical positions and playable slide/platform heights remain unchanged.
No AO intensity, filtering or depth-policy change accompanies that extension.

- 52 CPU/native tests and 15 production-WGSL Vulkan tests pass. The two new GPU
  tests cover exact foreground alpha coverage/holes at native and half AO working
  resolution, plus the production depth-seed shader rejecting background foliage
  behind opaque geometry with forward and reversed depth.
- `gtao-foliage-before` / `gtao-foliage-state` restore the same isolated state with
  only the QA mask-disable switch changed. All 141 captured mesh geometries and
  logical positions are bit-identical. In the first matched frame the largest
  brightening is 27 RGB steps and darkening at most one rounding step; the visible
  correction is localized to foliage, not a whole-map lighting change. Later
  frames have a few differing effect pixels, so no all-frame equality is claimed.
- `gtao-foliage-fi`: live Subtle -> Strong -> Off -> Subtle, 48 real/interpolated
  captures, clean guarded exit. The reviewed dialogue page-fold animation is
  original game behavior, not a black-screen fault.
- `gtao-log-foliage-msaa`: latest normal Windows release, fresh MSAA 4x board,
  11,000 ticks, 24 captures through the intro/dice sequence, clean exit; 18,890
  matching AO encode/composite callbacks and 3,950 foliage passes. No renderer
  validation errors were reported. Existing sound-entry errors appear in the log;
  this test does not establish audio correctness or a complete board-cycle pass.
- Windows graphical/headless and Android release native libraries build. No
  Android hardware visual test, APK packaging or upload is implied. Save/config
  data for these runs is isolated under ignored `build/board-qa-runs`.

### Foreground-color compositing follow-up (2026-09-07)

- 55 CPU/native tests and 15 production-shader Vulkan tests pass. The updated
  foliage regression uses colored wood, a full alpha ramp, texture holes and
  two overlapping colored leaves, at native and half AO resolution. It executes
  the native ONE / SRC_ALPHA blend and checks untouched scene alpha. The old
  transmission-only formula differs from the correct RGB by over 0.05; the new
  output matches within 0.001 (half-float target precision).
- `gtao-grass-color-state` creates an isolated state. `gtao-grass-color-legacy`,
  `gtao-grass-color-unprotected` and `gtao-grass-color-matched` restore that same
  state using one QA executable. The legacy switch reproduces only the old
  black-on-white foliage pass, old WGSL composition and old blend factors; it is
  not present in production. All 141 captured meshes and logical positions match.
  At tick 7511 the corrected/legacy difference over 2 RGB steps is localized to
  the leaf in front of the stump: 71 pixels at [807,232,820,243], with channel
  changes of +12/-17. Compared with no foliage protection, the correction only
  brightens (maximum 42, at most one rounding step of darkening). Fresh-save
  versus restored captures had a few different actor-edge pixels and were not
  used as the matched pixel comparison.
- `gtao-grass-color-fi` verifies fresh MSAA 4x and live AO cycling, 48 reviewed
  captures, exit 0, 1,677 matching encode/composite callbacks. Despite its run
  name it used free-running logic and captured no interpolated frames.
  `gtao-grass-color-replay` separately runs 60 Hz logic with Unlocked FPS, cycles
  AO live, captures 43 real plus 5 interpolated frames, and exits via the existing
  restored-state shutdown guard. No renderer validation failure was reported.
- Windows graphical/headless and Android release native builds complete.
  Android device rendering, APK assembly/upload and a full board match are not
  implied. AO strength, geometry and gameplay are unchanged by this revision.
- Scope at that checkpoint: the small-leaf color leak was fixed, but the grass
  strip subsequently identified in the user's close-up was not. That strip was
  mistakenly assumed to be opaque tree artwork; see the viewport correction below.

### Grass-strip viewport correction (2026-09-07)

The close-up identifies the transparent `gras` material on W01 terrain meshes
`obj40`, `grid314`, and `obj41`, not the tree's bark bitmaps. Rendering the private
foreground target directly revealed that the protection was shrunk/displaced
toward the upper-left: logical camera pixels were being used in a full-resolution
offscreen target. Its depth test consequently compared unrelated screen locations.
This is port-owned viewport integration, not incorrect gameplay/decompiled logic.

- Corrected only the private viewport/scissor mapping. No texture recoloring,
  color key, grass-specific shader rule, AO strength/radius change, mesh edit,
  or depth-test bypass is shipped. The WGSL shader is unchanged from the prior
  foreground-color checkpoint. Existing premultiplied RGB protection now aligns
  with the actual visible grass and leaves, including occlusion by opaque flowers.
- `gtao-grass-render-pixels-state` created an isolated state; the `-before`,
  `-after`, `-off`, and `-mask` runs restored it using one QA executable.
  `-before` uses the QA-only legacy viewport switch; `-off` sets AO shading to
  zero without changing geometry or passes; `-mask` displays foreground RGB.
  At tick 7511 all 141 mesh positions/vertices match. The user-edge crop
  [1360,420,1920,720] has 15,101 fully covered grass pixels. Their maximum
  difference from unshaded artwork falls from 58 RGB steps to 1; 2,820 previously
  differed by more than 8. Nearby unprotected solid surfaces retain their AO.
  The crop changes 8,574 pixels, with +58/-0 maximum channel delta.
  Reproduce assertions and the unscaled comparison image with
  `tests/integration/verify_foliage_viewport.py BEFORE AFTER UNSHADED FOREGROUND`.
- 55 CPU/native tests pass, including physical-pixel viewport/scissor mapping,
  split rectangles, fractional scale rounding, clipping, 4K extents, invalid
  targets, and headless no-ops. All 15 production-WGSL Vulkan tests pass.
- The matched 1920x1080 captures use MSAA 4x. `gtao-grass-viewport-1440-fresh`
  verifies three 2560x1440 board captures with MSAA 4x, exit 0. An earlier
  cross-resolution state load hit the existing address-layout guard; that run's
  boot captures are excluded, and no guard was bypassed.
- `gtao-grass-viewport-replay-board` runs 60 Hz logic and Unlocked FPS with 1.5x
  SSAA (2880x1620 target). Its 90 captures include 42 board frames, four of them
  interpolated; board captures were visually reviewed. Live Subtle/Strong/Off/
  Subtle transitions are logged through tick 7930, exit 0. The capture buffer
  ends at tick 7628, so it does not visually prove the later transitions.
- Windows graphical/headless and Android release native builds complete. Windows
  executable SHA256: `951d7e95a37a832b43f584f9f5551c93217523679d04b5e94a2b4b749ac260a9`.
  Android unstripped game library SHA256:
  `ea6682080cc2685f90d0c861a32ead53fd7cd3a9135699cdd212e408aa70eebf`.
  No Android device visual test, APK assembly/upload, performance guarantee, or
  full board-cycle pass is implied. Shared decomp and Aurora repositories remain
  untouched; all test saves/configs/artifacts are isolated under `build`.

### Temporary-event foreground correction (2026-09-07)

The user's running DK-space scene reproduced another foreground leak. Slot 5
was empty and was used to preserve that exact live session before investigation.
Inspection of the saved native model/material structures identified model 204,
the DK event's `tree1` / `leaf0`..`leaf7` (`cp37` texture, resource `0x000E0021`).
Its ordinary alpha-blended, depth-tested/non-writing batches were eligible, but
the W01 scenery allowlist omitted this temporary prop. Background AO therefore
appeared as a dark stripe across the leaves in front of the floor signage.

- A narrowly scoped debugger experiment added that existing tree to an unused
  port AO-registry entry in the running process. It did not patch code, move
  geometry, change gameplay or bypass save-state compatibility checks. The
  original registry bytes are retained in `build/recovery/live-ao-registration-backup.json`.
- `build/recovery/live-dk-ao/before.png` and `registered.png` show the same live
  scene/camera with Strong AO and SSAA 2x. The background stripe on the leaves
  disappears while surrounding opaque surfaces retain AO. These captures have
  slightly different idle poses; they are not a same-tick pixel comparison.
  `comparison.png` is an unscaled crop of those actual game captures.
- The permanent source fix removes the scenery allowlist, using a tested
  draw-state policy for all ordinary alpha-blended event/scenery batches. Special
  blends, hooks, reflections, Z-off draws and depth-writing cutouts remain
  excluded. The WGSL, AO strength/radius, original draw state and geometry are
  unchanged.
- All 56 CPU/native tests and 15 production-WGSL Vulkan tests pass. The new native
  test covers the observed DK material, an arbitrary unregistered event model,
  alpha/alternate-blend cases, high material-index bits, and every exclusion.
- Production Windows graphical and headless builds are staged under
  `build/recovery/ao-event-foreground`. They do not replace the running executable
  or its PDB. The live registry experiment validates the missing-prop diagnosis;
  it is not a live reload of the rebuilt, broader draw-state implementation.
  Windows candidate SHA256:
  `9a73fd85140fbd7a1c2205e02b99225d3ac56868a542a16de34f71cb4170cd03`.
- Android release native build also completes against the existing audited
  port-local toolchain/backend (178 verified optimized translation units).
  Unstripped game-library SHA256:
  `6972c2ba625f766e31894c9a2d4fdc764219ce4741032745c8c36a912b1d8d43`.
  No APK assembly/upload, Android hardware or complete board-cycle verification
  is implied. The original running Windows executable remains unchanged.

## Historical SSAO integration verification (2026-09-06)

These checks established integration/lifetime correctness, not satisfactory
close-up quality. Subsequent user captures exposed bands, harsh seams and noisy
decal outlines; see the quality revision below.

Evidence lives under ignored `build/board-qa-runs/`:

- `ao-stable-state` and `ao-stable-0/1/2`: isolated quick-state capture/restore,
  with twenty matching board frames per setting. Subtle/Strong only darken scene
  RGB, with stronger contrast at level 2; opaque HUD anchors remain within 1–2
  channel values. No intermittent shading drops after the lifetime repair.
- `ao-live-fi-stable`: Off → Subtle → Strong → Off → Subtle in one running
  board session. Fifty captures include real and interpolated frames. The 5-second
  board window including capture/compilation overhead recorded 1,059 presents/s
  at 1024×768 with 60 Hz game logic, on this RTX 4090 PC. This is one test scene,
  not a universal FPS guarantee or a controlled performance comparison.
- `ao-msaa-board-final`: fresh boot through real party setup into board turns
  with MSAA 4x + AO + Unlocked FPS, 6,000 ticks, clean exit, 178,841 completed
  presents. Board windows recorded roughly 1,500 presents/s at 1024×768.
- `ao-wide-shadows-final`: 1280×720 widescreen with 4x offscreen shadow maps,
  12,419 ticks into the second round, 23,327 matching AO encode/composite calls,
  captured HUD/board output and clean shutdown. No renderer validation errors.
- `build/ao-ui-qa/settings-review`: actual Windows settings row, readable help,
  Off/Subtle/Strong selections, correct Custom preset derivation, no restart
  warning, persisted isolated config and clean close. User config/saves untouched.

All 44 game/host regression tests pass. Run
`python -m unittest discover -s tests -p test_*.py` for native camera mapping,
headless isolation, config round-trip, preset and platform UI contracts, plus
the existing game regressions. `tests/integration/build_board_qa.py` provides
an opt-in live-setting sweep, never compiled into production builds. Use
`compare_ao_captures.py` only with captures from the same state and matching ticks.

Windows graphical/headless and Android release native builds compile. Android
device rendering/performance has not been tested; no APK upload is implied.

## Quality revision (2026-09-06)

- `ao-quality-old-state` reproduces the original horizontal ground bands and
  outlined spaces at 1600x900. `ao-quality-new-state` uses the same opening
  camera with the revised Strong setting; ground bands and heavy decal outlines
  are no longer visible. These are separate board runs, not pixel-matched RNG.
- `ao-quality-opening`: 80 captures through party setup, the board fly-in,
  character/dice introduction and first turn, at 1600x900; clean shutdown.
- `ao-quality-check-0`, `ao-quality-check-1-retry`, `ao-quality-check-2`:
  twelve frames per level restored from the same new-build state, matching ticks
  12011..12033. Maximum RGB darkening is 16/28 for Subtle/Strong; no pixels
  brighten by more than one channel value and opaque HUD anchors differ by at
  most one. The first Subtle attempt was excluded: the existing savestate
  address-layout guard correctly refused a different arena address.
- `tests/integration/test_ao_gpu.py` executes the actual production WGSL on
  synthetic depth using optional wgpu-py 0.32.0 (Vulkan/RTX 4090 here).
  Nine tests cover exactly unshaded sloping planes at near/far distances and
  odd dimensions, shallow floating decals, retained real contact shading,
  intensity bounds, clear depth/scissor, reversed/ranged depth, denoising and
  3840x2160 reconstruction with a 1600x900 AO working set. Reusing the raw
  target for the second blur is pixel-identical to three separate targets.
  Two small rounded shoes also require visible shading on the nearby ground;
  a large raised rectangle alone was insufficient to catch an overly weak AO.
- `ao-quality-live-fi`: restored board state, live Subtle -> Strong -> Off,
  with real/interpolated frame captures; clean guarded exit. Its captures
  include the original page-fold turn transition, not a rendering defect.
- `ao-quality-msaa-fi`: MSAA 4x + Strong AO + Unlocked FPS at 1280x720,
  5,700 logic ticks, 169,607 completed presents and matching 311,549 AO
  encode/composite callbacks. A five-second board-entry window recorded
  1,433 presents/s on this RTX 4090; this is not an all-scene FPS guarantee.
  The short capture burst falls within the original fade-in.
- `ao-quality-ssaa`: 1920x1080 output with SSAA 2x, 10,500 ticks, a captured
  fully visible board turn and clean shutdown. Higher internal resolution
  does not restore the old terrain striping.

Run the GPU tests separately from the 44 CPU/host regressions:

```powershell
python -m pip install --target build/ao-gpu-test-deps wgpu==0.32.0
python tests/integration/test_ao_gpu.py
```

The optional Python dependency is QA-only, never linked or packaged with the game.

### Contact-strength follow-up

The first quality revision suppressed artifacts but also made small character
contact too faint. User close-ups exposed this. The final shader keeps the
precision/filtering/contact-bias repairs, concentrates disk taps near contacts,
extends the radius from 24 to 32 units and raises the uncapped response by 2.4x.
The per-camera 20%/35% darkening bounds remain unchanged.

`ao-contact-state` captures the standing characters during the board intro.
`ao-contact-0/1/2` restore that same state with Off/Subtle/Strong at 1920x1080
output, 3840x2160 SSAA scene resolution. Six frames per level match ticks
7511..7516. The first-frame comparison is `ao-contact-2/feet-off-subtle-strong.png`:
contact shadow under Luigi is visibly restored without horizontal striping.
No pixel brightens by more than one RGB value. The final Windows release and
Android native library both include this response adjustment; Android hardware
rendering remains unverified.

Board-space artwork has an existing +3 world-unit translation in MasuDraw,
whereas mbPlayerPosReset uses mbMasuPosGet directly. That is not evidence of a
single global terrain/actor offset: model origins, animation poses and the
actual supporting mesh also matter. The subsequent measured, selective visual
correction is documented in [GROUNDING.md](GROUNDING.md). It does not move the
terrain or change gameplay coordinates.

## Exact bandwidth/work reductions (2026-09-08)

This revision leaves AO resolution, GTAO sample count, strength, blur kernel and
R32 depth precision unchanged. It targets the Android screenshot's expensive
depth snapshots and blur passes without introducing a lower-quality preset:

- If all seven blur samples are exactly unshaded, return the existing value
  without reconstructing depths and computing bilateral weights. Otherwise use
  the original filter and arithmetic order. No epsilon or contrast threshold is
  used to discard faint contact shading.
- Port-owned Aurora patch `0029-reuse-unchanged-depth-snapshots.patch` reuses an
  immutable R32 snapshot only in a continuation with no intervening draws or
  clears. This removes the duplicate final-depth copy between foliage coverage
  and AO application. New frames, targets, draws and clears cannot reuse it;
  offscreen passes cannot borrow an EFB snapshot.
- Depth-only intermediate EFB passes retain MSAA samples without resolving an
  unused single-sample color image. Later color consumers and final presentation
  still resolve normally, including an empty final continuation.

Verification:

- The 64-test CPU/native suite covers console clearing, Android/desktop display
  branches and the exact production depth-snapshot state machine, including
  draw/clear invalidation, offscreen suspension, unsupported depth capture and
  later color consumers.
- All 16 production-WGSL quality tests pass. The separate exact-fastpath GPU
  test compares the old and new blur using foreground feet, coarse holey decals,
  forward/reversed depth and native/reduced/odd target sizes. Raw AO, filtered AO
  and final composite must be bit-identical.
- The synthetic, GPU-resident RTX 4090 benchmark reports combined blur time
  reductions of 62.5-66.7% at 1280x720, 1920x1080 and 2401x1081. This excludes
  game rendering, depth copies, foliage coverage and presentation. It is **not**
  an Android FPS prediction; the Tab S10 Ultra still needs device-side timings.
- Windows Release board rendering smoke runs with FXAA and AA off complete
  9,020 ticks without a reported renderer error. Fresh-board captures have
  different randomized HUD ordering and animated leaves, so they are visual
  smoke evidence, not matched-state pixel-equivalence proof.
- Windows and Android native Release builds compile. Android Release APK
  assembly and lint complete with no new lint issues (existing baseline issues
  remain). This local APK has not been signed or uploaded as a new preview.

Reproduce the shader checks with `tests/integration/test_ao_gpu.py` and
`tests/integration/test_ao_exact_fastpaths.py`; the optional resident benchmark
is `tests/integration/benchmark_ao_exact_fastpaths.py`.

### Android startup regression and compatibility recovery

The user reports that private 0.4.10 repeatedly displays the in-game menu hint
after Play on a Tab S10 Ultra, with severe slowdown. Force-stopping and starting
with AO off succeeds; AA is FXAA. This confirms an AO-dependent regression but
does not distinguish the new blur fast path from depth-snapshot scheduling.
The tablet is not attached, so the precise driver/runtime cause is unproven.

The subsequent user test reports that the same 0.4.10 APK starts on a Snapdragon
Galaxy S22+, but performance is still inadequate. The recovery is therefore
device-selective: port-owned patch 0030 uses the selected adapter's vendor to
choose one shared shader/scheduling policy. Mali and other unverified Android
vendors use the pre-0.4.10 shader and depth-copy/resolve schedule; Qualcomm and
desktop keep the optimized path. Unknown vendor IDs fail conservatively. Vendor
IDs follow [Chromium's adapter constants](https://chromium.googlesource.com/chromium/src/+/refs/tags/142.0.7442.1/gpu/config/webgpu_blocklist_impl.cc).
This is a compatibility policy, not proof that every driver in a vendor family
works or identification of the precise Mali failure.

Resolution, samples, precision, strength, foreground protection, fullscreen and
console fixes remain unchanged. AO is not silently disabled. Mali recovery
deliberately withdraws the new AO optimizations pending device verification.
The earlier all-Android recovery APK is retained locally, not overwritten.

Tests execute the actual C++ shader builder and freeze a whitespace-normalized
hash of the compatibility WGSL. Only the selected source is submitted to the
driver; the seven-sample blur array is absent from the compatibility shader.
Native tests execute both scheduling policies with Qualcomm, ARM, unknown,
NVIDIA and AMD vendor IDs under Android and desktop compilation.
The original 0.4.10 Windows interactive launcher with Vulkan, AO and FXAA reaches
the title screen; it does not reproduce the tablet failure. Earlier smoke runs
used `--aa 2` (FXAA), not MSAA 4x (`--aa 1`); the earlier label was incorrect.

### Further exact work rejection and the 2–3x target

The optimized shader now rejects GTAO samples whose original contact or distance
weight is exactly zero before evaluating the remaining square-root/falloff work.
An unchanged horizon contributes zero occlusion, so it skips the redundant
visible-arc calculation while preserving the denominator. Reduced-resolution
compositing tests for an exactly unshaded signal before fetching receiver depth.
No sample is dropped for being merely faint; the kernel, precision and thresholds
are unchanged. Compatibility devices retain the entire earlier shader.

The 64 CPU/native tests, 16 GPU quality tests and three GPU equivalence tests
pass. Equivalence covers 511x257 through 2960x1848, native/reduced sampling,
forward/reversed Z, cutout decals, later characters, clear regions, dense edges,
clipped viewports/scissors and nondefault depth ranges. Windows Release
`ao-device-release-fxaa` completes 9,020 ticks, exit 0, with 12,710 AO jobs
encoded/composited and no reported renderer error. The separate
`ao-device-release-msaa` smoke run also completes 9,020 ticks with exit 0.
Android Release native
compilation succeeds; neither Android device is attached.

`tests/integration/benchmark_ao_device_paths.py` benchmarks the actual current
shader against an explicitly supplied archived 0.4.10 header, with alternating
resident work and exact image checks. The final production run reports
1.01–1.20x GTAO pass speed and 1.01–1.17x measured AO-pass-sum speed across the
feet/dense-edge fixtures at 1920x1080 and 2960x1848. The report is
`build/ao-0411-device-production-benchmark.json`. Every compared pixel is equal.
These are pass-specific RTX 4090 measurements, not Android FPS or an achievement
of the requested 2–3x game-wide gain. The earlier Android screenshot's EFB row
includes AO compositing, so summing only separately named AO rows does not
establish an AO cost fraction or a firm maximum possible speedup.

An inline decal-removal experiment eliminated the full-size clean-depth target
but was rejected: repeated source-depth reads made the synthetic measured pass
sum roughly twice as slow. The production clean-depth pass remains intact.
Further mobile optimization needs same-scene AO-on/off timings on the affected
hardware; desktop timings cannot identify its remaining bottleneck.

## Shared Android/PC optimization pass (2026-09-08, not uploaded)

The board's existing exact decal-removal pass now also converts its output to
linear view depth. It remains **R32Float at full framebuffer resolution**.
Horizon sampling, normal reconstruction, both denoise passes and the final
composite reuse this conversion. Cameras without decal brackets retain the
original raw-depth path; they do not pay for an extra preparation pass.
The original 24 samples, radius, strength, contact bias, depth precision,
resolution budgets, water exclusion and premultiplied foliage protection are
unchanged. Raw/prepared composite pipelines are keyed separately, and all new
GPU handles remain host-owned and are released on shutdown.

This applies to both the optimized and conservative Mali kernels without
re-enabling the withdrawn seven-sample-array fast path or changing Mali's
depth-copy/resolve schedule. The user confirmed that the latest *uploaded*
0.4.12 build runs on the Tab S10 Ultra with AO enabled. That confirmation does
not cover these not-yet-uploaded changes.

The shared settings layer now resolves environment overrides after startup
argument parsing instead of repeating `getenv`, preset parsing and validation
in per-object/material draw calls. Publishing live settings refreshes the
resolved values immediately. Bootstrap queries still work before initialization;
diagnostics changing environment variables can explicitly refresh the cache.
Preferences and their cache are excluded from quick states. The fresh board
probe counted 6,460,454 accessor calls over 9,300 ticks (including startup),
demonstrating why these previously presumed infrequent lookups matter.

### Verification and research scope

- 76 CPU/native tests and 25 GPU tests pass, including new exact-pixel tests for
  prepared depth on both kernels, far sloping planes, reversed Z, partial depth
  ranges, scissors, odd sizes, low-resolution decal cutouts, water and foliage.
- Ten same-state 1920x1080 FXAA captures are pixel-identical before/after,
  including RGB HUD pixels. Both runs restore normally after coroutine startup;
  a preliminary tick-1 restore was correctly refused because no coroutine pool
  existed yet. No save compatibility check was bypassed.
- `ao-opt-final-live-msaa-fi` completes 8,200 ticks with MSAA 4x, frame
  interpolation and ten live AO changes through Subtle/Strong/Off. All 2,607
  AO encode/composite callbacks match. Twelve reviewed captures include a replay
  frame, board spaces, characters, foliage and dice effects. Exit is 0; pending
  timestamp mapping is cancelled at normal device teardown. This is a rendering
  smoke test, not proof of an entire board game or Android compatibility.
- Windows and Android native Release builds pass. The Android manifest verifies
  all 178 native translation units use the release `-O2` profile. No new APK has
  been packaged or uploaded as part of this pass.
- `build/ao-prepared-production-compatibility.json` records resident RTX 4090
  Vulkan tests of the **Mali shader variant**, not Mali hardware: the sum of AO,
  denoise, composite and decal-preparation pass throughput improves 2.1–9.2% across
  1920x1080 and 2960x1848 feet/dense-edge scenes, with zero pixel difference.
  The benchmark alternates variants, runs 2,048 resident repetitions, timestamps
  the final 32, discards two warmup rounds and reports six-round medians.
- `build/ao-prepared-production-optimized.json` repeats with the optimized kernel
  and Android shading budget: measured pass-sum throughput improves 2.4–12.4%,
  again with zero pixel difference. This is still desktop GPU evidence, not an
  Android device measurement.
- `build/ao-prepared-production-desktop.json` uses the PC's 1920-pixel shading
  budget. The same four fixtures are pixel-identical, with 8.0–11.0% better
  measured pass-sum throughput (GTAO alone: 12.3–12.9%).
- Real same-state board CPU measurements and their limitations are recorded in
  [ANDROID_PERFORMANCE.md](ANDROID_PERFORMANCE.md). This is not a measured
  Android FPS multiplier, nor an assertion that optimization is complete.

[XeGTAO](https://github.com/GameTechDev/XeGTAO) describes preparing view-space
depth before the AO kernel. This port fuses that work into an already-required
pass instead of adding its depth pyramid or lowering sample count. Arm's
[GPU best-practice guide](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695)
informs the emphasis on bandwidth and render-pass cost. An algebraically
equivalent horizon-integration experiment was rejected: its small speedup was
inconsistent and it changed some half-float rounding boundaries.

Reproduce with `tests/integration/test_ao_prepared_depth.py` and
`benchmark_ao_device_paths.py --baseline <archived-header> --prepared-depth
--compatibility --output build/<new-report>.json`. Omit `--compatibility` for
the optimized kernel; use `--desktop-budget` for the PC shading budget.
`run_board_qa.py --raw-ao-depth --uncached-settings` enables the old behavior
inside the isolated QA executable only, allowing guarded same-state A/B tests.
