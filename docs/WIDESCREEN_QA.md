# Widescreen implementation and verification

Updated 2026-10-19 (local). All 32 compiled minigames received individual source and
rendered-keyframe review. This is not blanket certification of all 72 modules,
every camera state, AO, Android, or unlocked-frame interpolation. Native 4:3
behavior and original artwork remain the reference. Earlier evidence below is
historical and must not substitute for review of newly integrated scenes.

## Acceptance standard and reopened reports

### Option, Mic and Solo menus, Solo pause list, 23 October

Reported from the iPhone (19.5:9).

- Option Mode and Mic Mode: black upper corners beside the cloud frame.
  Both use the Party/Solo stage set: the `pillar` frame now continues with
  `mp6_widescreen_extend_pillar_edge()`, Mic's own `yuka` model gets the
  separate floor backing, and Option's `yuka` (shared with its flowers)
  extends its outer ring in place. Clean at 932x430 (`optiondll`,
  `mdmicdll` direct start).
- Solo Mode board select: the confirm warp reprojected the wide screen copy
  with the native 1.2 aspect, so the chosen board's picture sat in a 4:3
  window. Solo and Mic now use `1.2f * scale_factor()` as Party does.
- Solo pause minigame list: its two columns were widened by the scale
  factor while their 540-wide backing panel was not, so the right column
  ran past the panel. The columns are native size again.
- Board space-shape wipes (`bmasu` 7-9) grow uniformly by the scale factor
  so the captured screen covers the wide frame before shrinking.
- The sun/moon wipe from a board into the instructions screen and the
  instructions wipe-in were captured at 932x430 and fill the frame.

### Daft Rafts water reflection, 23 October

Reported from the Galaxy Tab S10 Ultra (16:10): the river's reflection of
the crates, logs, rainbow and cliff was stretched and sat beside the
objects instead of under them. The water (`m609dll`) registers a screen
copy with `Hu3DFbCopyLayerSet(2, 0, 0, 640, 480, ...)` and projects it
with the live camera, whose aspect is already widened, so the projection
spanned the wide view while the copy held only its left 640 pixels.

- The engine's projected copies (the `Hu3DFbCopyLayerSet` hook and the
  Hu3D water layer's display/reflection copies) now go through
  `mp6_widescreen_copy_view()`: a native full-frame request takes the
  current camera's whole viewport and scales it into the caller's
  native-size texture, as Tally Me Banana and Pier Factor already did by
  hand. Other rectangles, and 4:3, copy exactly as before.
- Reproduced at 768x480 and verified at 640x480, 768x480, 848x480 and
  1120x480, all CPU, one full round each: reflections line up under their
  objects at every width (before `20261023-020111-186306`, after
  `20261023-020352-506114`, `20261023-021005-383983`). The open 21:9
  bottom-left corner on the falls is unrelated and still open.

### Night skies in Jump the Gun and Mowtown, 22 October

Reported from the iPhone (19.5:9): Jump the Gun's top pane showed a black
void past the sky at the right end of the course. Swept every split-view
module day and night at 1120x480 with an edge-column black detector, then
reviewed the flagged frames; full fades and the native pane gutters are
the only other hits.

- Jump the Gun (`m643DLL`): the night set draws its sky with `grid1`, an
  untextured vertex-coloured gradient (x +-14028 at z -12569). Only the
  day's `643back2-1` was widened, so both panes ran off the night sky once
  their cameras tracked toward x +-2060 (top pane worst). `grid1` now grows
  its outer columns with `mp6_widescreen_extend_outer_x()`, which accepts
  untextured cards. Clean at 640/1040/1120/1440 x 480, day and night
  (receipts `20261022-022135-848399`, day `20261022-022229-384069`).
- Mowtown (`m613Dll`) night results: the night set names its wall, fog
  band and sky `Dwall`/`Dfog`/`Dbg`, so the day names were refused and 21:9
  showed black upper corners. They now get the day treatment (receipts
  `20261022-021840-274257`, day `20261022-022016-786957`).

### Daft Rafts, Blooper Scooper, Insectiride and Boo'd Off the Stage, 21 October

Captured with the unlocked tick rate and direct module start, all CPU, one
full round each at 640x480, 848x480 and 1120x480, reviewed against 4:3.

- Blooper Scooper: the opening's curved sky card (`sky00`) and cloud bank
  (`nyuudou`) end inside a 21:9 view, leaving black corners above the
  horizon, and their near ends left a dark band below the haze at the
  sides. Both scale with `mp6_widescreen_scale_backdrop()` about their top
  edge, so the extra height goes below the sea line; 4:3 is unchanged. The
  plain blue around the whirlpool is the native clear colour, as at 4:3.
- Daft Rafts: tilting down the falls, wide views looked past the left end of
  the cliff wall behind the waterfall (`obj2`) into the void. The wall now
  continues outward with `mp6_widescreen_extend_wall_outer_sides()`
  (repeating its brick texture). The rainbow mist card shows more of its own
  ring, which is the original artwork. Open: at 21:9 only, a corner of about
  ten pixels at the bottom-left still shows black for a few seconds on the
  falls; 16:9 is clean.
- Insectiride and Boo'd Off the Stage needed no change: their stage sets and
  the Insectiride monitor cover every width. Boo'd Off the Stage's white
  flash before the results is the native transition.

### Snow Brawl, Tally Me Banana, Mowtown and Pier Factor, 20 October

Reopened after the user rejected the first pass ("Widescreen fixes are bad").
Captured with the unlocked tick rate and direct module start, all CPU, at
640x480, 848x480 and 1120x480 (day) and 640x480/1120x480 (night), and
reviewed against 4:3 at full size.

- Tally Me Banana and Pier Factor: the water reflection copied only the
  native 640 pixels and projected it with a fixed 1.2 aspect, so wide water
  sampled a squeezed, partly empty copy. Both copies now take
  `mp6_widescreen_render_width()` and the projection multiplies its aspect by
  `mp6_widescreen_scale_factor()`; the water matches 4:3 at every width.
  Separately, both water discs drifted through grey, brown, blue, green and
  pink at every aspect. They are retail display lists indexing rodata
  arrays; display-list replay skips the native GXBegin size learning, so a
  bare `GXSetArray` uploaded zero bytes and the disc read stale GPU data.
  The arrays are now bound with `mp6_gxarray_bind_span()` and their real
  sizes; the water keeps one steady tint (receipt `20261020-122628-873263`).
- Tally Me Banana palms: ten loose fronds (`ha*`) hang in from an
  off-screen canopy to frame the 4:3 view, with matching ground shadows
  (`yasi_K*`). Wider views showed them floating mid-air. Their sway clip owns
  their position, so `mp6_widescreen_motion_x()` widens its X keys: shadows
  about the centred camera, fronds about a pivot 35% of the way out (the
  visible blade hangs inward of its pivot), which keeps them cut by the frame
  edge as at 4:3 (receipt `20261020-125956-469564`).
- Tally Me Banana DK plaza: its walkway (`cyl21`, plus the underwater wall
  in the second scenery model) runs off the right of the 4:3 frame and stops
  at local x=600, which wide views showed as a straight cut. At the user's
  request it now continues into a mirrored copy across that plane
  (`mp6_widescreen_mirror_mesh()`, faces re-wound so they still face out),
  turning the cut into the far end of a symmetric plaza (receipts
  `20261020-133309-851152`, 4:3 `20261020-133438-209196`).
- Mowtown results: the first pass zoomed the sky card, which blurred it and
  left the brick wall, fog band and back grass ending inside 21:9 (white and
  black wedges at the sides). The new `mp6_widescreen_extend_outer_x()` moves
  only a mesh's outermost X planes and continues each moved corner along its
  face's texture gradient. The REPEAT brick wall and fog continue at native
  density, the two CLAMP grass halves repeat outward on their outer sides
  only, and the soft-clouded sky widens its end panels (repeating it cut a
  visible seam through the cloud band). Interior geometry is untouched and
  4:3 restores native X, STs and wrap mode.
  The two brick flower beds (`plant`, `plant2`) end before their outermost
  flower clusters, which 4:3 never shows, so wide views had flowers floating
  over the paving. The same helper now picks the local axis nearest model X
  (the beds are authored lengthwise along Z) and lengthens each bed outward,
  capped at 400 units, repeating its brick (receipt `20261020-130936-542640`).
  Only the ten pink flowers inside the 4:3 view have stems (`kuki`,
  `kuki1`), so the outer blooms still hung in the air. The new
  `mp6_widescreen_add_stems()` gives every stemless head a copy of the stem
  piece whose height fits it best, placed under the head in model space
  (hierarchy transforms) with its tip 40% up the head quad so it hides behind
  the petals (receipt `20261020-132648-961821`).
- Snow Brawl: the sky wedge (`bg`) bends away below the village, so widening
  it horizontally alone exposed its lower corners. It scales about its top
  edge instead, keeping the gradient next to the village where it is natively.
  The village's own outer decks are authored geometry and stay as they are.

Receipts: day `20261020-121316-999350`, night `20261020-121503-262509`; every
round returned normally.

### Title backdrop without mirror folds, 20 October

The user rejected the title's mirrored margins: twinned balloons, houses and
stars were visible at each fold. The two backdrop quads in `TitleMdlId[2]`,
`grid5` (sky) and `grid14` (town row), now scale proportionally by the width
factor through `mp6_widescreen_scale_backdrop()`. The sky scales about its
middle and the town about its bottom edge, both about their own horizontal
centers, with 1% edge overscan so no frame-edge column is left uncovered.
Nothing is mirrored or stretched; the extra height falls outside the frame.
The cast, wordmark and copyright stay native, and 4:3 restores the exact
native vertices. Release captures at 640x480, 848x480 and 1120x480
(`build/decomp-update-20261020/title/after-*`) were reviewed.

### Sink or Swim, Rocky Road, Strawberry Shortfuse and Speak Up, 20 October

The four modules added at decomp pin `73a58c0b` were reviewed at 640x480,
848x480 and 1120x480 in the widescreen QA probe.

