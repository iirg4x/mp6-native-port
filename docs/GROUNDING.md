# Selective visual grounding

`src/hsf/mp6_grounding.c` corrects measured **static scenery** gaps in W01
(Towering Treetop) when ambient occlusion is enabled. Off retains the original scenery. Changes
apply live with AO, without a new setting or restart. This is a port enhancement,
not a claim that the original game or its decompilation is incorrect.

## Scope and safeguards

- Explicit W01 registration builds a bounded triangle cache from the actual
  static lawn meshes `haikei10`, `haikei11`, and `haikei17`. The cache follows HSF
  hierarchy transforms and native GX triangle/quad/strip index order. Queries
  stay inside the triangle footprint, reject steep surfaces, and require nearby
  support (at most 32 units). This is not a universal terrain raycaster.
- Flower, clover and grass branches with a measured small base gap are fitted
  together, preserving their internal wind animation. Grounded signposts are
  handled as well. The start graphic and its descendant terrain keep their
  authored matrices.
- The starting path and flat lawn are raised to the authored start-space floor,
  rather than lowering actors. The path's highest point sits 0.25 units below
  the flagged starting position used by the original opening logic; the lawn
  retains 0.5 units of clearance below the path's lowest point. Calibration
  verifies the supported footprint, flatness and static start-space reference.
  The full connected background (nine `haikei` terrain meshes and the `grid309`
  lower plane) receives one uniform world-up translation, including its slopes,
  cliffs and distant sections. The path retains its separately calibrated
  clearance. Source arrays and every model/object matrix remain unchanged. Unsupported paths, non-flat
  supporting terrain, animated reference transforms or changed reference geometry
  disable the lift. The contact cache is rebuilt from the raised draw copies so
  the existing vegetation and log/tree-base corrections use the same surface.
  Their candidate vertices/branches are still selected against original support,
  so raising the lawn cannot draw upper vertices into the bottom repair band.
- Three static log meshes (`obj34`, `kirikabu`, `obj37`) have measured base gaps
  of about 3-12 world units. `obj34` contains two disconnected logs in one mesh.
  Separate, bounded draw-vertex copies fit only their supported bottom rings
  to 0.1 units below the lawn. Tops, branches, platform heights and original HSF
  buffers stay unchanged. Animated object transforms and AO Off use the original
  vertex array; this is not a whole-log translation or a general mesh repair.
- The main tree is a separate W01 model, explicitly registered at its creation
  (day asset `0x00D7002B`, root `b01_m240`). Its two `b01_m240` meshes and
  `r_ashi`/`l_ashi` roots use the same bounded bottom-vertex fitting for measured
  gaps under 32 units. The slide, platforms, branches and facial pieces are not
  translated. The four alternate event models are not registered.
- Draw copies require unchanged model identity, source buffer, owner transform,
  every ancestor pose, and source vertex contents. Shape/cluster updates, animated
  parents or moved models fall back to the live original array, never a frozen
  snapshot. At most nineteen copies (path, ten background meshes, seven log/tree bases and
  the white connector mesh), each at
  most 768 vertices, are retained.
- Players, guides, board-space artwork and board effects retain their original
  transforms for every AO setting. There is no actor sole calibration, space
  plane fitting, player-slot binding or per-event attachment exception. All
  dynamic participants use the same authored coordinate system.
- The separate white-link mesh `b01_m001` uses bounded draw copies for its four
  lawn-level quads only. They sit 0.5 units above the raised path's highest point,
  below the space icons. Elevated links retain their exact original vertices;
  unsupported/sloped quads, shared-vertex topology and animated/moved meshes are
  excluded. Original blending, textures, depth tests and source buffers remain.
- Corrections affect render matrices, selected draw-vertex copies and original
  shadow passes only. Model positions, board coordinates, collision, original HSF
  vertex buffers, motion time and the camera remain unchanged. No camera,
  including the original shadow pass, applies an actor correction.
  Frame interpolation retains the static scenery's corrected GX
  matrices through its existing path.
- No external heap/GPU allocations: registries live with captured game state,
  validate model identity, and clear at W01 exit. Quick-state restores rewind
  those registries along with their game-heap pointers.

Not covered: arbitrary props, sky artwork, the tree's facial pieces, dynamic bridge geometry,
other boards, or original animation/asset defects. Do not broaden the mesh/name
allowlist without new world-space measurements and movement tests.

## AO-only dynamic placement regression (2026-09-07)