Sink or Swim (`m625Dll`) had three faults. Its underwater refraction copied
only the left native 640 pixels while the projective texture spanned the
widened camera, so the underwater view was offset and ended mid-screen. The
copy now downsamples the full live render width into the same native
640x480 texture; 4:3 copies exactly as before. The painted sky panorama
`bg` (four CLAMP segments of one half-octagon) ended just inside the 21:9
view, exposing the untextured red backstop `skyBG` and then the green clear
colour. `bg` now continues its outer edges with mirrored art, and `skyBG` is
widened horizontally so its forward ends stay behind that extension. Both are
static and collapse to native at 4:3. The refraction sheet `wave2` continues
its REPEAT ripple art at both sides. It is extended before the wave process
captures crest vertices, so the appended crest row also animates. Trees,
foliage platforms, ripples and gameplay coordinates are unchanged.

Speak Up (`micquizdll`) captured its quiz-board monitor texture from screen
X=0. The board's centered HUD sprites sit in the middle of a wider frame, so
the monitor showed a black band and clipped the right columns. The capture
now takes the centered native 640x480 window. The full-screen fade overlay
and scrolling pause backdrop span the live width; the backdrop keeps its
16-unit tile density.

Throw Me a Bone (`m650dll`) stages its new-record scene just past the native
edge: the Chain Chomp waits at x=+-700 and the bone is thrown in from +-550,
both ~170 units outside the 4:3 frame. The widened frame showed the Chomp's
face from the first wipe-in frame. `mp6_widescreen_offstage_offset()` (camera
layer) returns how far to push a point that is outside the native frame of a
live camera so it stays the same distance outside the widened one; it is zero
at 4:3 and for anything inside the native frame. The scene applies it once,
after the winner camera is posed under the wipe, and speeds up only the
bone's first throw so it lands where it does at 4:3. Gameplay positions never
use it. QA: `--winner-camera` on m650dll forces the record path for all-CPU
runs, `--no-offstage` shows the old framing.

Rocky Road (`m644dll`) and Strawberry Shortfuse (`m680Dll`) needed no change:
their split views, track, cliffs and kitchen walls cover every aspect.

Receipts (all CPU unless noted, isolated saves, exit 0, no reported errors):
day `20261020-021422-942820` (Sink or Swim and Strawberry Shortfuse full rounds
return at every size, 12 frames each); night
`20261020-015933-078647` (Sink or Swim and Strawberry Shortfuse rounds return
at every size); Rocky Road full CPU-3 round to its winner and return
`20261020-020945-019381` (20 frames at 1120x480); Strawberry Shortfuse 4:3
return `20261020-021018-774755`; Speak Up board after the fix
`20261020-020856-841665`. All frames were rendered and reviewed. The pause
backdrop and Speak Up's later question states were not reached by the
scripted input and are checked by source regression only.

### Mini-game Tour bus signpost early restart, 19 October

The signpost is MGMfree layer 5 (`null70`, with `grid228`, `cyl66`, `grid226`
and `grid227` geometry). Its authored motion lasts 1079 frames. The previous
widescreen code slowed that motion by `1/k`, but still decremented its restart
timer by one full frame per tick. At 21:9 it restarted at motion frame 617.716,
with its actual projected vertices occupying X=561.661..624.021 inside the
1120-wide logical viewport. This was a port timing regression, not culling.

Only the two corresponding motion countdowns (layers 5/6) now decrement by
the same `travelStep` as their playback. Native 4:3 remains exactly 1.0; no
motion keys, root positions, geometry, parallax speed or random decisions were
changed. Live aspect changes use the current motion step without restarting.

The opt-in QA probe transforms every signpost vertex through the actual object
and camera matrices each tick. Receipt `20261019-215844-238397` measures 4091
ticks per aspect and reviews 12 actual edge-exit frames each at 720x540,
960x540 and 1260x540. All seven observed restarts occur at frame 1079, with
leftmost vertices at X=909.241 / 1319.617 / 1856.264, beyond the respective
640 / 848 / 1120 viewport edges. Median visible movement remains
1.601745 / 1.601715 / 1.601714 logical pixels per tick. The old-clock control
`20261019-220142-914431` reproduces three visible mid-screen restarts, and its
12 frames visibly reproduce the disappearance. Its measurement verdict is
deliberately false; a successful capture receipt is not visual acceptance.

The 112-test focused package passes, including the real recovered update body,
both prop timers, live width changes, native behavior, layer-6 random skips and
the old-clock negative control. Normal Release was rebuilt with two compiler
jobs (SHA-256
`ad851e7838dc8648710f5b2c4ec5487060874713c62f78c70c845b62f0fbf4c9`).
Its smoke receipt `20261019-220406-831016` has two 21:9 frames, 3589 encoded /
composited XeGTAO strong passes, exit 0 and no reported errors. The actual-vertex
logger and old-timer override are absent from the shipped executable. The
long-loop verification belongs to the isolated QA build; the normal forced
overlay smoke has a 600-tick budget. No player saves or pinned sources changed.

### M675 no-fog geometry replacement, 19 October

The user rejected the earlier mist treatment. Its hook and draw code are now
removed, not merely given a lower alpha. The old `210512`, `210857`, `211928`
and `212128` receipts describe that superseded fog build, not current acceptance.

The finite sky and hill now reflect their own artwork. The second `obj24`
corniche repeats its six authored atlas/profile bands. The `bouha` rear curb
also continues both ends with independent atlas starts/ends, retaining block
joints instead of stretching concrete. Its actual 34 boundary edges require
272 tile strips; the former 64-strip capacity silently refused this mesh.
The bounded capacity is now 288, and the new two-sided sloped path uses each
side's outward sign. Grass extends independently; actors and town are not scaled.

Animated `umi` keeps all 55 native vertices, 40 native faces and six native
shape buffers. Appended reflected rows follow the exact evaluated shape after
ShapeProc, before drawing, including the stepped shoreline's per-row endpoints.
A depth-tested sea sheet beneath the waves uses the measured opaque S=.8..1
interval of the original `hama_umi` texture. S=0 is transparent shoreline and
was rejected as backing. The result has 1,317 vertices / 408 faces; there are
no per-frame allocations or shape-buffer count changes. Added geometry collapses
to zero area at native aspect and authored cull bounds are restored.

Blender 4.3 renders the actual exported evaluated water pose, with its real
decoded alpha texture, in `build/m675-blender-review/no-fog-final.blend`.
The calibrated vertical tangent/aspect are 0.414213546 / 2.100000143. Other
materials remain diagnostic colours; this is not a substitute for in-game
texture/animation review. Shape export is explicitly opt-in and has no timeline.

Receipt `20261019-225618-960106` contains ten intro/gameplay/Finish frames at
each of 720x540, 960x540 and 1260x540. All three native-AI rounds return normally.
Receipt `20261019-225740-397225` contains 18 active-play live resize/toggle
frames across all nine settled states. All 48 frames were rendered and reviewed:
sky, hill, ocean and curb cover the exposed edges without the fog mask. XeGTAO
strong encoded/composited 14,453 passes across these four runs; all exit 0 with
no reported renderer/crash errors. This is M675-specific evidence, not blanket
certification of other modules or arbitrary extreme aspect ratios.

The follow-up also fixes the four parked intro players exposed by widening.
Authored X staging points (-1300,-1000,1000,1300) are baselined and multiplied
by the live width factor at creation and only while waiting in entry phases
0/1. Walking, gameplay and results never restage them. The real entry targets,
native walk speed and all-player arrival barrier remain intact; a wider entry
can take longer rather than speeding up animation or popping models at 4:3.
Receipt `20261019-231101-445320` repeats all three aspects through normal return
with 12 frames each. `20261019-231404-187391` samples 18 entrance frames at
12-tick spacing: waiting players are absent, then walk continuously from the
actual screen edges. `20261019-231251-948504` adds 18 live resize/toggle frames.
All 72 frames were rendered and reviewed, with 18,013 XeGTAO strong passes and
no reported renderer/crash errors. The native fixture replays the actual
staging, registration and update gate, and checks all four points against the
measured opening frustum with a 150-unit character margin. Shared shape,
edge/cap, native baseline and Blender regressions are included in the package.
The final focused package passes all 114 tests.

Normal Release was rebuilt with two jobs (SHA-256
`16a34762dda615b5bea1683b8e8030080da4c8ee111688e718ff5053aa224657`).
Production receipt `20261019-233310-959793` adds two 21:9 frames, 599 XeGTAO
passes, exit 0 and no reported errors. The production forced-overlay smoke
has a 600-tick budget; full-round and live-resize evidence belongs to QA.
The pinned decomp is unchanged and all runs use isolated saves.

### M662 fence, cap joints and hill follow-up, 19 October

The Trick or Tree report is corrected in the actual scene, not just its pause
UI. Both brick walls continue their measured sloping profile offscreen. Each
new cap/brick segment repeats the original CLAMP-atlas interval with independent
ST starts and ends: mirroring the atlas erased the concrete-block joints and
was rejected. Shared profile endpoints use their own longitudinal spans, so
adjacent cap, bevel and brick faces stay welded at every tile boundary.

The complete five-rail profile needs 224.66 / 634.69 local units of additional
reach at 16:9 / 21:9. The explicit 1200-unit-per-extra-aspect registration
provides 390 / 900 units; using the native mesh span alone left a front edge
onscreen. Real-buffer tests cover both directions, end/start UV jumps, welded
multi-face seams, native preservation, failed allocation and repeated resizing.

The earlier floor extrusion was removed: its 18 appended quads created the
unwanted dirt behind the walls. The original 38 vertices / 34 faces, including
the interior paving, remain unchanged. Scene exports also confirm both walls'
15 original vertices / eight original faces and corner records are preserved
at all three aspects. Only the hill's `bg_field` X coordinates follow the curved
sky's mapping; Y/Z and the smaller `bmerge16` trees stay native. A mirrored-hill
trial intersected the convex sky shell and was discarded. Native-aspect hill
records match the original exactly; no depth/cull override or extra fog is used.

Final receipts are `20261019-182809-721485` (two paused frames per aspect),
`20261019-182834-298743` (ten native-AI intro/selection/ending frames per aspect,
all three normal overlay returns), and `20261019-182902-486297` (18 paused
resize/toggle frames across all nine settled states). All 54 frames were
reviewed: cap joints are visible, outside dirt is absent, and hills and walls
reach the exposed edges. All seven runs exit 0 without reported renderer/crash
errors; 16,301 XeGTAO strong passes were encoded and composited. The 104-test
focused package passes. This is M662-specific acceptance, not a new claim about
every minigame or physical microphone input.

Normal Release was rebuilt with two compiler jobs (SHA-256
`801718a751ba69f6effbe9d1beac722178fbcc4a69599ab9d570cf2a1fd80283`).
Production receipt `20261019-183308-855572` adds two reviewed paused 16:9
frames, 598 XeGTAO passes, exit 0 and no reported errors. QA pause, original-sky
and live-resize probes are absent from that executable. Saves are isolated;
the pinned decomp checkout is unchanged. The 18 October scenery acceptance
below is historical and is superseded by this follow-up.

### M659 native bottom-gradient repair, 19 October

The reported "white overlay" cutoff is an original asset defect, also present
at 4:3: Asteroad Rage's `642nightSKYa1` vertex-coloured sky grid ends at local
Y=-13804.753. At the opening course depth its middle edge projects to row
461.56/480, leaving 18.44 native pixels uncovered. It is not the shared pause
fade; the same finite grid is visible in both active and paused gameplay.

The repair appends edge-colour continuation down to local Y=-18000. The
measured worst bottom-frustum intersection is -17660.68 at 21:9 with lateral
ship motion. Existing vertices, faces, corner colour indices and animation
remain intact; camera framing, star textures and gameplay are unchanged. This
native-height repair deliberately stays active with widescreen disabled. Other
colour-rim users retain their previous aspect-only behavior.

Actual before/after pause receipts are `20261019-164816-656522` (QA-only
original-gradient negative control) and `20261019-164849-345584` (repaired).
Each contains two frames at 720x540, 960x540 and 1260x540. The sampled bottom
eight-row sky strips contain 1598/2211/2877 dark navy gap pixels before and
0/0/0 after; median native-aspect colour changes from (0,0,50), the scene clear
colour, to the authored gradient's (45,51,74). The sample excludes the divider
and central ship exhausts; it is not a whole-image similarity claim.

Receipt `20261019-165130-649945` adds ten active-play/Finish/winner-transition
frames per aspect. All 42 before/after and gameplay keyframes were reviewed,
with XeGTAO strong enabled. All nine runs exit 0 without reported renderer or
crash errors. The 101-test focused package passes, including the real-buffer
writer, repeated width/toggle changes, original gradient preservation, measured
frustum coverage and an isolated old-behavior control. This closes the M659
bottom-band report, not every minigame's effects or scenery.

Actual scene-export comparisons also confirm all 25 original vertices and 16
original faces/corner colours are unchanged at all three aspects. Normal
Release was rebuilt with two jobs (SHA-256
`a14526e38de40e1d6bc564d89010bffbccfc3e51d09080d8c1dc69a7a0f6766c`).
Its separate smoke receipt `20261019-170153-907805` captures two paused 16:9
frames from that production binary: both reviewed with the strip absent,
2,393 XeGTAO passes, exit 0, no reported renderer/crash errors. QA-only pause
events and the original-gradient control are absent from the release binary.

### Solo results / Star Bank follow-up, 19 October

The reported results dimmer and purple Star Bank backing were separate finite
sprites. Only their flat tint layers now opt into live-width drawing; the
results frame, text, earned-star model and Star Bank picture are not stretched.
Both Solo sprite-loading paths and both shared save entry points are covered.
The matrix fixture checks native/disabled byte preservation, untagged artwork,
both exposed wings, repeated aspect changes and the old narrow negative control.

Actual completed-board return (event 1, legal 20-coin / one-Star state) was
captured, not the Solo menu. Receipts `20261019-161711-972641` (results) and
`20261019-161732-929040` (shared Star Bank award) each contain three keyframes at
720x540, 960x540 and 1260x540 with XeGTAO strong. All 18 frames were reviewed:
the dimmer covers the whole screen and the purple backing exposes no ships.
All six runs exit 0 with no reported renderer/crash errors. Cards are isolated
copies of attested `wide-create-20260930e-640x480`, never the player's saves.
Reproduce with `--solo-results --scenes mdsingdll --card-seed
build/filesel-qa-runs/wide-create-20260930e-640x480 --capture-trigger
qa.mdsing.result-ready=ready` (or `qa.mdsing.bank-ready=ready`), `--delay 3
--stride 60 --count 3 --startup-slack 2400 --ao 2 --implementation xegtao`.

The focused package now passes 100 tests. Normal Release was rebuilt with two
compiler jobs; SHA-256 is
`5db614fd1cd3329a93b4a694fcc36a239737ed4edda142a756a789be727f4142`.
The QA seed/probes and scene exporter are absent from that executable. This
acceptance is limited to these two reported layers, not all Solo-board scenery.

### Active follow-up, 19 October

New screenshots supersede earlier "clear" labels for the affected states:

- S01 canyon: left wall termination and both floor joins remain visible.
  Whole-scene stronger fog was rejected; the native 4:3 area must not be tinted.
  Side-only haze is still a prototype, not visual acceptance. The opaque
  half-margin plateau was removed: the full margin now eases from alpha 0 to
  88/255 in eight connected bands. A real-body draw fixture checks opacity,
  native-canvas exclusion, joins and repeatable resizing. Three-aspect receipt
  `20261019-135142-351653` still exposes geometry gaps; fog does not certify them.
  The upper sand floor's boundary vertex alpha is zero, so faithfully extending
  that border cannot supply an opaque backing floor.
- M635: exposed light/dark field join remains open; the X-only and larger-rim
  trials did not improve it and were discarded.
- M662: side fences, concrete-cap joints, outside dirt and hill silhouette are
  resolved by the scene-specific follow-up above.
- M675: the rejected edge mist has been removed. Animated sea, sky, hill,
  corniche and rear-curb continuation are covered by the no-fog follow-up above.
- M659 pause/active white-overlay report: resolved as the native sky-gradient
  cutoff above, not a shared pause-window tint. Other pause effects are not
  blanket-certified by this scene's correction.
- Solo Mode menu: cloud-frame edges, reflected rotating-disc rim and a separate
  clean-plank floor backing now continue the scenery without enlarging ships,
  characters, cards, the floor emblem or its native shadow receiver. The floor
  crop uses measured rows 192..255 of `ys78_floor`, not an assumed empty area.
  Receipt `20261019-145111-288610` covers intro/character/settings/launch states
  at native, 16:9 and ultrawide with XeGTAO; `145452-487265` samples another
  5,000 ticks of the rotating background. The reviewed 100-pixel side strips
  contain 0 near-black gap pixels versus 67,026/108,000 before the fix. Native
  source surfaces remain intact; appended rims collapse and the floor backing
  is hidden at 4:3. The three native keyframes (0/2/3) are pixel-identical to
  `20261019-135142-351653`. This menu acceptance does not certify its three boards.
- W05: the displaced player is fixed, but its ultrawide overview exposes finite
  sky corners. That separate scenery issue remains open.

Two independently tested gameplay/lifetime fixes are retained locally:

- Missing-minigame fallback releases the specific preloaded archive before
  returning. `build/unavailable-archives-after/run` completes 32 distinct real
  archive handoffs; DVD usage after each return is 1,095,264 bytes. The negative
  control detects a retained archive before the destination menu can close it.
- Castaway Bay's ship setters use named `W05_BOARD_WORK` fields. Fixed PPC
  offsets left the 64-bit second-ship player field at zero, repeatedly placing
  player 0 in the cannon. The real-body fixture rejects the old setters and
  verifies all four new setters preserve every unrelated byte. This explains
  Mario's apparent displaced spawn; it is a port ABI bug, not an intended role.
  Receipt `20261019-132103-623521` shows all four starting players with XeGTAO
  enabled at native, 16:9 and ultrawide ratios.

The QA-only Blender live-scene exporter is locally committed as `04db1b9`.
Capture with `run_widescreen_scenes.py --export-scene`, then import using
`blender --factory-startup --background --threads 2 --python tools/blender_scene.py
-- --snapshot <scene.json> --output <scene.blend>`. Blender 5.2 successfully
opened actual S01 (`20261019-133455-745496`) and M635 (`132716-780991`) snapshots;
S01's runtime camera direction/up were also checked in Blender before rendering.
The tool preserves static/live-sampled geometry, UVs, corner colors and transforms,
with duplicate-safe model/object identities. It does not export texture pixels,
animation timelines or skinning; unsupported objects are explicit markers.
Blender 4.1 is not validated. Production builds do not include the exporter.

The preceding follow-up package passed 97 tests, none skipped. Normal Release was
rebuilt with two jobs and the existing generated-disc includes. Its SHA-256 is
`6f8033070e394771d363a7e809230fa41f86953ff808265432a42be9245f0d4c`.
Production receipt `build/production-followup-qa/20261019-150343-572420` exits 0,
captures the actual Solo menu, executes 599 XeGTAO passes, reports no renderer
errors, and leaves the player's save directories unchanged. All commits remain
local; no push or publication was performed.

### Remaining-module sweep, 19 October

The other 24 compiled minigames now have ultrawide XeGTAO-on captures and
normal overlay-return evidence. Intro/play/later frames were inspected; this
does not certify every input, night variant, winner animation or camera.

| Minigames | Initial capture receipt under `build/widescreen-qa/runs/` |
| --- | --- |
| M602/M608/M612/M616/M621/M629 | `20261019-062717-731657` |
| M631/M633/M635/M636/M637/M646 | `20261019-062948-767605` |
| M649/M650/M651/M656/M657/M658 | `20261019-063245-093087` |
| M659/M662/M667/M668/M670/M675 | `20261019-063429-875764` |

Longer completion repeats are `20261019-063829-066793` and
`20261019-064102-115029`. Together with returns in the initial runs, they cover
all 24. Sparse winner samples with spinning/entering text are not settled-banner
approval. M635's newly exposed distant light/dark art boundary remains open.

All 13 mode owners and 11 board owners also received AO-on capture runs, not
blanket visual approval. Modes: `064858-789437`, `070644-362138`,
`072256-100634`, `072850-976980`; boards: `072403-325284`, `072547-355318`
(all prefixed `20261019-`). Native-ratio controls: `073158-302967`.
The captures found and led to these corrections:

- Solo settings used packed PPC byte offsets for difficulty/pack. Named
  `GW_COMMON` fields fix its real texture-binding crash. The repeated AO-on run
  passes; a real-body test covers all 20 legal difficulty/pack pairs.
- Battle/Bingo preview ownership now handles empty/failed allocations and restores
  the original image. Their 284x256 offscreen capture stays native-sized and
  reads from the centered native canvas after THP is ready. Real-body tests cover
  empty/capacity/valid cases, draw termination and wide source offsets. A visible
  movie-preview animation loop has not yet been verified.
- Mic/Solo feedback capture reads the full live framebuffer into its original
  320x240 allocation; the photographed 2D trail is outside AO. Resize tests execute
  the real callback bodies. The Solo hook has no current enable caller, so its
  correction is not a claimed visible effect improvement.
- W03/W04/W05 lighting/event checks now use `GwSystem.curTime`, not the PPC
  bit position in its containing byte. Actual-body tests vary `nextTime`
  independently and verify both day/night routes.
- S02's native landing requests `omVibrate(player,20,0,0)`. Host modulo-zero
  crashed in `PadReadVSync`; the zero-period pattern now retains the initial
  motor command until its original cutoff. The normal repeating pattern is
  unchanged and both paths run under UBSan. The failing landing now passes in
  `074922-461505`.
- Clockwork Castle's cloud-wall continuation removes its opening side wedges
  (`074922-461505`). Native/16:9/ultrawide opening and later board frames were
  inspected in `082220-636704`.
- Factory upper wall borders now cover the exposed top corners. Its lower
  exterior wedges remain open; tested floor candidates did not resolve them.

Direct QA launch prerequisites are explicit and isolated from production saves:
native Party/Solo board initialization, native unlock calls for only the 32
compiled games, and legal Party rules for results. Battle with an empty fresh
card and results with `turnMax==0` were invalid forced admissions. Their later
faults were addressed in QA setup, not by replacing gameplay or clamping the
results bank. Results now complete and return at 4:3/16:9/21:9 in
`075731-305385`; this is a zero-score rules fixture, not a full played match.

Still open: Thirsty Gulch's opening wedges, T Minus Five's layered sky wedges,
Infernal Tower's complete sky/fade-layer border, the factory's lower exterior
corners, M635's distant art boundary, and deeper mode play/results. Canyon
duplicate-mesh and rocket side/lower-rail candidates did not improve the rendered
defect and were discarded. Infernal Tower's asset-4 sky continuation filled black
wedges but exposed dark fade-plane joins; extending both fade planes still left
visible seams (`083110-582467`, `083312-785693`). That candidate and its extra
geometry APIs/tests were discarded too. These captures are rejected evidence,
not approval of the retained build. Mode captures cover
menus/setup/transitions (TT Wars reaches real piece creation), not completed
Mic, Bingo, Decathlon, Battle or TT Wars sessions. Do not call these modules
fully audited from the capture-count receipts. No Android validation was run.

The final focused package passes 74 tests. Its first run found a stale generated
Android renderer source cache lacking the already-retained patch 0085; applying
that existing queue entry refreshed the desktop test fixture. No Android binary
was built or device tested. The decomp checkout remains unchanged.

The retained release executable rebuilt successfully with two compiler jobs
(75 units rebuilt, 347 unchanged, zero failures). Its SHA-256 is
`4bafff664af0fb87096f110eacd79d4f64495a2ae1f6ab76cf3e38e7f6b75b75`;
provenance is in `build/release-smoke/20261019-remaining-modules/`.
Production-executable Solo/Mic smoke captures in `20261019-083744-089270`
exit cleanly with XeGTAO active (599/644 composites). These are smoke checks,
not full mode-play or feedback-effect approval.

A launch, full framebuffer, or successful capture receipt is not visual approval.
Review native 4:3 against 16:9 and ultrawide at the important intro, active-play,
edge/lifetime, winner/merge, and return states. Check artwork joins and texture
density, complete UI exits, shared split endpoints, and actual camera-side
visibility. Measure repeated geometry and scrolling rather than judging speed
from isolated stills. Keep source geometry, actors, collision, and native framing
unchanged. AO and unlocked replay are separate checks, not inferred passes.

User reports reopened several initially "Clear" states below. Early gift-stage
trials filled black space but introduced a shading join or duplicated baked prop
shadows; those results were rejected. Their evidence remains under
`20261018-232602-985539`, `234617-491683`, `234910-446678`, and
`235630-733482`, not as approval of the retained implementation.

### Current corrections

- M618: the fourth course uses the actual ten animated grid rows, not a plain
  edge-color fill. Reflected copies meet halfway through the authored 14.923-unit
  grid gap. The previous pane-edge reflection doubled one column and was rejected.
  In `20261018-222344-740645`, rendered ultrawide stripe centers measure
  66, 162.5, 258.5, 355, 451.5, 547.5, 643.5, 739.5, 835.5, 932, 1028.5,
  1124.5, 1221 pixels: every pitch is 96–96.5 pixels, including the joins.
- M627: a material-2 brick-wall companion fills the upper corner wedges;
  material-3 floor extension remains separate. Full-resolution room and split-play
  frames were inspected in `20261018-224911-750553` at 16:9/ultrawide.
- M638: the camera update now uses the real scene owner's sprite slots 1/2.
  Slot 0 is the stationary center rail, not a team marker. Native-body tests cover
  progress and endpoint clamping; `20261018-224911-750553` shows the aligned rail.
- M640: the native merge stopped at frame 59. Frame 60 now empties the losing
  scissor and finishes the winning viewport. Real CPU winner captures at all
  three aspects, including frames after completion: `20261018-225904-170663`.
- M630: only the ten QUADs at the actual open path end-cap are reflected across
  its oblique XZ edge. Closed rings, source faces, collision and UV density are
  unchanged. The first actual-asset attempt refused because packed S8 normals
  were incorrectly read as F32, then because their magnitude was assumed to be
  one. The retained guard checks direction and reuses the source normal indices.
- M632: an independent frozen companion adds curtain/trim/floor margins, leaving
  the original skinned owner untouched. HSF-wide normal-format counts and baked
  per-mesh matrix metadata are retained; only the private copy's animation is
  detached. Curtains reflect; all floor layers continue their CLAMP edge shade,
  avoiding mirrored present shadows or a hard diagonal join. This is deliberate
  edge continuation of finite artwork, not newly painted floor detail.
  Native/16:9/ultrawide opening and later play/return frames were inspected in
  `20261019-004604-651559`; all three complete XeGTAO-on matches returned normally.
- M643: ultrawide review exposed finite sky wedges in both play panes. The
  authored REPEAT sky quad now continues sideways, while the render-only
  platform window uses `1300 * liveScale`. Physics/search thresholds are untouched.
  Real callback tests retain strict native and wide boundary behavior.
  The corrected native/wide panes were inspected in `20261019-003346-630152`.
- Instruction UI: horizontal exit distance and parked windows include the live
  widescreen margin. The actual start/slide sequence, not just Back/fade, was
  inspected in `build/inst-qa/runs/20261018-230658-918469`.
- Bus: signed passenger cutouts are clipped after widescreen placement, avoiding
  unsigned/native-width clipping. Transition evidence is in
  `build/mgmfree-qa/runs/20261018-230815-697950`. Travel is measured separately
  below; it is not inferred from these images.

The final M627/M630/M638 XeGTAO-on comparisons at all three aspects are in
`20261019-004251-750008`; native and widened important frames were inspected.
An additional ultrawide source-driven sweep of M629/M637/M643/M650/M657 is in
`20261019-001701-799842`. It found the M643 defect above; no background cutoff
was observed in the captured daytime states of the other four. This does not
certify their untested night/input/camera states. The focused renderer, crash,
native-body and widescreen regression package passes 60 tests.

### Measured bus movement

At equal native viewport height, before correction the two widened animation
layers travelled 1.325 times native speed at the measured 16:9 canvas. Their
projected native-viewport speeds were 1.910079 vs 1.441569 and 1.398788 vs
1.055689 pixels/tick. After correction the corresponding 16:9/ultrawide values
are 1.441568/1.441583 and 1.055662/1.055664. Scripted layers 7–9 likewise return
from 0.531718 to approximately 0.40129 native world units/tick. Three already
correct continuous layers were left unchanged. These are coordinate-domain
measurements, not raw physical-window pixel rates.

Baseline and corrected bus evidence: `20261018-214126-059657`,
`214000-813532`, `214634-773729`, `214653-657899` under `build/mgmfree-qa/runs/`.

### Crate and Peril performance and crash follow-up

At the saved desktop-quality settings (2560x1440, VSync, 16x shadows, 2x SSAA,
unlocked rendering), the slowdown reproduced with AO off: steady game cadence
was about 45–47 ticks/s, rendering about 55–60 frames/s, with roughly 20 ms in
frame-end waits. The renderer's one-entry offscreen cache alternated between the
240- and 192-base shadow sizes, recreating large color/depth targets each tick.
A subsequent run failed allocating `Offscreen Depth` with a D3D12 out-of-memory
error. This is distinct from the native M664 stack overwrite.

Patch 0085 retains a bounded two-size LRU working set; quality/overlay teardown
still clears it. Empty camera-specific shadow views also skip unused passes,
while the single shared-map path and last-pass background clear are preserved.
The real extracted cache test checks allocation counts, eviction, and teardown.
Unchanged-quality corrected runs reached 60 ticks/s and approximately 238–240
rendered frames/s; steady frame-end waits were about 0.24 ms with AO off. The
XeGTAO-on repeat also maintained 60 ticks/s and approximately 240 frames/s,
with 14,527 AO composites. This is Windows evidence, not an Android claim.

Timing evidence: before `20261018-231642-616087`, failed old-cache run
`232641-122000`, corrected `234124-809989` and `235145-788792` under the
widescreen run directory. Each retains the settings, executable hash and logs.
The final geometry repeat, `20261019-004908-293576`, again reached steady
60 ticks/s and 240 presents/s with 14,607 XeGTAO composites. Its startup windows
include loading stalls (one 51.855-tick/s window); steady-state rates are not a
claim of stutter-free loading.