### Complete background lift and slide seam (2026-09-07)

The initial lift changed only flat tops in `haikei10`, `haikei11` and `haikei17`.
That left adjacent terrain sections and shared lower vertices at their original
heights. It was a port-owned incomplete landscape adjustment. The current lift
uniformly translates all vertices in `haikei1`, `haikei10`, `haikei11`, `haikei12`,
`haikei14`, `haikei15`, `haikei16`, `haikei17`, `haikei2` and `grid309` by the same
15.1336 units. Their authored internal slopes and boundary relationships are
preserved; no camera-dependent radius or start-area cutoff is used.

The native fixture exercises every background name, distant sloped vertices,
cliff bottoms, live Off/Subtle/Strong and headless behavior, and unchanged source
arrays and path/actor matrices. `terrain-slide` completed the original slide
and three player turns; eight full-resolution samples spanning its ride/landing
were reviewed. `terrain-slide-off` / `terrain-slide-on` restore one compatible
post-slide state. `check_background_terrain.py` checks every background mesh
visible in that camera: all 311 vertices translate uniformly, 32 matched boundary
vertex pairs preserve their original gaps within 0.003 units, all 69 logical mesh
positions and all 108 space matrices are unchanged. The three visible player
meshes also remain identical. Frustum-culled sections are covered by the native
fixture and asset inventory, not falsely counted as captured draws.

Windows Release and Android release native builds include this change. The
`terrain-release-smoke` run checks the normal Windows binary through the board
introduction. The Android mobile AO budget is also rendered in the paired
`terrain-slide-mobile` capture, on the desktop GPU; this is not Android hardware
testing or a new APK upload. The source/runtime checkpoints are under
`build/checkpoints/before-background-terrain-mobile-ao` and
`build/checkpoints/before-terrain-mobile-release`.

The older measurements below describe the earlier partial lift and are retained
as historical evidence; their unchanged-lower-cliff assertions are superseded.

### Provenance boundary: start path and lawn

`tests/integration/audit_iso_ground.py` reads `data/w01.bin` directly from the
user-provided USA ISO's FST, verifies it is byte-identical to the port's extracted
archive, and independently decompresses entry 4. It parses the big-endian HSF
vertices and parent/base transforms and reconstructs the static hierarchy using
Python/NumPy, without invoking the port's loader or matrix implementation.

All 342 vertices of `start`, `haikei10`, `haikei11` and `haikei17` match the
AO-off native draw capture within 0.0031 world units in any coordinate and
0.0007 units in Y. The ISO reconstruction puts the start graphic at approximately
Y=-3.016..-2.901 and the adjacent lawn near Y=-10. Thus this particular gap is
present in the asset data, not introduced by the port's static transforms.
It may be deliberate layering; it is not proof of an original-game defect.
The visible AO band is introduced by the port's optional shading. This audit
does not certify animated/skinned characters, every other board object, or the
original GPU's final rasterization. Its report is
`build/board-qa-runs/ao-placement-actors-off/iso-ground-audit.json`.

The proposed start-path lowering was backed out before deployment when the
user questioned provenance. The subsequent user-requested surface lift below
raises static artwork; all dynamic participants retain their authored positions.

### Raise the lawn, not lower the path (2026-09-07)

First iteration, superseded by the additional foot-contact lift below:

`lawn-lift-before` / `lawn-lift-after` restore the same compatible quick state
at tick 9001, Strong AO, 1280x720, MSAA 4x and widescreen. The QA-only
`--no-lawn-lift` baseline rebuilds the visual contact registry after restore,
using the original floor; it does not change saved simulation state.
`compare_lawn_lift.py` verifies 186 flat-top vertices rise by approximately
6.4824 units and all 108 remaining lawn/cliff vertices stay exactly unchanged.
Across 15,708 supported path samples, the gap falls from 6.9848-7.1015 to
0.5024-0.6191 units, without any path vertex moving or intersecting the lawn.
All 141 logical mesh positions, all four players (11 meshes) and all 108 space
matrices remain identical. The existing static corrections recalibrate upward;
none of the changed scenery moves lower than the baseline.

The actual framebuffer comparison removes the dark raised-platform border
around the starting area without weakening AO. Full frames and
`lawn-lift-metrics.json` are under `build/board-qa-runs/lawn-lift-after/`.
The paired `lawn-lift-off` run also passes `check_ao_placement.py`: all 16 dynamic
meshes are identical with AO Off/Strong. Repeating the direct ISO audit against
that Off capture still matches all 342 original static vertices within the
same 0.0031-coordinate/0.0007-Y bounds above.

Native tests exercise the actual runtime in graphical and headless builds,
including live Off/Subtle/Strong behavior, untouched source/path/cliff geometry,
log/vegetation support recalibration, reference/owner mutation, unsupported
paths, sloped floors and teardown. The new lawn-copy assertion fails on the
previous static-only implementation and passes with the lift.

### Additional foot-contact lift (2026-09-07)

The first lift stopped at the original path and left the players' soles about
8-9 units above it. The user requested raising the path too. The current version
reads the authored `MASU_FLAG_START` position already loaded by `MB1_Create`;
it does not read a player's animated pose or modify board-space state. The path
now rises 8.6511 units, with the flat lawn rising 15.1336 units in total.
Original hierarchy matrices are unchanged, so children are not moved twice.

`start-floor-before` / `start-floor-after` restore one compatible saved scene
with Strong AO. The updated `compare_lawn_lift.py` verifies all 141 logical
positions, all four players' 11 meshes and all 108 space matrices are identical.
All 108 non-top lawn/cliff vertices remain exactly unchanged. The 15,708 path
samples retain 0.5024-0.6190 units of lawn clearance. Sampled Mario, Luigi and
Yoshi sole vertices have 0.38-0.98 units of shallow overlap, while Brighton's
sampled shoe has 0.30 units of clearance. This fixed floor accommodates small
authored idle-pose variations; it does not track animations. Peach's dress hem
is not misclassified as a shoe. The starting-area spaces (18, 21, 28 and 64)
retain at least 3.2497 units of clearance across their complete 21x21 sampled
footprints. Actual matched framebuffer crops show the improved foot contact.

`start-floor-off` confirms all 15 dynamic meshes stay unchanged with AO
Off/Strong, and the direct ISO audit still matches all 342 original static
vertices within the bounds above. The source buffers remain untouched in
every mode. Native tests also reject absent/platform start references and
ensure raising the lawn never selects new upper log vertices for base fitting.

`start-floor-slide` completes the authored slide and three player turns with
Strong AO; twenty sampled frames cover the ride, landing and following pickup.
`start-floor-live` restores the same scene and changes AO through Subtle,
Strong, Off, Subtle and Strong with interpolated presentation and clean exit.
It is a live-toggle test, not a completed-turn test. Windows Release is rebuilt;
`start-floor-release-smoke` verifies its normal, non-QA executable through
9,020 ticks and captured board-introduction frames, with no fatal marker.
its previous runtime/config/slot-5 files are preserved at
`build/checkpoints/before-start-floor-release/runtime/`. These are bounded tests,
not a claim to have checked every character pose or board event.

### White connector layer regression (2026-09-07)

Raising the lawn and starting surface left the separate additive white path
mesh (`b01_m001`, texture `I8line`) at Y=2. The new surfaces at Y=5.13-5.75
therefore covered its links between the blue spaces. Same-state zero-strength
AO captures still show the missing strip: this was a port-owned layer-order
regression, not evidence that AO intensity or depth filtering needed changing.

The fix raises only its four supported flat quads to Y=6.25, leaving all 352
other link vertices unchanged. `connector-before` / `connector-after` use one
compatible state, Strong AO, 1280x720 and MSAA 4x; the QA-only
`--no-connector-lift` switch reproduces the old buried layer. The separate
`check_connector_depth.py` verifies 140 other meshes and all 141 logical
positions are identical, all original materials/flags are retained, and all
space matrices stay unchanged. Across 1,848 samples the links have at least
0.4996 units of surface clearance and 2.7501 units below the space layer.
The actual framebuffer restores 1,935 brighter pixels in the two sampled
connector strips without changing any sampled space-icon pixel.

Native tests cover Off/Subtle/Strong, headless mode, source/pose mutation,
unchanged high links and rejection of sloped or partially unsupported quads.
The regression fails before the connector fix and passes afterward. The paired
`connector-off` run confirms unchanged dynamic geometry; `connector-live`
cycles through all AO settings after a guarded state restore. All 56 CPU/native
and 15 GPU regressions pass. Evidence and the direct before/after crop are under
`build/board-qa-runs/connector-after/`. The previous Windows runtime is preserved
under `build/checkpoints/before-connector-depth-release/runtime/`.