M664's collision helper now receives a complete `MGACTOR_COLMAP_POLY`, not a
one-word output buffer that overwrote the stack. Three post-fix full-return
runs at 4:3/16:9/ultrawide passed in `20261018-235235-077121`, in addition to
the native ABI fixture. Star Bank's missing `mdbankdll.rel` now follows the
existing unavailable-overlay return instead of a DVD panic; it is **not** a
recovered or integrated Star Bank implementation.

## Current 32-minigame review

The initial sweep uses isolated, hidden Windows runs, original assets, native AI,
720x540 (4:3) and 960x540 (16:9), AO/AA off, and no physical controller input.
Contact sheets and important full-resolution frames were inspected; successful
launch/capture counts alone are not a visual pass. Short games received denser
captures so their gameplay was not confused with the subsequent Party menu.
"Clear" below means no defect observed in the stated captured states, not a
full-playthrough or all-aspects guarantee. Menus after return do not count as
minigame evidence.

| Module | Widescreen applicability and inspected states | Outcome / evidence |
| --- | --- | --- |
| m602 | Native card-atlas cameras, widened display camera, shelves, card reveals, scores | Clear; A. Return not reached in this budget. |
| m608 | Full scene, snowboarding, prompts, scores and result | Clear; A |
| m612 | Four camera panes, crossbars, active play and transitions | Clear; A |
| m616 | Full scene, platforms, GO, play and winner | Clear; A |
| m618 | Four courses, shared timer and split scissors; real winner expansion | Corrected animated grid spacing; see current corrections. B/J are historical. |
| m621 | Desert scene, all player HUD panels, play and finish | Clear; B |
| m627 | Intro room, pinball table and changing split views | Fixed exposed floor wedges; B before, K after. Return not reached in this budget. |
| m629 | Balloon pumping, inflation, Finish and winner | Clear; B, contact-00 (contact-01 is mostly returned menus) |
| m630 | Multi-lane play, countdown, ground/backdrop edges and winner | Open path cap reflected and reviewed on actual assets; see current corrections. |
| m631 | Establishing camera, platform play and winner; day/night backgrounds | Clear; C and L |
| m632 | Opening stage, box cutaway, tilted arena, play and return | Corrected performance and layered border continuation; three final AO-on full returns. |
| m633 | Circular arena, room margins, play, Finish and winner | Clear; C |
| m635 | Garden, button callouts and winner; day/night outer terrain | Clear; D and L |
| m636 | Paired play areas and score monitors | Clear; D. Native-aspect return was outside the initial budget. |
| m637 | Reels/carousel, score bars and winner | Clear; D |
| m638 | Race lanes, split cameras, outside prompts and finish | Center rail/marker alias corrected; see current corrections. |
| m640 | Factory machinery, floor/wall borders, two play panes and finish | Real final winner merge corrected and inspected at all three aspects. |
| m643 | Full-scene intro, two play panes, moving cannons/targets and winner | Reopened ultrawide sky/culling defects corrected; current corrections supersede F. |
| m646 | Target board, four-player HUD, projectiles and active play | Clear; F |
| m649 | Stamp By Me: full-view intro demonstration and four play panes | Fixed exposed checker-floor corners; F before, K after |
| m650 | Four camera quadrants, moving obstacle course and timer | Clear; F |
| m651 | Vortex intro, active hazards, Finish and winner sky | Clear; G and M |
| m656 | Two space views, divider tiles, markers and finish | Clear; G |
| m657 | Two descent views, individual timers, Finish and winner view | Clear; G and M |
| m658 | Launch-area introduction, moving players/effects and ending | Clear; G |
| m659 | Two ship views, timer, active hazards and full winner camera | Native bottom-gradient cutoff repaired; current follow-up supersedes H/M for that defect. |
| m662 | Forest intro, active selection scene, timer and ending camera | Clear under CPU control; H and M. Physical microphone input not tested. |
| m664 | Raft/boat intro, moving cave camera and obstacles | Native stack overwrite fixed; three full post-fix returns passed. H/M/N retain pre-fix failures. |
| m667 | Cloud platforms, active targeting, timer, Finish and three-player winner | Clear under CPU control; H and M. Physical microphone input not tested. |
| m668 | Lava, cave borders, moving bar/effects, platforms and winner | Clear; I |
| m670 | Fruit platforms, vertical camera changes, HUD and ending camera | Clear; I |
| m675 | Beach intro, shore/sky coverage, moving players and score HUD | Clear; I |

Evidence keys are local build receipts, not committed image assets. Widescreen
scene runs live under `build/widescreen-qa/runs/`; each scene-size directory has
`result.json`, `game.log`, `.mfd` frames and rendered PNG/contact sheets.

| Key | Run identifier |
| --- | --- |
| A | `20261018-200331-977759` |
| B | `20261018-200557-391099` |
| C | `20261018-200803-857367` |
| D | `20261018-201102-744293` |
| E | `20261018-194712-541926` (m640 pair; interrupted runner before final m643 receipt) |
| F | `20261018-194903-718692` |
| G | `20261018-195205-849180` |
| H | `20261018-195721-008745` |
| I | `20261018-200033-001678` |
| J | `20261018-202519-387032` (event-triggered real m618 winner, 4:3/16:9/21:9) |
| K | `20261018-205822-402245` (corrected m627/m649, 4:3/16:9/21:9) |
| L | `20261018-203458-427629` (m631/m635 night, all three aspects) |
| M | `20261018-201932-396286` (dense short-game captures) |
| N | `20261018-202715-470499`, `20261018-202729-092424`, `20261018-202738-282642` (m664 4:3 repeat: one full return, two access violations) |

### Corrections found by review

- M627: material 3 (`yuka`) of `stage_si13` is sloped room flooring, mixed with
  unrelated wood/brick meshes. A material-filtered form of the existing terrain
  helper appends only its two extreme-X boundary quads. Census comparison with
  `20261018-201543-611906` retains all 536 original face/vertex/UV/color rows
  identically at all three aspects; only two border faces are added. Native
  aspect collapses the added margin. The table, obstacles and collision stay native.
- M649: only resource 5's `grid1` checker-floor boundary gains mirrored border
  panels using the existing terrain helper. Its central pedestal child,
  conveyors, players, prop positions, camera animation and collision are unchanged.
  The single-view intro demonstration is explicitly inspected, not inferred
  from the four-pane gameplay view.
- Shared instruction preview: a fixed EFB rectangle captured a horizontally
  shifted movie sprite after the 2D canvas widened, producing the black left
  section. `instDll` now reuses the existing decoded RGBA8 movie-copy adapter
  instead, with matching texture allocation/loading. The redundant off-screen
  movie draw is hidden; decode continues independently. This adds 128 KiB to
  the active 256x256 preview buffer but removes its per-frame EFB copy and draw.
  No FPS improvement is claimed without measurement. Animation and coverage
  were visually checked on Same Is Lame at all three aspects and specifically
  Stamp By Me at 16:9. Instruction evidence lives under `build/inst-qa/runs/`:
  `20261018-210247-674951`, `20261018-210253-202378`,
  `20261018-210258-577320`, `20261018-210857-217842`.

The historical m664 access violation was a native collision-output ABI error,
not a widescreen-camera failure; see the fix and full-return evidence above.
Other open visual states still prevent any 32/32 completion claim.

The full shared-camera regression sweep also found stale per-overlay widening
in `actmanDLL` and `sequencedll`. Their setters now receive native aspect/640
coordinates like every other caller; the central camera mapper applies the
wide transform once. This is a contract fix, not a claim that those support
overlays received this minigame visual audit.

## Rendering rules

- Hu3D camera setters accept native screen coordinates. The port maps horizontal
  projection, viewport and scissor once. FOV, vertical framing, camera motion,
  near/far planes and gameplay coordinates are not widened.
- Split-screen edges share rounded endpoints. Resizing and returning to 4:3
  rederive from the native camera values, not previously widened values.
- Clip rectangles are bounded before scaling. Negative inset sizes wrapped
  into unsigned values become empty, so a collapsed losing view cannot paint
  over a winner's expanding view. Valid vertical camera framing is unchanged.
- Framebuffer effects capture the full render width into their original-sized
  buffers. Changing the source rectangle must not enlarge an allocation silently.
- Scenery extensions must leave actors and collision alone. Prefer added border
  geometry and native-density tiles over stretching existing scenery.
- Model teardown explicitly clears widescreen registrations. Model IDs and arena
  addresses can both be reused; pointer equality alone is not a lifetime check.
- Backdrops register their native baseline even if widescreen starts disabled.
  Enabling later extends them; disabling restores native geometry and authored
  culling bounds. Companion models stay hidden at native aspect. Restoration
  runs once, not as repeated native geometry work every frame.

## Retained changes

- Game/board wipes separate their full-width capture source from their fixed
  320x240 texture destination. View-shift and wave captures no longer stop at
  the native left640; the wave quad covers the live canvas. Image-mask emblems
  retain native proportions and center while their outer masks extend. The save
  banner keeps its native size and right margin. The rendered sweep exposed
  an ANM ownership crash in star-wipe cleanup: decoded animations now release
  through their sprite owner, while captured raw images keep ordinary heap
  cleanup. Native contracts and all 13 game variants/five board capture styles
  pass at all three aspects. Board ANM masks also pass: the circle now grows
  uniformly with the viewport diagonal so its corner-clearance timing matches
  native. Bowser/Koopa mask artwork retains native proportions.
- Towering Treetop extends the extreme left/right borders of three background
  terrain meshes and their distant backdrop. Fourteen appended quads fill the
  outer log-space cameras, night Boo-house view and ultrawide overview. Native
  geometry/UVs/faces stay byte-identical; interior river banks and seams are
  excluded. Registration precedes AO's ground snapshot. Actual day/night asset
  prefix checks, 4:3/16:9/21:9 camera captures and an AO-on ultrawide sweep pass.
  See `build/board-qa-runs/treetop-border-*`; eight-space coverage uses the
  explicit `audit-wide-edges` camera fixture, not natural routing proof.
- Fixed-size render-to-texture cameras now explicitly retain native projection,
  viewport and scissor coordinates. On-screen cameras still widen. The opening
  storybook switches modes around its page-content pass; this fixes shifted,
  missing characters and the cropped grayscale transition without changing the
  640x320 page buffers. M602's three 192x288 card views similarly stay inside
  their original 576x288 atlas while its fourth, on-screen camera widens.