### Actor/effect offsets removed (prior verification)

The earlier render-only actor/guide/space adjustments were removed. Their
source/destination-space interpolation did not describe scripted movement,
off-route positions or prop attachments. Player bindings also survived turn-order
permutation while continuing to read the old slot's space IDs. Moving a rendered
actor independently of effects and event sockets could therefore bury its legs
or separate it from those effects. This was port-owned, not a decomp fix.

`test_grounding.py` compiles the actual patched Hu3DExec model submission block
into the native fixture. The new actor/effect alignment assertion fails on the
previous implementation and passes without the dynamic hooks. It covers every
AO level, multiple cameras, original shadow rendering, jumping, scale, local
matrices, off-route movement, reordered player slots and attached-space flags.
Source contracts also check that the board player, opening, capsule, effect and
space paths contain no grounding hooks; the AO space-depth bracket is retained.

`ao-placement-actors-off` / `ao-placement-actors-on` restore one compatible QA
state at tick 9001, 1280x720, MSAA 4x, widescreen. `check_ao_placement.py` verifies
identical submitted vertices for all four players (11 meshes), all 16 dynamic
meshes in view, all 141 logical mesh positions, and all 108 authored space
matrices. Space matrices are reconstructed by the QA probe and backed by the
production-source contract; actor vertices come from actual draw submissions.
Only registered static scenery changes. The paired framebuffer captures and
`placement-check.json` are under `build/board-qa-runs/ao-placement-actors-on/`.

`ao-placement-slide` completed the original slide ride and subsequent turn with
Strong AO; its slide begin/end and completed-turn gates pass. Twenty sampled
frames spanning the ride, landing and following space effect were reviewed.
`ao-placement-dk` exercised the original DK leaf event through capsule return
and turn end with Strong AO; twenty sampled frames cover the leaf rise, player
attachment, DK arrival and dialogue. Its event/turn gates pass. The unchanged
production Release executable also boots through the board introduction in the
isolated `ao-placement-release-smoke` run (9,020 ticks, captured frames, no fatal
marker); this is a boot/render smoke, not a full-match test.
These are bounded tests, not a claim to have replayed every board event. Original
authored art/animation gaps can remain; AO no longer moves actors to hide them.

## AO regressions (2026-09-07)

- Historical, superseded by the static-only revision above: the old centre-only space placement was replaced with a cached plane fitted
  to the complete decal footprint. Original placement is retained wherever
  that footprint is not fully supported by the registered static terrain.
- AO filtering now checks the tangent planes of both the source and receiver.
  Composite reconstructs the receiver normal from full-resolution depth;
  grazing background surfaces cannot borrow visibility onto foreground actors.
  A Vulkan regression reproduced 20.36% erroneous foreground darkening before
  this change and zero afterward. Existing contact/flat-plane tests still pass.
- Historical: `check_space_ground.py` validated QA-captured decal matrices against the
  submitted W01 terrain. `ao-leaf-check` checks all 108 spaces: five supported
  corrections have at least 0.5002 units of clearance across a 21x21 grid;
  unsupported spaces keep their authored transform. This run predates the
  additional DK attachment correction.

### Log bases and foreground foliage (2026-09-07)

The measurements in this subsection precede the lawn lift. Current base copies
use the raised floor; the bounded vertex-selection and original-buffer policy
remain unchanged.

`gtao-log-before` / `gtao-log-state` use the same isolated quick state and Strong
AO at 1920x1080. The test-only switch disables only the new log vertex copies.
`compare_log_grounding.py` verifies all 141 logical mesh positions are identical
and all 138 other captured meshes are unchanged. Only 49 bottom-ring vertices
change across the three selected meshes; their 378 other vertices stay identical.

| Mesh | Changed vertices | Base gap before | Base gap after |
| --- | ---: | ---: | ---: |
| `obj34` (two logs) | 20 | 2.995-12.219 | -0.100 |
| `kirikabu` | 13 | 10.002-10.003 | -0.099 |
| `obj37` | 16 | 9.998-10.009 | -0.100 |

Native runtime tests cover unchanged source arrays and tops, unsupported vertices,
animated-object exclusion, live Off behavior and scene exit. The matched image
is `build/board-qa-runs/gtao-log-state/log-grounding-before-after.png`.

Some foreground grass/leaves also use alpha blending without depth writes.
Their background AO previously darkened the already-composited leaf color.
The separate full-resolution transmission pass documented in
[AMBIENT_OCCLUSION.md](AMBIENT_OCCLUSION.md) protects those eligible W01 scenery
draws without moving them. It does not invent blade geometry for grass painted
into an opaque log texture.

The separate early-Last-Five-Turns report is not reproduced. An unforced
`cpu-soak` run completed rounds 1-3 and reached the normal night change at round
4 without invoking Last Five Turns. Native tests execute the actual scheduling
block for 10-50-turn matches and verify one ceremony at `turnMax-4`, independent
of the three-turn day/night counter. No event-timing workaround was introduced.

### Main tree and slide base (2026-09-07)

The measurements below precede the lawn lift; current contacts are recalibrated
against the raised lawn without moving the slide or tree platforms.

`gtao-tree-before-capture` / `gtao-tree-state` restore the same isolated state
at tick 7511, Strong AO, 1920x1080. The QA-only before switch disables only the
tree copies, retaining the log and foliage corrections on both sides.
`compare_tree_grounding.py` verifies all 145 logical positions are identical,
all 141 other meshes are unchanged, and only 52 supported bottom vertices change
across the four tree meshes. Their 690 other vertices stay unchanged.

| Mesh (object index) | Changed vertices | Gap before | Gap after |
| --- | ---: | ---: | ---: |
| `b01_m240` (1) | 38 | 2.156–26.353 | -0.100 |
| `r_ashi` (14) | 4 | 1.473 | -0.100 |
| `l_ashi` (8) | 4 | 1.463–1.469 | -0.100 |
| `b01_m240` (0) | 6 | 12.264–12.271 | -0.100 |

The matched image and JSON measurements are under
`build/board-qa-runs/gtao-tree-state/`. `gtao-tree-slide-verified` completed the
authored slide ride and subsequent turn with Strong AO, MSAA 4x and widescreen;
40 captured frames were reviewed and the slide/turn event gate passes.
Runtime tests cover the larger tree mesh, ancestor animation, source mutation,
moved/reused model identity, original geometry, live Off and scene teardown.

## Historical verification (2026-09-06)

The actor/guide gap reductions and space fitting below describe the removed
dynamic adjustment. They are not the current placement policy.

The test-only `ground_geometry_probe.h` captures submitted world-space vertices,
face topology, hierarchy and logical positions. `measure_board_ground.py` and
`compare_grounding.py` reconstruct support and check matched captures.

`grounding-before-matched` / `grounding-after-matched` restore the exact same
`grounding-verified-state` at matching ticks 7511–7512, Strong AO, 1920x1080
output with SSAA 2x. Only the QA build has a no-grounding comparison switch.

| Contact | Gap before | Gap after |
| --- | ---: | ---: |
| Luigi | 7.8031 | 0.3344 |
| Mario | 7.8425 | 0.5888 |
| Yoshi | 8.2814 | 1.1387 |
| Brighton right foot | 8.8649 | 0.5528 |

Units are world units above the highest supporting start/lawn triangle under
the lowest rendered vertex in this pose. These are geometric measurements,
not AO pixel intensity. All 75 submitted logical model positions are identical;
nine checked terrain/background meshes have exactly identical world vertices.
The native state/geometry files are isolated under ignored `build/board-qa-runs`.

- `grounding-bridge`: completed a real bridge-route turn; 60 captured frames
  include dice animation, walking on the moving bridge, capsule pickup and coins.
- `grounding-spring`: completed the spring flight and landing, followed by
  ladder descent and the turn end; 12 captured frames, no black-screen/crash.
- `grounding-live-fi`: same-state restore with live AO cycling and interpolated
  presents, clean guarded shutdown. This is a correctness test, not a benchmark.
- Native runtime tests run the actual grounding module with fixture geometry
  in graphical and headless modes. They assert untouched logical state, the
  full height of a jump, moving-platform exclusion, camera/shadow policy, live
  Off behavior, and scene exit. Math tests cover slopes, footprint bounds and
  degenerate/vertical triangles; topology tests cover original GX index order.
- Windows release and Android release native builds compile. Android hardware
  rendering has not been tested; no APK assembly/upload is implied.

Current regression commands: `python -m unittest discover -s tests -p test_*.py`
(57 tests) and `python tests/integration/test_ao_gpu.py` (16 GPU tests). Windows
Release and Android native Release include the complete background lift and
mobile shading budget. The published private 0.4.7 APK predates these two changes;
Android device rendering and a newer APK upload remain unverified.