- M602 extends both audience-shelf layers past the exposed front ends and adds
  mirrored floor margins. Original shelf/floor surfaces, player podiums and
  texture scale remain unchanged; the shadow-receiver layer is included.
- M635 appends 20 outer grass-border quads in both day and night variants,
  removing exposed black ground gaps. Its distant terrain gains 34 side quads
  per variant. The original garden, houses, floor, terrain faces, colors and UVs
  stay unchanged. The original contrast between the two grass layers remains.
- M638 keeps both teams' button callouts at their original outside margins.
  Initialization and frame updates derive from native coordinates, including
  after resizing or returning to 4:3; the central progress gauge is unchanged.
- M657 anchors each timer to its own split-view center. Digits, separators and
  score-box backgrounds move together without scaling, preserving native offsets
  and updating on resize even after the timer stops.
- Shared camera mapping, including animated split views and scene reentry.
- Full-width file-selection and water captures; full-width M668 screen effects
  with unchanged texture allocations and particle proportions.
- Burnstile (M668) extends the cave and its outer shoreline together. Compact
  disconnected rocks translate without scaling; their lava rims receive the
  exact same translation from the rock's own centre. The two playable platforms,
  centre pillar, their three contact rings and collision models stay native.
  Lava planes gain side strips at original texel density (mirrored CLAMP art,
  continued REPEAT UVs), rather than stretching the original lava surface.
- Shared sprite/score-box clipping and centered board-map projection.
- M651 and M656 sky coverage; M656's divider uses additional native-sized tiles,
  keeping progress markers and the finish marker unchanged.
- M659's real winner camera exposed triangular side gaps in its untextured sky
  gradient at 21:9. Sixteen appended edge-color panels fill those gaps; all 16
  original gradient faces, their vertices/colors, the star layers, planets,
  ships, animated transforms and camera height remain unchanged.
- File-selection's Select Data badge now parks outside the widened camera during
  name entry and refreshes that position on resize. The on-book placement,
  native trajectory, return animation and badge geometry remain unchanged.
- Tiled sprite bands retain their original group ID, transform and matrix
  address when resized. Equal-priority sprites are ordered by group, so replacing
  M656's track group had incorrectly moved the track over its portrait markers.
  The helper now swaps only the owned member list; priorities and original
  sprite records stay unchanged. Rollback and retired-tile cleanup are retained.
- M640 adds floor/wall boundary strips without enlarging the machinery or actors.
- Crate and Peril (M632) continues its sloped box walls beyond their outer rim.
  Twelve appended panels follow the original wall directions and mirror the
  CLAMP texture at native density. All 372 original faces, floor, obstacles,
  collision, animation and the hand-held box remain unchanged. This fixes the
  black corner exposed by an ultrawide view while the box tilts.
- M630 fills its missing floor margins by extending only the opaque ground
  material and the original-height translucent tint above it. The mixed mesh's
  decorative paths remain untouched. Its curved backdrop gains two side panels;
  the original curve, foreground scenery, actors and collision do not stretch.
- M633 fills the 21:9 room corners with an appended floor rim. Its original
  vertices, UVs, arena, machines and collision remain unchanged. The rim mirrors
  the border texture at native density and collapses at native aspect.
- M618 extends five brick-wall boundaries plus the second/fourth course
  background margins. Independent variants render only in the corresponding
  player's course camera; original instances remain available to other views.
  Pipes, platforms, actor motion and collision are unchanged. Three untextured
  backdrop rims continue native edge colors without stretching the original
  gradient or artwork. At 4:3 the original instances are used.
- M618's winner transition exposed a shared camera clipping defect: a losing
  view's `-4` inset size became `4294967292`, then overflowed during horizontal
  scaling. Camera scissors now clip these invalid extents to zero after all
  four native assignments. Tests cover all winner quadrants and 61 transition
  steps at each aspect ratio; the observed bottom-winner strip is gone.
- Background centers are computed from transformed vertices, not cumulative
  culling boxes. Rotation and hierarchy are covered by native tests.
- Explicit model-kill cleanup, duplicate-floor slot recycling and a regression
  fixture reusing exactly the same model and arena addresses 100 times.

## Evidence and limits

All paths below are relative to the port checkout. Captured frames are engine
frame dumps, not desktop automation. Tests run muted with isolated save/settings
directories, AO and AA disabled unless a scenario says otherwise. Current
Windows runners additionally require a background-capable binary: the game
window stays hidden and physical controller/joystick/haptic initialization,
reads and motor commands are disabled. Scripted virtual PAD input remains
available. Older binaries are rejected before launch; normal play is unchanged.

| Area | Evidence | What is proven |
| --- | --- | --- |
| Shared camera/effect code | `tests/test_widescreen_camera.py`, `tests/test_widescreen_effects.py` | Native fixture checks for mapping, resize, ownership, bounds and captures |
| Mode-select reentry | `build/widescreen-qa/runs/20260929-002336-914645` | 4:3, 16:9 and 21:9 capture runs pass; 21:9 sky inspected |
| Minigame Mode | `build/mdmini-qa/runs/20260929-001519-358190` | 21:9 player/character/mode navigation, exit/reentry and unavailable-mode return pass |
| Minigame Mode 4:3 | `build/mdmini-qa/runs/20260929-002519-931759` | Native-aspect lifecycle passes; frame inspected |
| Minigame Mode navigation, all aspects | `build/mdmini-qa/runs/20260930-033629-795934`, `20260930-033517-210023`, `20260930-033636-404827` | Fresh hidden 4:3/16:9/21:9 lifecycle runs pass. Each has 46 captures covering player count, characters, CPU difficulty, mode selection, backtracking, exit and reentry; all three sheets inspected. `20260930-033406-693853` additionally verifies the 16:9 unavailable-mode handoff and safe return. |
| Party Mode setup, all aspects | `build/widescreen-qa/runs/20260930-033238-719450`, `20260930-033340-065986` | Ordinary scripted inputs reach board selection, rules, player count, human/CPU characters, handicaps, confirmation and Treetop entry. Backtracking from rules to board selection is checked. 100 frames per aspect; menu sheets inspected at 4:3/16:9/21:9. Existing native-sized props and center art stay unchanged; the extended cloud frame retains its authored plain-white side columns, not invented decorations. |
| M612/M632 split views | `build/widescreen-qa/runs/20260928-222623-668596` | Capture smoke tests at three aspects; split layout inspected |
| M651/M656 skies | `build/widescreen-qa/runs/20260928-225609-603441` | Three-aspect captures; 21:9 inspected |
| M656 divider | `build/widescreen-qa/runs/20260928-231219-932284` | Three-aspect captures plus native ownership/resize tests |
| M640 factory | `build/widescreen-qa/runs/20260928-232255-468427` | Three-aspect captures; floor/wall coverage inspected at 21:9 |
| M630 layered ground and backdrop | `build/widescreen-qa/runs/20260929-124728-329559` | 12 captures per aspect pass at 4:3, 16:9 and 21:9. Gameplay inspected at all three; 21:9 finish and return inspected. Exact mesh census preserves native face/position/UV/color prefixes and adds only 6 opaque-floor, 12 tint and 2 backdrop quads. Native allocation/resize tests additionally cover the full curved strip and tail indices. |
| M633 room | `build/widescreen-qa/runs/20260929-004627-864585` | Final 21:9 capture run passes; start and gameplay frames inspected with filled corners |
| M633 native/16:9 | `build/widescreen-qa/runs/20260929-005223-774167` | Final 4:3 and 16:9 capture runs pass |
| M618 course traversal | `build/widescreen-qa/runs/20260929-022746-694930`, `20260929-022917-279015` | Native AI traversal and return, 20 captures per aspect; second-course and later-course margins inspected. These runs precede the final clip guard. |
| M618 winner transition | `build/widescreen-qa/runs/20260929-023914-086295` | Final build: 32 event-triggered captures at each of 4:3, 16:9 and 21:9; expanded winner frames inspected, including the formerly corrupt bottom-winner case |
| M618 final winner through return | `build/widescreen-qa/runs/20260930-041254-031643` | Fresh hidden run on the final code, 80 captures per aspect with `--require-return`. All three logs show real winner expansion, native return and following menu entry; all three sheets inspected. The earlier 32-frame capture stopped before return and must not be cited as full lifecycle proof. Native finite artwork and its plain edge-color continuation are preserved. |
| M668 cave and rigid rocks | `build/widescreen-qa/runs/20260929-112421-817020` | Final build: 12 captures at each of 4:3, 16:9 and 21:9 pass; final 21:9 gameplay inspected. Exact mesh-census comparison proves both rocks translate rigidly, both rock rims match, all three play contacts stay native and lava surface/UV prefixes are unchanged. Equivalent M668 run `20260929-111231-622653` also has inspected gameplay at all three aspects and the 21:9 winner camera. |
| Results | `build/board-qa-runs/wide-results-21-9-final` | Final-round fixture reaches ceremony, ranking, graphs, save and return; selected frames inspected |
| Native minigame sweep | `build/widescreen-qa/runs/20260928-223034-165939` | 18/20 compiled minigames render; not full gameplay completion |
| M635 garden / M638 callouts | `build/widescreen-qa/runs/20260929-162225-657617` | Ten captures each at all three aspects, exit 0 and no logged renderer errors. M635 gameplay inspected at all three; M638 21:9 callout margins inspected. |
| M635 night garden | `build/widescreen-qa/runs/20260929-162236-011031` | Ten captures per aspect pass; 21:9 gameplay inspected. Exact face-census comparison against this run's native view preserves all 90 original grass faces and adds only 20 border quads. Background terrain and sky unchanged. |
| M635 day geometry | `build/widescreen-qa/runs/20260929-162423-169493` vs `20260929-161224-702741` | All original grass, soil, background terrain and sky face records identical; exactly 20 grass-border quads added. Includes position, UV, material and color records. |
| Expanded minigame sweep | `build/widescreen-qa/runs/20260929-162339-910664` | M602/M608/M612/M616/M621/M632/M633/M637/M650/M657/M659/M670: all 36 three-aspect scenarios complete with ten captures each. Selected 21:9 frames inspected; visual review is not complete. This run exposed M657 timer drift, fixed in the next row. |
| M657 split timers | `build/widescreen-qa/runs/20260929-163923-247169` | Eight captures per aspect pass. Initial timer placement inspected at 4:3/16:9/21:9 and later play at 21:9; both timers remain over their own views. Native fixture checks 100 aspect changes and all timer components. |
| M635 distant terrain, day/night | `build/widescreen-qa/runs/20260929-171606-083452`, `20260929-171616-425736` | Eight captures per aspect pass for both variants; 21:9 gameplay inspected. Exact census retains all 90 grass faces, 160 day/156 night distant-terrain faces, original soil and sky. Only 20 grass and 34 distant-terrain quads are appended. |
| M602 scenery layers | `build/widescreen-qa/runs/20260929-172253-035913` | Eight captures per aspect pass. Exact owner-qualified census retains all 84 shelf faces, 28 shadow-receiver shelf faces, the original floor quad and all four podium surfaces; adds 12/4/2 quads respectively. This run precedes the atlas-camera fix. |
| M602 native card atlas | `build/widescreen-qa/runs/20260929-173455-961905` | Final build: twelve captures per aspect pass; gameplay inspected at all three. Logs show the three atlas views retain X=0/192/384, W=192, H=288, aspect=2/3 while the stage uses the full window. Actual camera-setter fixture checks 100 aspect changes and mode reset on camera reuse. |
| Opening storybook | `build/title-qa-runs/wide-opening-fixed-{4-3,16-9,21-9}` | Final fix: 22 captures per aspect, valid opening/title/file-selection lifecycle and no unexpected renderer errors. Corrected 21:9 page and grayscale transition inspected against native reference. Earlier `wide-opening-final-*` runs exposed the cropping bug and are not visual acceptance. |
| Title / file-selection entry | `build/title-qa-runs/wide-title-{4-3,21-9}`, `wide-title-fixed-16-9`, `wide-filesel-21-9`, `wide-filesel-fixed-{4-3,16-9}` | Title: 18 frames per aspect across two attract visits. File selection: 20 frames per aspect. Selected stable views inspected at all three aspects. Entry/idle only, not every menu action or save operation. |
| Native CPU completion sweep | `build/widescreen-qa/runs/20260929-175507-730930`, `20260929-180057-099616` | All 19 games other than separately tested M618 complete at all three aspects: 57 scenarios, 26 captures each, ordered native entry/return/next-overlay lifecycle and no renderer errors. All ultrawide sheets and native/16:9 comparison sheets inspected. Additional dense `20260929-184132-579317` captures show M612's real winner at all aspects and M659's native winner; M659's other two outcomes were ties, so their winner views remain open. M656's layering defect is fixed in the next row. |
| M656 portrait layering | `build/widescreen-qa/runs/20260929-181611-659309` | Final helper: 18 captures and native completion per aspect at 4:3, 16:9 and 21:9. All three contact sheets inspected: complete portrait faces draw in front of the track throughout gameplay. Native fixture checks group identity/order, matrix address and sprite records across 102 resize cycles, repeated no-op calls, allocation rollback and cleanup. |
| Board map / pause | `build/board-qa-runs/wide-map-buttons-{640x480,848x480,1120x480}`, `treetop-border-map-*` | Ordinary Y/X/Start inputs open overview, scrolling map and pause, then return and finish a turn at all three aspects. The later terrain fix fills the previously exposed outer corners; final captures inspected at all three aspects. |
| Board shops / movement | `build/board-qa-runs/wide-shop-buttons-*`, `wide-hidden-transition-20260930-spring-*` | Shop choices, descriptions, purchase and return inspected at all three aspects. Spring event begin/end and turn completion pass at all three; rope travel, landing and subsequent board event cameras inspected. These movement routes use explicit starting-space fixtures. |
| Results ceremony / ranking | `build/board-qa-runs/wide-results-current-{4-3,16-9,21-9}` | Final-round fixture reaches ceremony, ranking, interactive results, save and return. Selected frames inspected at all three aspects; this is not a full-match proof. |
| Results graph tabs | `build/board-qa-runs/wide-results-graphs-{640x480,848x480,1120x480}` | Ordinary R/down inputs reach actual graph drawing and complete the results lifecycle, 16 captures per aspect. Frames inspected, but the final-round fixture has no prior-turn history and blank graphs even at native aspect. This proves tabs/layout, not history lines; a complete match is still required. |
| File-selection actions | `build/filesel-qa-runs/wide-{create,copy,erase,verify}-20260930e-{640x480,848x480,1120x480}` | All 12 isolated native-menu runs pass; every contact sheet inspected. Name entry, copy destination, erase confirmation, card write messages and return/reload stay centered with native proportions. Persisted card hashes and slot states checked across fresh processes; user's saves untouched. Corrected badge is hidden during name entry at all aspects. |
| M659 real winner | `build/widescreen-qa/runs/20260930-030544-638565` | Real AI win, 50 captures and native return at each aspect; all three winner sheets inspected. The baseline `20260930-023304-694985` exposed 21:9 corner seams. Hiding only `642nightSKYa1` isolated their owner; the retained border extension removes them with original face/position/color records identical. No hidden meshes in accepted runs. |
| Board image masks | `build/board-qa-runs/wide-hidden-board-masks-circle-20260930-{640x480,848x480,1120x480}` | Styles 6..8 animate and return at all aspects; all midpoint and endpoint sheets inspected. Corrected circle clears the whole viewport before removal; character-mask endpoints match authored native behavior. Effect fixtures, not natural event routing. |
| Bridge and day/night transitions | `build/board-qa-runs/wide-hidden-transition-observed-20260930-{bridge,day-night}-{640x480,848x480,1120x480}` | All six hidden runs pass without logged renderer errors. Bridge capture triggers on actual deformation under the player, followed by turn completion. Day/night captures cover the real transition from its initial wipe through the overview, Boo-house and upper-tree cameras; begin/end events prove return. All six sheets inspected against native aspect. Starting-space/turn fixtures, not unrestricted route coverage. |
| Full-match history attempt | `build/board-qa-runs/wide-hidden-full-results-20260930-1120x480` | Incomplete: external 1500-second limit on turn 20's final player, 80 turns begun/79 finished. Log was advancing in a board event, not a proven deadlock. No results/history acceptance; longer rerun required. |
| Full-match missing-history baseline | `build/board-qa-runs/wide-hidden-full-results-long-20260930-1120x480` | All 80 turns, Last Five Turns, day/night, ceremony, tabs, save and return completed. Visual review rejected the near-empty graphs: every completed-round history cell was zero despite nonzero live totals. Native640 follow-up was intentionally stopped and848 never started; neither is acceptance. |
| Corrected full-match history, all aspects | `build/board-qa-runs/wide-hidden-full-results-historyfix-20260930-{640x480,848x480,1120x480}` | All three complete: 816.2/1070.1/1214.5 seconds respectively, each with all80 turns and20 independent round snapshots, Last Five Turns, six day/night transitions, full ceremony/tabs/save/return. Every coin/star graph point equals the independently recorded round-end total for all four players. All26 captures per run exist; all three sheets inspected, including real star/coin lines, selected players, save fade and title return. |
| Cold-disabled live toggle, minigames | `build/widescreen-qa/runs/20260930-045113-086184` | All20 native games start at640 with widescreen OFF, then pass nine settled size/preference states. 26 captures each; all20 sheets inspected. Root registration/restore fixes remove M656 sky, M618 course and mode-select backdrop gaps. Short games may return before the last state; the separate60-case native lifecycle evidence remains required. M632's newly found wall corner is fixed in the next row. |
| M632 sloped outer walls | `build/widescreen-qa/runs/20260930-051003-212152`, `20260930-051241-332857` | 28 captures and native gameplay/winner/return pass at all three aspects; all three sheets inspected. Actual face census retains all372 original faces/positions/UVs/materials and adds only12 QUADs. A separate26-frame cold-disabled live-toggle run passes all nine states while the game remains active; its tilted-box corners are filled. Native tests cover100 cycles, exact original surfaces and bounds, flat caps, failure rollback and no gameplay edits. |
| Cold-disabled live toggle, menus | `build/widescreen-qa/runs/20260930-045123-317903`, `20260930-045521-297728`, `20260930-045529-361812` | Mode-select, Party Mode, Minigame Mode, seeded file selection and the boot health warning pass nine settled states,26 captures each; all five sheets inspected. The first run's unseeded file-selection error is rejected. Its successful replacement uses only a hash-verified isolated QA card, never a player save. The warning-screen run does not replace the separate opening/title lifecycle evidence. |
| Final results geometry regression | `build/board-qa-runs/wide-results-final-coldfix-20260930-{640x480,848x480,1120x480}` | Rebuilt QA with the final cold-toggle and sloped-rim changes. All three final-round fixtures pass ceremony/ranking/graphs/save/title lifecycle without renderer errors.26 ceremony captures per aspect; all three sheets inspected, including the extended floor and theater frame. These short fixtures do not replace the independent20-round history proof above. |

Windows production, headless, Android native and isolated QA Release pass with
the completed-round history, cold-toggle lifecycle and M632 sloped-rim changes.
All255 regression tests pass (100.482 seconds). Native checks include the
stricter history gate, byte-preserving
bookkeeping,100-cycle cold toggles and sloped boundary extension.
The board evidence checker now also rejects renderer validation and allocator
errors after an otherwise clean exit, allowing only the exact expected empty-card miss.
All three aspects complete all 13 game variants, five board capture/animation
styles and three board image masks without renderer or ownership errors;
midpoint and endpoint contact sheets are inspected.
Three-aspect captures and native geometry comparisons pass for the affected scenery.
Android's build audit verifies exactly one `-O2` optimization flag on each of
391 translation units. No Android visual run or new APK upload is implied by
a native build. Continuation notes are in `build/widescreen-audit/CURRENT.md`.

The current build list is reconciled with retained capture files in
`build/widescreen-audit/completion-minigame-artifacts.json`: all20 compiled
minigames at all three aspects,60 cases. The check rereads ordered overlay
entry/return/next-entry events and errors, verifies every capture's dimensions,
byte size and increasing ticks, and rejects diagnostic mesh hiding. It is an
artifact/lifecycle check, not a substitute for the visual reviews above.
`completion-menu-artifacts.json` independently rechecks18 Party/Minigame Mode
and file-operation cases, including persisted card hashes across create/copy/
erase/reload, native navigation markers and actual capture dimensions/ticks.

## Audit findings and scope

### Results history regression — closed (Sep30)

The full-match run exposed missing results data, not simply an untested graph
layout. The existing explicit minigame stub returned directly to the board
without storing the completed round's coin/star totals. It now stores only
those actual totals at `turnNo - 1`, bounded by Party Mode, the configured last
round and the graph capacity. It does not award coins/stars, unlock games,
simulate a minigame, or change the other skip paths. Native tests compare all
player/system bytes and exercise valid, invalid and repeated calls.

QA independently observes live totals after the board's round-end callback,
before advancing the round or calling the stub. The full-match gate compares
every results graph entry with those observations for all20 rounds and all
four players; ceremony bonus stars are kept separate. Missing history no longer
passes on lifecycle markers alone. Fresh full matches at4:3,16:9 and21:9 now
pass, including visual inspection of the real coin/star history lines and
save/title return at every aspect.

### Coordinate-domain review (Sep30)

The regenerated `build/widescreen-audit/inventory-current.json` covers 388
selected native/port units (316 native, 72 port). Direct viewport, scissor,
texture-copy and framebuffer-copy call sites were reviewed against their
owners. Screen-sized constants are not globally widened: these paths have
different domains.

| Path | Coordinate/storage domain |
| --- | --- |
| Shared camera setters, camera setup and Z clear | Registered screen or split-view camera rectangles; explicit native render targets opt out. |
| Shadow and reflection passes | Square shadow-quality targets or fixed reflection textures, then restoration to the live screen. Their texture dimensions must not follow aspect ratio. |
| Sprites, messages, map and clipped score panels | Centered576-unit logical canvas; clipping is mapped to the live framebuffer. Model/sprite local scales are not screen widths. |
| Game/board wipes, pause, Party/Minigame Mode composites, file-selection full-frame copy | Live screen source and quad coverage, separately fixed native capture storage. |
| Opening pages and M602 card views | Offscreen native artwork/atlas cameras and copies. Widening these crops the actual texture content. |
| File-selection card previews and M668's40x32 sample | Intentional subrectangles/low-resolution samples; not full-frame copies. |

The dormant mode-selection feedback hook previously declared a wide texture
over a640x480 allocation. Its capture now reads the full live screen into the
original640x480 destination, and texture declarations remain native. The
extracted runtime test covers resize/toggle widths640/848/1120/2560/back640.
There are no callers of its allocation/enable functions in the current source;
this is a storage-contract correction, not a claimed visible menu improvement.

The generic water renderer also has no current source caller of
`Hu3DWaterCreate`. Its initial copy is adapted, but the two later
`WaterLayerHook` copies still use native source widths; review the whole
water chain before enabling a new consumer. This is not evidence of a current
Treetop-water defect. The legacy debug-font/wireframe paths and port diagnostic
scissor restoration remain separately identified, not certified as player UI.

The board circle-mask endpoint comparison exposed leftover black side bands
just before the ultrawide reveal ended. `WipeCircleHalfSize` now preserves
native corner-clearance timing by uniform diagonal scaling; the native path
is exact, and the two character masks keep their original artwork animation.
The extracted geometry test and fresh three-aspect rendered comparisons pass.
The earlier `wide-hidden-board-masks-20260930-*` captures remain the baseline.

### Completion gate

The completed audit covers the current compiled content, not only the
initially reported scenes. Its required gates and recorded evidence are:

- Inventory the current compiled source closure and the port's screen-domain
  adapters. Classify projection, viewport/scissor, texture-copy, UI and scenery
  calls by their real coordinate domain; literals alone are not defects.
- Exercise every currently compiled native minigame (20 in this checkout) at
  4:3, 16:9 and 21:9. Inspect active play plus differing intro, split-view,
  winner/end or moving-camera states. Capture-count success alone is not visual
  acceptance. The test launcher must provide valid teams and motion preloads.
- Cover opening/title, file selection, mode selection, Party Mode and Minigame
  Mode menus, board play/map/shop/pause, board transitions/effects and the whole
  results flow. Test the original 4:3 view as well as widened views.
- Verify resize/toggle and scene teardown/reentry behavior, native artwork and
  gameplay/collision preservation, full-frame effect coverage, and unchanged
  texture allocation bounds. Preserve the user's rigid Burnstile rock request.
- Fix verified port-owned defects and rerun affected visual/native tests.
  Unavailable gameplay stays explicitly unavailable; implementing missing game
  code or bypassing it is not a widescreen fix.
- Rebuild applicable Release targets and run the regression suite after final
  production changes. Report remaining device/scene coverage honestly; do not
   mark the whole goal complete while required evidence remains missing.

For the historical September 20-minigame closure, all six gates above were
recorded satisfied. That statement does not certify the expanded 32-minigame
closure or supersede the reopened October reports above. Windows captures and
native tests establish the stated visual/geometry evidence; the earlier Android
compilation is not on-device visual validation.
Dormant water/debug paths and unavailable gameplay remain explicitly outside
the playable-content claim, as described above.

- M633's later ultrawide camera and the CPU sweep's native/16:9 comparisons
  are now inspected. M618 retains its original finite decorative
  artwork; its newly exposed outer background uses edge-color continuation,
  not additional invented decorations. Not every camera/input sequence is proven.
- M630 retains the original finite decorative sky pattern; the added side
  panels continue its plain backing rather than stretching that pattern.
  The rejected lowered-tint prototype is not retained: the verified material
  has `invAlpha=0.7`, so its 30%-opaque tint requires opaque ground underneath.
  The retained fix extends both layers at their native heights.
- M635/M638's earlier crashes were invalid direct QA launches: all four players
  were in team zero and menu motion preloads were skipped. The QA-only launcher
  now prepares groups from the actual minigame type and preloads the selected
  characters. Native tests cover every minigame type and configuration
  preservation. Both games now run at all three aspects, without a gameplay fix.
- M635's black gaps are filled, but its finite distant terrain still has an
  authored light/dark color boundary at 21:9. Its distant perimeter now extends;
  further art blending, if needed, must not stretch the garden or houses.
- M659's narrow bottom background band was already visible in native 4:3
  (`20260929-162339-910664`, frame zero). That observation was not acceptance:
  it is now repaired through bottom-edge colour continuation, as documented in
  the current follow-up above. No camera-height change was used to hide it.
- Treetop's formerly exposed overview corners and night Boo-house wedge are
  fixed and inspected at all three aspects. Map, pause and shop choices/purchase
  have three-aspect visual coverage. Slide and spring travel/return captures
  also pass at all three aspects. The hidden Sep30 sweep covers all13 game
  wipes and board capture styles1..5 at all three aspects, with all15 contact
  sheets inspected. Board image-mask styles6..8 and file-selection actions now
  also have three-aspect visual coverage. Actual bridge deformation and the
  complete day/night transition now pass at all three aspects with inspected
   captures. Real-history results now pass full20-round matches at all three
   aspects. Short final-round fixtures are separate geometry checks, not
   full-match proof; bounded CPU playthroughs do not prove every input sequence.
- Minigame Mode's unavailable submodes remain explicitly unavailable; widescreen
  work does not implement their missing gameplay.
- Experimental M630/M633/M668 whole/background warps were removed after visual
  rejection. Their old screenshots are **not** evidence for the retained build.

## Reproduce

Build the isolated QA executable, then request exact native overlay names:

```text
python tests/integration/build_widescreen_qa.py
python tests/integration/run_widescreen_scenes.py --scenes mdseldll --reenter --count 2
python tests/integration/run_widescreen_scenes.py --scenes m618dll --all-cpu --cpu-difficulty 3 --winner-split --delay 0 --stride 3 --count 32 --timeout 300
python tests/integration/run_widescreen_scenes.py --scenes m656dll --all-cpu --cpu-difficulty 3 --delay 300 --stride 240 --count 18 --timeout 180 --require-return
python tests/integration/run_widescreen_scenes.py --scenes m659dll --all-cpu --cpu-difficulties 3 0 0 0 --winner-camera --delay 0 --stride 10 --count 50 --timeout 600 --require-return
python tests/integration/run_widescreen_scenes.py --scenes mdpartydll --party-menu --delay 0 --stride 60 --count 100 --timeout 240
python tests/integration/run_widescreen_scenes.py --scenes minigames --sizes 640x480 --resize-live --all-cpu --cpu-difficulty 3 --delay 400 --stride 60 --count 26 --timeout 180
python tests/integration/build_mdmini_qa.py
python tests/integration/run_mdmini_qa.py --widescreen --window-size 1680x720
python tests/integration/run_mdmini_qa.py --scenario lifecycle --widescreen --window-size 848x480 --capture-trigger qa.mdmini.stage=intro --capture-delay 0 --capture-count 46 --capture-stride 40
python -m unittest tests.test_widescreen_camera tests.test_widescreen_effects -v
```

The scene runner arms its tick budget on the target overlay event, avoiding
false failures during boot's wall-clock waits. Its external timeout remains a
hard bound. `--geometry` logs a mesh census; `--mesh NAME` adds a bounded face
dump in the QA executable only. Scene initialization can continue after the
first census, so a missing object in that census is not proof it never loads.
The census waits at least 500 scene ticks for asynchronous loading. Mesh names
may be comma-separated, and untextured faces include vertex colors. Capture
stride is restricted to the engine's supported 1..1000 range; invalid schedules
fail rather than silently capturing adjacent frames. `--all-cpu` uses the
native AI at the selected difficulty for every compiled minigame;
`--require-return` requires ordered target entry, native return and a different
next overlay, rejecting skipped/stubbed gameplay. M618 `--winner-split` waits for a real win,
then captures the transition. Neither changes production gameplay or teleports
actors. A winner-transition capture is not by itself an end-to-menu proof.

The Minigame Mode input script selects one human through the native menu. The
menu otherwise defaults to the number of connected controllers, which can leave
a P1-only script waiting for real P2 input.
