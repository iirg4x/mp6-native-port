# Board QA: current findings

## Board Masu / Single integration (2026-09-12)

The port consumes clean decomp main
`a1aa433c8d3b1c589e896372a40f5fd559117eb4`, rechecked against the remote branch
after building. Only the port-owned dependency checkout advanced. The 703-file
pre-update dirty source state and both native binaries are retained under
`build/checkpoints/main-a1aa433-before/`; the shared decomp was not touched.

The recovered Masu and Single sources and shared game-work API are consumed.
Masu's native big-endian decoding and AO decal brackets and game-work's
custom-minigame-list stub were relocated, with every removed/added hunk line
verified unchanged. Single compiles directly from upstream, including its
corrected particle ownership. The recovered integer return from
`GWSingleMgFlagSet` comes from the real upstream header and implementation.
The existing minigame-call stub remains. QR/DCT speech updates are included in
the pin, but the native port does not compile that SDK.

Verification:

- Windows Release (175 units) and Android arm64 Release (177 units) link.
  Android's authenticated manifest verifies every game/platform unit at `-O2`
  and the exact stripped libraries staged for later packaging.
- All 176 engine/native tests and 55 setup/release tests pass. New tests execute
  the actual Masu decoder macros on aligned and unaligned packed data, checking
  vectors, attributes, link numbering and cursor movement. Negative controls
  reject native-endian attributes and off-by-one links. Recovered Single flags
  are exercised across all 96 bits and both invalid bounds, including the new
  return value; a wrong-return negative control fails as intended.
- `main-a1aa433-release-smoke` reaches `w01.live`, decodes 107 spaces per layer,
  and captures three 1920x1080 frames with widescreen, AO and FXAA enabled.
  First and last captures were visually inspected. The harness ends the run
  at its 40-second limit, with no crash marker. This is board rendering at the
  opening dialogue, not full gameplay or Solo Mode completion.
- Checkpoint comparison preserves all pre-existing source files except these
  two relocated compatibility patches and the update documentation. Renderer,
  Android optimization and widescreen source contents remain unchanged.

The evidence and hashes are in `build/main-update-a1aa433/integration.json`;
the isolated log and raw/decoded captures are under
`build/board-qa-runs/main-a1aa433-release-smoke/`. No player saves, shared decomp
files, APK uploads or remote branches were changed. No physical Android test
or framerate improvement is claimed for this dependency update.

## Board Star integration (2026-09-11)

The port consumes clean decomp main
`3b0a20d3239d31f20921287d7ffa17f4219026f7`, verified against the remote branch.
Only the port-owned `build/deps/marioparty6` checkout was advanced. The previous
615-file dirty source state and both native binaries are backed up under
`build/checkpoints/star-3b0a20d-before/`; the shared decomp is untouched.

Graphify's bounded source-tracing fallback identified Star and its W01 caller
as the changed gameplay inputs. Star now owns its constants and uses the
recovered Star-reset flag, not the turn-no-start flag. The obsolete nine-value
board constant bridge and build entry were removed. The existing particle
alpha conversion guard is unchanged apart from its hunk location. Native
adaptations supply `abs`, match the guide provider's `int` return, and keep the
previous port's supplied position vector rather than interpreting the address
of a host pointer parameter as a vector. The latter is explicitly a native ABI
adaptation, not a claim of matching the retail PPC parameter-address quirk.
No new gameplay stub, shader change or quality reduction was added.

Verification:

- Windows Release (175 units) and Android arm64 Release (177 units) link.
  Android verifies exactly one `-O2` for every game/platform unit. The counts
  drop by one solely because the unused constant bridge was removed.
- Star compiles under both actual target toolchains with implicit-function and
  incompatible-callback declarations treated as errors. Both report the existing
  `signF = 2` truncation into a one-bit field; it remains unchanged, not silently
  reinterpreted as a gameplay fix.
- 155 engine/native tests pass in the full rerun, plus the subsequently added
  hyphenated-fixture parser regression (all six parser tests pass). 55 setup and
  release tests pass. The first full run timed out in the unrelated negative
  vertex-register test; its isolated rerun and the full rerun both pass.
- New native tests execute the recovered Star addition helpers across positive,
  negative, zero and clamped awards; fast/animated pacing; and display modes.
  They reject an incorrect cap, reject both host-pointer-address mutations, and
  check finite/nonfinite particle alpha conversion. Flag/prototype/constant
  ownership checks accompany those executable tests.
- Four isolated widescreen/FXAA live runs finish normally, pass state/camera and
  one-shot motion gates, wait 1,200 game ticks after the event, and complete at
  least one ordinary turn. `star-3b0a20d-purchase` buys an authored Star for 20
  coins (50 to 30, zero to one Star); `star-3b0a20d-position` awards one Star at
  an explicit vector; `star-3b0a20d-ztar` and `star-3b0a20d-ztar-position` reduce
  the fixture's two Stars to one through real Ztar event code. Strong AO is on
  except for the normal Ztar run. Six frames per run are retained, and event
  captures from all four runs were visually reviewed.
- Audio logs prove every observed looping 1095/1122 handle was stopped before
  event return. This is handle-lifetime evidence, not a subjective listening test.
- The QA shader selector now forwards the borrowed-depth seed variant, and its
  event parser recognizes hyphenated fixture names while still rejecting missing
  events and invalid vectors. These are harness changes only.

Source/artifact hashes and event/inventory/audio assertions are recorded in
`build/star-update-3b0a20d/integration.json`; logs and raw captures are in
`build/board-qa-runs/star-3b0a20d-*`. Existing renderer and Android binding
optimizations were preserved byte-for-byte. This is not a full-match, physical
Android-device, FPS or every-Star-branch certification. User saves were not
used. No APK packaging/upload, source push or shared dependency edit occurred.

## Board opening, coin, dice and effect integration (2026-09-11)

The port consumes clean decomp main
`a15c000ba1aac3c05f2db5f8b9a7665552050716`, verified against remote main after
the update. Only the port-owned dependency checkout was advanced; the shared
decomp development checkout was not changed. Existing dirty port work was
preserved in `build/checkpoints/board-a15c000-before/` before the update.

The recovered owners now provide the particle quad-rotation, colour-register,
emission and dice-effect corrections. Compatibility patches preserve native
fade storage, widescreen fades, particle array registration, explicit GXEnd,
coin-table bounds and dice-object detachment. Opening's curve callbacks now use
explicit float arguments without integer pointer casts. Its guide and space-list
return declarations match their native implementations. Seventeen obsolete Dice
and Opening constant definitions were removed; unrelated constants remain.

Verification:

- 144 regression tests and 55 setup/release tests pass. New tests execute the
  actual recovered opening curve functions through volatile callbacks, including
  curved paths and inverse-distance evaluation. Updated math/Opening caller
  declarations are checked on x86-64 and AArch64.
- All six changed board translation units compile with implicit declarations
  and incompatible callback types treated as errors. This exposed missing
  `abs`, `sprintf` and branch-mask declarations, now supplied by native patches.
  The audit still reports the existing low-address message-pointer cast and
  SNPC's valueless return warning; this is not a warning-free certification.
- Windows production Release and Android arm64 native Release link. The
  Android manifest verifies exactly one `-O2` for all 178 game/platform units.
- `board-a15c000-opening-ao2`: fresh W01 opening, turn-order dice, ten starting
  coins per player and one completed normal player turn. AO + FXAA, 1280x720
  widescreen. State/camera and player-motion gates pass. Reviewed captures show
  the normal turn announcement, dice particles, roll and number animation.
- `board-a15c000-donkey-dice`: QA-only selection of the real DK dice branch,
  reward, event return, subsequent normal dice/movement and completed turn.
  FXAA, AO off, 1280x720 widescreen. The scared reaction reaches its end without
  looping; state/camera and required-event gates pass. Reward and wait completion
  are not stubbed. Captures were reviewed.

The QA harness was brought up to date with the current ordered AO depth task;
it forwards all shader variant parameters and rejects its incompatible old
raw-depth comparison mode explicitly. No new gameplay stub, full-match claim,
physical Android-device validation, FPS claim, APK upload or push accompanies
this update. Source-change and artifact hashes are recorded in
`build/board-update-a15c000/integration.json`.

## Board math integration (2026-09-10)

The port now consumes clean decomp main
`9651a1e5252c4631d30f9ad4632fb665429e13c4`. Board math compiles directly from
the dependency's portable C branch, with no math replacement or new gameplay
stub. This includes upstream's normalized-to-screen Y correction. The W01
radian-rotation declaration was aligned with the recovered `u8` axis parameter;
the degree callers are already corrected upstream. Eight board-effect
compatibility hunks were relocated while preserving their contents, including
native fade storage, widescreen fade coverage and particle array registration.
The preceding CapSpecial integration remains intact.

Verification completed:

- 103 CPU/native regression tests and 55 setup/release tests pass. New tests
  execute 28 actual portable math functions: quantized degree/radian lookup,
  negative/wrapped angles, in-place and nonuniformly scaled rotations, Euler
  order, translation, screen projection and bounding-box updates. Negative
  controls reject the old screen-Y formula and a broken translation store.
  Axis declarations are checked against their owners on x86-64 and AArch64.
- The complete math translation unit also compiles with implicit declarations
  and incompatible callback types treated as errors; its diagnostic log is empty.
- Windows production Release and Android arm64 native Release link. Android's
  authenticated build manifest verifies exactly one `-O2` for all 178
  game/platform units. No APK was packaged or uploaded for this update.
- `math-9651a1e-slide`: real slide entry/exit, two completed turns, valid
  player/camera vectors and one-shot motions; AO + FXAA, 1280x720 widescreen.
  Reviewed captures show the descent, Orb pickup and continued board movement.
- `math-9651a1e-spring`: real spring entry/exit, four completed turns and a DK
  capsule entry/return; FXAA, AO off, 1280x720 widescreen. Reviewed captures show
  flight, landing and the DK appearance. State/motion gates pass, including a
  completed scared reaction.
- `math-9651a1e-bridge` completed two normal turns and passed movement/camera
  gates, but did **not** reach a bridge event. It is not bridge-animation proof.

These are bounded Windows integration checks, not a new full-match or physical
Android-device certification. The shared decomp checkout and matching tooling
were not changed. Pre-update source and production artifacts are preserved at
`build/checkpoints/math-9651a1e-before/`.

Production SHA-256 values (not the instrumented QA executable):

- `build/release/mp6native.exe`:
  `b3df34d84d32dbce72b80ace97e49a34831cfbfce468aae23ecea83486d7d93c`.
- `build/android/aurora/libmp6game.so` (unstripped):
  `2cf32fb8860b44187e793b64b38d1628cb3840379469265eea59e4cd9d827cfd`.

## CapSpecial integration (2026-09-10)

This integration used clean decomp main `bf77696ce47c4914e39fe726972c837065cdedde`.
The shared decomp checkout and matching tools are unchanged. Existing Boo
widescreen/material handling and the explicit duel-minigame stub are retained.
Upstream now owns the DK one-shot reaction, `sizeof(CAPWORK)` child copy,
field-relative Bowser event data and recovered constants; obsolete replacements
were removed from the port patch queue.

The native work layout still needs a 96-byte shared event payload. CapSpecial's
new embedded guide pointer would widen that payload on 64-bit hosts, so it is
kept on the duel coroutine stack and passed to its helpers instead. Compiled
layout/copy tests compare all five capsule views. Cross-file declaration checks
cover over 60 CapSpecial APIs on x86-64 and AArch64, including the void squish
forwarder whose return register was incorrectly consumed as Bowser's count.
The native caller now obtains that count from the forwarder's unchanged
implementation, with the identical `FALSE` voice flag.

Two initial Windows DK dice runs stalled after the hit animation:
`capspecial-bf77696-dk` and `capspecial-bf77696-dk-dice`. Wait instrumentation
identified `DiceProcMain` waiting in `mbDiceNumStopCheck`, not a missing
minigame. The recovered `mbDiceNumObjKill` left deleted objects registered in
`diceNumOMObj`; the native object pool can reuse the slot for an unrelated
object with its update bit set. The port cleanup now detaches only references
to that exact object before deletion. It does not clear live animations or
short-circuit the completion test. A compiled regression demonstrates failure
with the original cleanup and success with the patch for every player/slot,
while preserving neighboring live dice and their genuine waits.

The QA-only `donkey-dice` route selects DK's real dice branch, without forcing
its reward, completing an animation, or changing any wait predicate. All
fixtures and wait probes are confined to the separate board-QA executable.
This integration is not a new full-match or Android-device certification.

Verification completed:

- 101 CPU/native regression tests and 55 setup/release tests pass. CapSpecial
  also compiles with implicit function declarations treated as errors.
- Windows production Release and Android arm64 native Release link successfully.
  The Android manifest verifies exactly one `-O2` for each of 178 game/platform
  translation units. No APK was packaged or uploaded for this integration.
- `capspecial-bf77696-dk-dice-fixed-1` and `-2`: both complete the real DK dice
  roll (10-coin reward), event return, normal dice/movement and player turn.
  The second uses AO + FXAA. Both pass the scared-reaction end/no-loop gate;
  reviewed captures from the first show the reward and return to board play.
- `capspecial-bf77696-bowser-v2`: event entry/return and camera checks pass
  with AO + FXAA after the squish-count ABI correction. `capspecial-bf77696-duel`
  passes its existing explicit stub and returns to normal turns. Neither run
  is evidence that an undecompiled minigame was played.
- `capspecial-bf77696-boo-house` and `capspecial-bf77696-boo-return`: real night
  assets, theft, entrance/return state gates and reviewed 1280x720 captures,
  including the late full-width cross-fade. AO + FXAA enabled.

Artifacts (unpackaged production outputs, not the instrumented QA executable):

- Windows `build/release/mp6native.exe` SHA-256:
  `c0f62d05eb9d9d23a0ec5778b20166b71fe3ac24387ad02cf2b2cce9cd55f0bb`.
- Android `build/android/aurora/libmp6game.so` (unstripped) SHA-256:
  `d9d6725ead15203def256ca004e0569d044f5fe2c5e81f61c3767c065e1cc9b2`.
- Pre-update source/runtime checkpoint: `build/checkpoints/capspecial-bf77696-before/`.

## Previous rendering verification

Status: **results widescreen, capsule rendering/entry audit, and water AO exclusion
verified on Windows; not a new full-match or Android-device certification**.
Last tested 2026-09-08, Windows GPU release, port HEAD
`2a62946` plus the current dirty port tree and the repairs below.
The original eighteen-session diagnosis remains below as historical evidence.
The critical reconstruction was published as private Android 0.4.3 Preview.
The capsule-call repair below was published in private
[Android 0.4.4 Preview](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-space-capsule-fix-2a62946-dirty-20260905)
on 2026-09-05 (versionCode 40004). All 173 Android native units were rebuilt;
APK assembly, strict lint (no new issues), matching update certificate, 16 KB
alignment, version/content checks, and download hash verification passed.
The uploaded APK SHA-256 is
`c98ba428a93c9b7869b403db30e4614e2866246e49064d03920797d37850ec40`.
Its source snapshot is preserved in
`build/checkpoints/space-capsule-044-release-source/`. Android device gameplay
remains untested, and the sound-loop report below remains unresolved.

## Results theater border extension (2026-09-08, not published)

The 0.4.12 results change widened the camera and floor but left the theater
frame ending at its original width. `mp6_widescreen_extend_results_set()` now
continues the two upper cloud borders (`obj28`/`obj29`) and side columns
(`stage_base`/`stage_base2`). These are separate meshes inside the results asset,
not Party Mode's single `pillar` mesh. A runtime mesh/transform census identified
their cuts at world X = +/-800; checked profiles refuse unexpected layouts.

Twelve appended quads copy the source boundary loops' UVs, normals, material
and winding, following Party Mode's pinned-UV edge technique. Original vertices,
center props, podiums and character transforms are untouched. The appended
vertices follow their own objects' animation and live aspect ratio; the strips
collapse to zero area at native aspect. No shared decomp or Aurora edits.

Windows Release graphical runs `results-theater-wide-v1` (1728x720),
`results-theater-169-v1` (1280x720) and `results-theater-native-v1` (960x720,
widescreen disabled) all pass `check_board_qa.py --results --min-turns 4`.
Reviewed captures show the extended frame and normal award/winner transition;
all three complete rankings, statistics, save and return. These are final-round
fixtures, not full naturally played matches. `results-theater-census` preserves
the before-extension mesh evidence; `--results-geometry` enables that QA-only log.

All **74 CPU/native tests** pass, including live aspect arithmetic, native
vertex preservation, edge UV/winding checks, invalid-layout refusal and negative
controls for missing extension or incorrect UVs. Windows production Release
SHA-256: `907502dcff47e01bb343d077b64e24353f7d73f8be61d9befa48a57b2d9d162f`.
Android native Release SHA-256:
`21337dbd3ff0d26ef5cb0fb600b5c33af682c904a3f27c2fcb9f05687d328fd0`.
Android device gameplay and a new APK package/upload have not been performed.

## Results widescreen, capsule rendering and water AO (2026-09-08, 0.4.12)

Production changes remain in this port's compatibility/renderer layer. Shared
decomp and Aurora checkouts were not modified, and no missing gameplay was
reconstructed or newly bypassed.

- Results registers the same live Hor+ camera helper as Party Mode. Only the
  outer border of the results asset's `yuka` floor is extended. Its podiums,
  decorations, characters, inner floor and HUD are not globally stretched.
  The larger cylinder backdrop already covers the tested 2.4:1 viewport.
- Capsule rays lacked declarations for the float rotation/translation helpers;
  native calls default-promoted those arguments to double. Crack normalization
  also called the native double-returning `__frsqrte` as an implicit int.
  Correct declarations preserve the original effect formulas. Reviewed before/
  after placement captures show the solid beam replaced by separate fanning rays
  and fading sparkles. Existing native work layouts and GX array-span fixes remain.
- Boo's projected fade now captures the live width and uses the live board-camera
  aspect. The Boo-house cross-copy wipe explicitly resamples the full frame into
  its original 320x240 RGB565 buffer, retaining the original allocation size.
- Towering Treetop's day/night `mizu3` water is masked out of AO at its visible
  texture-covered pixels. Its original draw, alpha test and depth are unchanged.
  The mask shares the existing foreground target's alpha channel; no new target,
  fullscreen pass or readback was introduced. This excludes AO **on** water; it
  does not certify every other board's water asset or remove its scene depth.

Evidence in `build/board-qa-runs/`:

- `results-wide-floor-v1`: 1728x720, all four final-round turns, board finish,
  real ceremony, rankings, statistics, save and return; `--results --min-turns 4`
  reports `"errors": []`. Reviewed ceremony captures show full-width coverage.
  This is a final-round fixture, not twenty naturally played rounds.
- `capsule-full-audit-v3`: 41 bounded cases, all normal exit/required handler/
  camera gates passed. Reviewed placement captures cover all 13 placeable types.
  Self-use sets dice modes 1..6; Warp Pipe swaps players; Flutter purchases a
  star; landing tests transfer coins, remove/transfer inventory and move players.
  Night Boo steals a star; flashlight prevents theft and is consumed. Kamek's
  initial landing case has no owned spaces and only tests the refusal branch.
- `capsule-advanced-v4`: successful longer Zap/Thwomp movement, Bullet Bill's
  actual handler 40 after the roll (including victim coin transfers), and Kamek
  changing the placed space's owner from player 2 to player 3. Reviewed captures
  show the movement and ownership-replacement impact. These four runs resumed
  the same-build `capsule-audit-advanced-state` before their audited next turn.
- That advanced seed was unintentionally captured during an earlier player's
  turn. Its long Bob-omb run exited with access violation **before audit.begin**,
  at the previous player's turn end. It is not capsule evidence; the likely
  fixture-local host-pointer restore issue is not a diagnosed production fix.
  The matrix now rejects seeds captured after any turn starts. Fresh replay
  `capsule-long24-fresh-pass-long-24` passed: roll 8, real explosion, four-space
  movement and turn end; exit 0 in 54.1 seconds, captures reviewed. No gameplay
  changes were made to make this repeat pass.
- `boo-house-return-wide`: 1280x720, real night assets, entrance/capture and
  successful theft/return; `--min-turns 0` with audit/fade/return events reports
  `"errors": []`. The late capture explicitly includes the full-width cross-fade
  back to the house, not only its entrance.
- `water-mask-slide-live`: AO Strong, 1280x720, actual slide; reviewed river stays
  unshadowed while land/trunk AO remains. GPU synthetic tests additionally verify
  exact water pixels, tiny-alpha fringes/holes, preserved scene alpha and intact
  neighboring ground AO on desktop and Android shader variants.

Together the successful runs cover the 46 matrix cases, not every random branch,
every board or all character variants. Existing duel/DK minigame stubs are named
as stubs, not counted as played minigames. Null slots 30/32/45 are unchanged;
slot 31 is tested through Boo's real passive flashlight path.

Windows production Release SHA-256:
`114a597761972d9c92996813930c0c2932600351d6ce31ca80d2b92c19ba409a`.
Android native Release (`libmp6game.so`) SHA-256:
`5175f915a30c98c099bd914311af372b137ff2322754a7676f7736b5e625ffac`.
Both build successfully. The final regression run passed **73 CPU/native tests**
and **17 real GPU tests**. Native regression tests include negative controls for
the missing ABI declarations and fixed-width capture. The subsequent
[private Android 0.4.12 upload](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-capsule-results-0412-20260908)
included these changes, but not the theater-border correction above.
Android hardware gameplay remains untested.

## Recovered party results on latest main (2026-09-07, historical verification)

The port now pins `5f71cac24c910a0b061a0ed53bfa8ef53784dfca` and compiles
both recovered `mdpresultdll` units. The native loader binds the real prolog and
epilog. Only native symbol namespacing and two explicit GX submission endings
were needed in those units. No ceremony, ranking, reward, save, or return logic
was reconstructed or skipped. The separate shared decomp checkout was untouched.

Testing also exposed an existing port widescreen shortcut in `sprput.c.patch`:
it replaced every sprite scissor with the full screen. Results uses explicit
scissors, so off-panel statistics leaked beyond the table and graphs. The fix
widens only the default full-frame rectangle; custom framebuffer rectangles are
recentered and clipped to the live viewport. Native tests cover ordinary/wide,
odd-width, ultrawide and narrow viewports, empty/offscreen rectangles and live
Off, including a negative test that removes the required translation.

Verified Windows graphical runs:

- `results-main-first`: final-round fixture, actual four player turns and board
  teardown, recovered bonus ceremony including a tie-break, winner presentation,
  results table and normal title return. The initial test lacked a completion
  probe for that title destination; this is not the final gated run.
- `results-main-trigger`: Strong AO, MSAA 4x, widescreen; actual R-trigger graph
  switching and player selection followed by save and return. The custom-clipping
  defect is visible in these **before-fix** captures.
- `results-main-clipping-verified`: all four final-round turns end; results entry
  at tick **11870**, ceremony **11937**, ranking **16546**, statistics **16666**,
  graph draw **16758**, save return **17298**, and title setup **17299**.
  Exit 0 in 79.1 seconds. The state/ordering gate passes; 70 captured frames and
  their contact sheet show that the off-panel columns are no longer visible.
  Star/coin graph modes and selection respond to normal pad input. This final-round
  fixture has no fabricated 19-round history; it is not a populated-history or
  full 20-round gameplay certification.

`results-main-full20` was an unforced fresh all-CPU match. It completed 30 player
turns and reached round 8 without a crash before its 480-second wall-clock limit.
It is **not a full-match pass**. A temporary null-renderer probe reached the board
opening but not the first turn within 120 seconds; it also provides no full-match
evidence. No gameplay bypass was added to make either test pass.

`check_board_qa.py --results` now requires board finish, real ceremony/ranking/
statistics/save/return in order and rejects fallback, failed process, unrecognized
input and script timeout. `--full-match` additionally requires all 80 ordered turns,
day/night and Last Five Turns, not the obsolete unavailable-results marker.

All 54 CPU/native regression tests pass. Windows graphical/headless releases and
the Android release native library build; Android device gameplay and APK
packaging/upload were not performed. See [GROUNDING.md](GROUNDING.md) for the
same turn's measured main-tree/slide-base AO correction.

## Title crash and Windows release cleanup (2026-09-06)

The reported launch was the stale September 5 `build/mp6native.exe`, but the
same title bug also reproduced in the current sources. After two natural intro
replays, pressing Start issued the normal file-select overlay call and then
failed with `ANM native loader: free of unregistered native graph`.

Cause: the port's old attract-loop leak patch bulk-freed `HU_MEMNUM_OVL` from
HEAP_DVD/HEAP_MODEL while the boot overlay was still alive. Window fonts,
icons/cursors/card sprites retained those allocations until `HuWinAllKill` at
the actual overlay transition. Removing those two premature frees restores the
decomp's original replay lifecycle; the ANM ownership checks were not weakened.

Evidence in `build/title-qa-runs/`:

- `before-fix-headless` and `before-fix-windowed`: three title visits, accepted
  Start, then the same invalid animation free (exit 1).
- `after-fix-headless-20`: twenty title visits and successful file-select entry.
  After the first replay, HEAP_HEAP stayed at 668544 bytes, HEAP_DVD at 96 bytes,
  HEAP_MODEL at 15118464 bytes, and live native animations at 99 on every visit.
- `after-fix-headless-first`: first-title Start also passes.
- `after-fix-windowed-verified`: two natural replays, accepted Start, file-select
  entry and another 600 ticks, exit 0. Reviewed captures show the opening book,
  save slots and complete "Please choose a file" message. Memory is stable.
  This is an isolated new-card fixture, not the user's existing save.

Windows releases now use the GUI PE subsystem (verified value 2), with normal
diagnostics and crash stderr in `logs/mp6.log`, retaining `mp6.previous.log`.
Debug/headless retain the console subsystem (headless verified value 3).
Executed native tests cover absent GUI standard handles, both redirected
streams, stderr-only redirection, UTF-8 paths, log rotation and no console.
All 41 game/host tests plus four optimized-build tests pass.

Rebuilt production Windows executable SHA-256:
`c4ce66e38ffc30dbb49a1958775979161a2793aaf618c3dcdd189345f2b60689`.
Headless executable SHA-256:
`6fda6fc26e2cc3c82fef932026d78ca02161162cc933ef88cdfb17fbd5c89c71`.
No Android APK was built or uploaded for this change. Decomp and shared Aurora
were not edited. Production contains no title QA probe. The original card save
is hash-unchanged. Pre-change source is in `build/checkpoints/title-repeat-before-fix`.

Eight stale top-level/misplaced executable and PDB artifacts were moved,
hash-verified, to `build/recovery/stale-builds-20260906/`. The preceding release
executable/PDB is also copied into its `pre-title-release/` subdirectory. Nothing
was permanently deleted. `build/Play MP6.lnk` points at the maintained release
and retains `build` as its working directory; no saves/config were moved.

### Historical results limitation (superseded on 2026-09-07)

The older 20-round board tests below reached the **results stub**, not a completed
results ceremony. At decomp commit `8f9c3c010da32352908b637e7d6c46e8d99989e7`,
`src/REL/mdpresultdll/mdpresult.c` contains partial C functions but no `_prolog`
or `_epilog` implementation (the symbol map lists both). The source is marked
NonMatching in `configure.py`; the native build's `REL_SOURCES` and DLL bridge
do not register it. The loader therefore reaches its unavailable-overlay return
and went to mode select. No missing scene/entry code was reconstructed then.
The current port instead consumes the recovered main and utility units from
decomp main `5f71cac`; see the new verification section below. Historical stub
captures and old binary hashes remain evidence of those older builds only.

## Player reaction looping audit (2026-09-06, not yet published)

The DK-space event incorrectly requested `HU3D_MOTATTR_LOOP` for player slot 9,
the `c000m1_324` startle/jump/landing clip. In `player-motion-dk-before`, Mario's
44-frame reaction wrapped **26 times** during one real DK event. The exclamation
and landing effects repeated with it. The port's `capspecial.c.patch` now requests
`HU3D_MOTATTR_NONE`, matching every other use of that stock reaction. It finishes
once and holds its final pose until the event selects the next animation. No
global loop suppression, animation replacement, or event/minigame reconstruction
was added; decomp was not edited.

`tools/audit_player_motions.py` inventories **297** player-motion requests in the
available decompiled sources with the port patches applied. It checks **100**
statically resolved stock one-shot requests, including **15** uses of slot 9.
The unpatched negative control identifies DK as the only incorrect stock
one-shot loop; the patched inventory has none. Movement, stun and sustained
damage loops remain unchanged. **77** requests contain dynamic slots/attributes
(including forwarding helpers); these are exposed with `--unresolved`, not
silently certified by the static check. Inspected dynamic cases include the
stun-followup slot 6, Wiggler/Paratroopa/Klepto carries, Twister flight, Miracle
floating and the W01 ride poses. This is not exhaustive rendered coverage of
every event-specific clip, character, or unavailable board/minigame.

The native regression executes the actual DK request and actual
`Hu3DMotionShiftSet` / `Hu3DMotionNext` functions: the old request fails, the new
one reaches and holds frame 44. It also checks all nine stock one-shot slots
after a looping motion and after an interrupted blend, then verifies that idle
can still loop. QA-only probes record accepted motion requests, end holds and
wraps; the evidence validator rejects one-shot loops and missing instrumentation.

Rendered evidence under `build/board-qa-runs/`:

- `player-motion-dk-after`: the real capsule-44 handler returns, the human turn
  completes, and the process exits normally (24.2 s). One scared start, one end,
  **zero scared wraps**; before/after captures were reviewed. Mario lands and
  stays standing during DK's dialogue. The old run deliberately fails the new
  motion validator; its clean process exit alone does not imply correct motion.
- `player-motion-full-cycle`: fresh ordinary CPU play completes all **20 rounds /
  80 player turns**, with all **29** capsule calls returning, **13** placements,
  **12** slides, **4** springs, **2** Boo fades and natural night Last5 at
  **76609-78943**. The last turn ends at **116268**, board teardown at **116291**,
  and the existing unavailable-results stub at **116295** (231.3 s, exit 0).
  The full-match and motion gates pass: **2,150** motion requests, **795** end
  observations, **6** scared reactions reaching their end, and **zero stock
  one-shot wraps**. Intentional non-one-shot wraps were still observed. As before,
  this reaches the existing results stub, not an implemented results screen.

Both fixed runs use QA executable SHA-256
`649fb947d247dc7c494d73429ca7b96257a05c0bcdd2003f499711589e68ae31`.
All **35 unit tests pass**, as do Windows production/headless and the 173-unit
Android native release builds. Production Windows SHA-256:
`cfe7e10f7988ff74fbbd647bb69592504ae8551bef29dafa295239f5647da16b`.
Unstripped Android game-library SHA-256:
`93f7e69aa366ea3590879520d9d7ba828e9424ca92baf0b936f6985536b410f3`.
No APK assembled/uploaded for this repair; Android device gameplay is untested.
Saves are unchanged. Source checkpoints: `before-player-reaction-loop-fix`
before the edit, and `player-reaction-loop-verified` after verification.

Reproduce from the port root:

```text
rtk proxy python tools/audit_player_motions.py
rtk proxy python -m unittest discover -s tests -q
rtk proxy python -u tests/integration/build_board_qa.py
rtk proxy python -u tests/integration/run_board_qa.py --name NEW-UNIQUE-DK-RUN --capsule 44 --one-turn --seconds 120 --window-size 1280x720 --widescreen --capture-frames 32 --capture-stride 8 --capture-trigger motion.scared
rtk proxy python tests/integration/check_board_qa.py build/board-qa-runs/NEW-UNIQUE-DK-RUN --event capsule.return --player-motions --scared-end
rtk proxy python -u tests/integration/run_board_qa.py --name NEW-UNIQUE-MATCH --route cpu-soak --stop-round 0 --seconds 480 --ticks 220000 --window-size 1360x630 --widescreen
rtk proxy python tests/integration/check_board_qa.py build/board-qa-runs/NEW-UNIQUE-MATCH --full-match --player-motions
```

## Shop widescreen and first-use rendering (2026-09-06, not yet published)

The shop description carousel parked unused windows only 576 UI units apart.
The widescreen canvas exposes those parked windows beyond the old 4:3 edge.
`compat/decomp/src/board/shopevent.c.patch` now scales each description's
displacement from the selected panel by the live canvas scale. The selected
description stays centered, original slide timing is retained, and positions
refresh while idle as well as during a slide so resizing does not leave a stale
layout. At 4:3 the positioning is unchanged. No messages or shop logic are stubbed.

First-use pop-in was also reproduced: the first shop frame drew its text but
omitted its background/frame. Aurora's normal pipeline priority queues shader
compilation, and `get_pipeline` returns false while that work is unfinished,
causing the draw to be skipped. Port-owned renderer adaptation
`0027-required-game-pipelines-before-draw.patch` routes required GX and clear
pipelines through Aurora's existing blocking path, already used for the launcher.
Previously encountered pipelines still prewarm asynchronously from disk; a newly
needed pipeline must be ready before its frame is submitted. This removes that
source of missing draws, **not the cost of compiling an unseen shader**: first use
can still briefly hitch. It does not preload every possible mesh/texture/shader
combination, introduce a substitute shader, or reset the v3 cache.

Evidence under `build/board-qa-runs/`:

- `shop-wide-before` at 1360x630 reproduces the extra description on the right
  and the initially missing panel. Its directional-button script did not move
  selection; after buying an unusable defensive Orb, A-only input repeatedly
  reopened its menu and the run timed out. This is visual before-evidence, not a
  successful gameplay gate. The subsequent fixture uses analog-stick input and
  explicitly finishes after the real shop handler returns.
- `shop-wide-layout-after` at 1360x630 visits selections **0→1→2→1→0**, buys the
  selected Orb and returns (21.1 s, exit 0). All event/vector gates pass. Captures
  show only the selected description while idle and retain both panels during
  their intended slide. This build contains only the shop layout change; the
  missing first-frame box still demonstrates the independent shader issue.
- `shop-ready-cold-169` at 1280x720 repeats selection/purchase with both repairs,
  an empty application graphics cache and exit 0 (25.8 s). The same shop event
  gates pass. Its **first captured shop frame** already contains the complete
  description panel and text; no parked description is visible. The run persists
  157 GX pipeline configurations. These timings are not a controlled performance
  comparison, and the operating-system/driver caches were not cleared.
- `full-cycle-shop-cache-cold` at 1360x630: a fresh, non-accelerated **20-round /
  80-player-turn** match passes with both repairs and an empty application cache.
  All 39 capsule calls return, 15 placements, 14 slides, 5 springs and 4 Boo fades
  complete. Natural night Last5 runs at **96473–98807**, the last player turn
  ends at **137512**, normal teardown at **137535**, and the existing results
  stub at **137539**. Exit 0 after **283.0 s**; the full-match gate passes and
  Last5 fade/guide captures were reviewed. It persists **483** GX configurations.
  This still certifies the board cycle only, not an implemented results screen.
  Both this run and `shop-ready-cold-169` use QA executable SHA-256
  `9ce0cb68e9b8e8578c0ed393c4224260e655920636acf0c133470228520d2e54`.
- `shop-ready-warm-wide` restarts that build at 1360x630 with the completed
  match's 483-configuration cache. All four selection transitions and purchase
  return pass, exit 0 after **18.8 s**. The first captured frame shows the complete
  panel and no unused description at either edge. It uses the same QA hash.

All **31 unit tests pass**, including executed native carousel positioning at
4:3/16:9/ultrawide and actual renderer specialization priority selection. Both
tests reject the old behavior. The Windows production/headless and 173-unit
Android native release builds pass; both Port-local renderer archives were
actually rebuilt. Decomp and shared Aurora checkouts were not edited. No APK
has been assembled/uploaded for these changes, and Android device play is untested.
Production Windows executable SHA-256:
`adbe65342f319bebaf3eff0d3357da60752e50ff07fd72d965af0640fad498ef`.
Unstripped Android game-library SHA-256:
`b818ec9d8d4ddfa6358fdf240e116f07af7ca0df9e9c643f2b3f23be0ba8dc09`.
Source is preserved in `build/checkpoints/shop-first-use-verified/`; the preceding
board-cycle source remains in `build/checkpoints/full-board-cycle-verified/`.
Original user saves and the paired pre-fix quick-state executable are unchanged.

## Full board cycle repairs (2026-09-06, not yet published)

Two fresh 20-round Towering Treetop matches now complete all **80 player turns** each,
including the natural night-time Last Five Turns event, and reaches the existing
unavailable-results stub. It returns to mode select, which is visible in the
reviewed captures. This is **not a working results-screen certification**:
`mdpresultdll/mdpresult.c` contains partial decompiled functions, but no native
`ObjectSetup`/prolog. It was not reconstructed or replaced with invented results.
The decomp checkout and shared Aurora are unchanged.

Port-side fixes:

- `game/data.c`'s direct short archive reader now decodes the big-endian file
  count, selected offset and next-file offset with `be32`, just like the other
  archive-reading paths. The previous native signed load could reject valid
  character animation data (the recorded capsule-41 crash at `0x00f30027`) or
  issue a corrupt DVD read. No asset substitution or missing-animation fallback
  was added.
- Capsule 41 is a **duel minigame event**. It now honors the existing zero-native-
  minigames stub before opening a wager selector, debiting coins/stars, resetting
  board positions or fabricating a draw. It posts the existing duel-skipped
  notification and returns through normal capsule-process cleanup. Its original
  event implementation remains available when the minigame count is nonzero.
- The Boo fade material hook is also used by W01's night gate. Its non-one-texture
  material branches previously returned without setting material state, leaving
  an untextured surface with the preceding surface's UV-dependent fade shader.
  Those branches now select their normal textured/untextured TEV setup. The
  one-texture fade and the gate/Boo event still execute.
- The minigame stub continues the board in place, so the original end-of-match
  check at the next overlay load was not necessarily reached after round 20.
  `mbNextTime` now performs that same party-mode completion check before another
  day/night transition, requests normal board exit, and does not play round 21.
- The cache namespace is now `gpu-cache-v3`: old root/v2 caches are retained,
  including invalid Boo configurations created before this repair. Saves and
  settings remain at their existing locations.

Evidence under `build/board-qa-runs/`:

- `full-cycle-duel-data-after`: targeted real duel handler returns, then the
  player turn completes. Captures reviewed. This intermediate build repaired the
  archive reader but had not yet added the pre-wager stub gate, so its old draw
  dialogue is not the final behavior.
- `full-cycle-data-after-soak`: failed in round 4/night with another
  `unmapped vtx attr 13`. `full-cycle-render-trace2` reproduced this and identified
  `ev_CapTeresaFadeMatHook` as the UV-dependent state setter. Neither run passes.
  `full-cycle-render-trace` was deliberately stopped because its requested GX
  diagnostic objects were not yet selected by the QA builder; it is not a pass.
- `full-cycle-fixed-soak1`: **80/80 turns**, 45 capsule calls/returns, 15 complete
  placements, 14 slides, 5 springs, day/night, Last5 begin 106574/end 108908.
  Round 20's final player turn ends at tick **144849**; normal board teardown is
  observed at **144872**; the actual results stub is entered at **144876**, then
  mode select renders. Exit 0 after **231.1 s**. No round-number, dice, inventory,
  branch or capsule-outcome fixture was used; only all-CPU control and ordinary
  A-button input. Four duel encounters report the minigame stub. The full-match
  validator passes. QA SHA-256:
  `ca9e9273e293fba294ae12782f94e0a60eeb369c14f1566e11c553d5b2d53a71`.
  This run used the repaired gameplay with the intermediate v2 cache namespace.
- `full-cycle-fixed-warm`: the final v3 build, seeded with the production boot
  cache, also completes **80/80 turns**. All 36 capsule calls return; 13 placements,
  13 slides, 6 springs and 3 Boo fades begin/end. Last5 runs at 96050–98372;
  the final turn ends at **139614**, board teardown at **139637**, and the results
  stub at **139641**. Exit 0 after **216.3 s**. The full-match/Boo event gates pass;
  captured Boo fade, dialogue and reappearance are visually intact. This is a
  warm-start test, not a claim that every board shader was already compiled.
  QA SHA-256: `432960b5b17fa585df063868e493af111906adb02603949c963f78fa1fe17339`.
- `full-cycle-boo-cache-recovery`: the production v3 executable starts with a
  copied pre-fix poisoned v2 cache, runs 20,000 ticks and exits 0 (29.9 s).
  This is a cache-recovery boot test, not a completed board match. Production
  SHA-256: `e8772c01eebbd0ce9c84143b4eb6ff6f2d646530d27f79224bfdb2a97ee81ba7`.

The full-match gate requires exactly the ordered 80 turns from round 1 through
20, Last5/day/night, no played round 21, normal board completion and actual entry
to the unavailable-results stub. Merely reaching round 20, setting a round
fixture, or exhausting a tick/time budget does not pass it. New native tests
execute the actual archive-reading, fade early-return and board-continuation
functions; each rejects its original broken implementation. The suite has
**29 passing tests**. Windows production and headless builds and all 173 Android
native release units build; no APK was assembled/uploaded and Android device
gameplay is untested. The earlier sound-loop report is still open.

Source before these edits is preserved in
`build/checkpoints/before-full-board-cycle-fixes/`; the two-match verified source
is preserved in `build/checkpoints/full-board-cycle-verified/`. The original card save,
quick-state and matching pre-fix executable remain unchanged. Broader historical
port recovery, other boards, all capsule branches and the results screen remain
outside this successful board-cycle gate.

## Last Five Turns rendering crash (2026-09-05, not yet published)

The recorded `unmapped vtx attr 13` crash is repaired locally. The guide fade
hook in `board/snpc.c` transformed mesh UVs with a matrix constructed from model
positions, camera inverse, world-space fade-plane position and rotation.
Twila's untextured `rod` and `sphere` materials have `GX_VA_TEX0 == GX_NONE`, so
requesting that input aborts native shader compilation. The port-local patch
changes the fade texgen input to `GX_TG_POS`. The fade, guide, roulette and its
outcomes remain active; no event or renderer assertion was stubbed.

A second part of the same failure persisted in the graphics cache. Even with
the fade code repaired, a copied old cache caused asynchronous compilation of
the old invalid configuration to abort a later launch. The port now selects
`gpu-cache-v2` beneath the existing cache root (or `MP6_GPU_CACHE_PATH` test root)
(superseded by v3 in the 2026-09-06 repair above)
and creates it before Aurora starts. Old cache files are preserved, not deleted;
the user/save/settings paths do not change. Failure to create the new directory
is reported rather than silently falling back to the poisoned cache. The first
launch needs to rebuild graphics caches.

Evidence under `build/board-qa-runs/`:

- `last5-current-soak`: with the particle repair already applied, 60 player
  turns completed, then round 16/night still aborted with the original shader
  error. This establishes that the particle fix alone did not repair Last5.
- `last5-night-before`: after a real day/night reload, the targeted call logs
  Twila's untextured rod/sphere at tick 12914 and subsequently aborts with the
  same shader error.
- `last5-night-after`: the repaired event returns at tick 15205; the capsule
  roulette outcome and following player turn complete. Captures reviewed.
- `last5-transition-after`: an explicitly accelerated fixture starts round 15
  on the final day round. Normal round-end/day-night logic enters Last5 in
  round 16 at tick 12393 and returns at 15297, including the Bowser outcome.
  Twelve player turns complete across rounds 15-17 and round 18 is reached.
  This is a transition regression, **not** a fresh full 18-round match.
- `last5-day-after`: the daytime guide/roulette sequence and subsequent turn
  complete; the 40-coin roulette outcome is visible in reviewed captures.
- `last5-old-cache-long-before`: the production executable with only the fade
  repair still aborts on a copy of the preserved legacy cache after 14.8 seconds.
  The earlier 100-tick `last5-old-cache-before` run ended too soon to expose it.
- `last5-old-cache-after`: the production executable with cache generation
  isolation reaches its 20,000-tick budget, exit 0, using the same legacy seed.
- `last5-night-cached`: a warm-cache night event returns and five player turns
  complete; its new cache contains 293 GX pipeline records. QA executable SHA-256:
  `9ed8d970372647d9f9b8b0e4f997a9b53e045717eeca9768ec1cd8fb2219492e`.
- `last5-restart-after-event`: the production executable restarts with that
  populated cache (including the repaired Last5 shaders), reaches 20,000 ticks
  and exits 0. This and the recovery boot are startup/cache tests, not completed
  board-match gates.

The daytime, targeted night, transition, and cached-night runs pass the QA
event/completion/vector gates. QA now creates its cache directory; earlier
isolated runs without that directory disabled caching after SQLite open errors.
`--cache-seed` copies only graphics-cache files inside the ignored build test
area, including the current generation for warm-start coverage.

All **24 unit tests pass**. The fade test executes the actual patched function
with mocked GX calls, tests textured and untextured materials, checks the fade
plane transform and texgen/stage selection, and rejects the original function.
The cache selector test executes the actual bridge function with filesystem
stubs for Windows/Android-style roots, overrides and failure handling. Windows
production, headless, and all 173 Android native release units build successfully.
Android device gameplay remains untested. Published APK 0.4.4 does **not** contain
this repair or the particle repair below; no new APK was assembled or uploaded.

Two longer runs still fail **outside** the Last5 handler:

- `last5-after-soak`: 42 turns complete, then capsule handler 41 in round 11 does
  not return after entering at tick 69067. The 240-second bound ends the run.
- `last5-transition-warm`: Last5 returns at tick 15300 and three round-16 player
  turns complete, then capsule 41 enters at tick 17497. Loading data `0x00f30027`
  reports a missing file/malformed model before an access violation. This whole
  run is **not** a pass despite its successful Last5 event and warm startup.

These capsule-41 failures were open at this checkpoint; the 2026-09-06 section
above records their repairs and a completed board cycle. The earlier sound-loop
report remains open. Source is preserved under `build/checkpoints/` in
`before-last5-render-fix` and `last5-render-fix-verified`. Original saves and the
paired old quick-state executable are unchanged; decomp/shared Aurora are not
modified.

## Capsule particle distortion (2026-09-05, not yet published)

Reproduced the red Mushroom's screen-spanning white/cyan strips in the Windows
GPU build. The ray vertices and colours are arrays embedded inside
`CAPEFFRAYPARTICLEWORK`, not standalone heap allocations. Their addresses miss
both the exact-pointer registry and the allocation-header lookup, causing a
zero-byte GPU upload. Display-list replay does not call the port's `GXBegin`
size-learning hook, so the real data never replaces that empty binding.
The shared glow, crack and trail paths also bind colours inside larger records.

Port-local compatibility changes now bind explicit byte spans before replay:
192 bytes for the ray vertices, 32 for ray colours, and the actual position/UV
arrays and first-to-last strided colour spans in the other three draw paths.
The new bridge entry point retires stale learned bindings without retaining
interior pointers in the persistent registry. No particle scale, motion,
animation timing or effect logic was changed. Decomp and shared Aurora were
not edited.

Evidence under `build/board-qa-runs/`:

- `particle-before-mushroom/frames/f000008.png` (tick 11104) shows the oversized
  strips. `particle-after-mushroom/frames/f000008.png` (tick 10864) shows compact
  rays at the Mushroom instead. The after contact sheet also shows restored
  gold/white sparkles. Both use the same capsule fixture, but randomized roster
  and startup timing differ: this is not a pixel-identical replay.
- `particle-after-mushroom`: capsule handler returned and the turn completed.
- `particle-after-placement`: real stick/A placement of a Zap Orb completed
  destination confirmation, impact and throw cleanup. Captures show the space's
  compact sparkles and the subsequent dice/turn sequence.
- `particle-after-piranha`: the trap handler returned, the visitor lost five
  coins and the owner gained five; smoke and event captures were reviewed.
- All three after runs passed `check_board_qa.py`, with requested event markers,
  completed turns and finite/bounded player/camera vectors. Their QA executable
  SHA-256 is `cb90272586817e0c1b7cd4c17e2a7ce006277ccd9315a72da02c3760f8d0901f`.

All **22 unit tests pass**. Native tests execute the actual patched draw bindings
against the original data layouts, check exact colour extents at 1/32/192
particles, reproduce the old zero-byte embedded-array bind, and verify that
later learned-size growth cannot replace the explicit span. Windows production,
Windows headless, and the 173-unit Android native release build passed. The
regular Windows executable was rebuilt with the repair. Android gameplay is
untested; no new APK was assembled or uploaded, and published 0.4.4 does **not**
contain this particle repair.

QA capture options now support a trigger, frame count and stride. Initial
`particle-before-piranha` and `particle-before-piranha-frames` attempts produced
no frames because their trigger was not routed; they are not visual evidence.
The later `particle-before-piranha-capture` and `particle-before-placement`
captures succeeded. User saves and the old matched quick-state binary remain
unchanged. Source checkpoints are `before-capsule-particle-size` and
`capsule-particle-spans-verified` under `build/checkpoints/`.

Follow-up audit findings, not repaired or claimed as gameplay failures here:
`capthrow.c` declares/calls `mbev_CapEffCapLoseAdd` with height/count reversed
relative to its owner, and `captrap.c` declares `mbev_CapEffElectricModelSet`
with a narrower model-id argument. At this particle-only checkpoint the sound
loop and night Last Five Turns reports remained unresolved; the later Last5
repair is documented above. These targeted runs do not certify every capsule
or a complete match.

## Capsule-space crashes and looping-sound investigation (2026-09-05)

Fixed port-local call signatures in `capsule.c`, `capthrow.c`, and `captrap.c`:
`mbev_CapEffRingAdd` takes vectors/colour by value, whereas RingHit, Explode,
DustHeavy and PlayerMoveVelSet take vector/colour pointers. Mixing these calling
conventions is invalid; AArch64 passes homogeneous vector aggregates in FP
registers, and a colour word interpreted as a pointer also crashes Windows.
All affected callers now match the actual `capevent.c` definitions, including
the trap call that previously bypassed argument checks with an unprototyped cast.
No capsule or animation was stubbed to avoid these calls.

`tests/test_capsule_call_abi.py` compiles the real patched cross-file declarations
against their definitions for Windows x64 and freestanding AArch64. Its negative
control rejects the original declarations. All **21 unit tests pass**; Windows
production and Android native release builds passed. No Android device is
connected, so this is not an Android gameplay-test claim.

New evidence under `build/board-qa-runs/`:

- `land-capsule-before-12`: real Piranha Plant space event crashes with Windows
  access violation `0xC0000005` after entering the handler.
- `land-capsule-after-12`: that event returns, transfers five coins from the
  visitor (10 to 5) to the Orb owner (10 to 15), and completes the turn.
- `night-placement-after-21-pad0`: after a real day-to-night transition, human
  stick/A input places a Zap Orb on space 28 with owner 0. Impact animation,
  throw cleanup, and the fifth player-turn completion all pass with finite
  camera/player coordinates.
- `space-capsule-before-10-stick`, `space-capsule-before-20-stick`: actual human
  stick/A input confirms a destination and completes a fresh Spiny/Podoboo throw.
  These already passed on Windows before this repair; they do not disprove the
  ARM64 calling-convention defect.
- `replace-capsule-before-12`: replacement of an existing Goomba Orb completed
  on Windows before the repair.
- `audio-board-cycle-trace`: post-fix run completed 24 player turns through round
  7, day and night, two CPU throws and five returning capsule handlers. All 30
  traced looping sound voices had an end record. It is a bounded smoke test,
  not full-match or every-capsule certification.

The user's sound-loop report is **still unresolved**. `star-sound-realtime-before`
ran at 60 Hz: Star growth sound 1095 was explicitly stopped after 112 ticks;
1096 and 1097 ended as one-shots after 159 and 94 ticks. Star purchase/relocation
in `star-buy-sound-before` also stopped its looping voice. These observations do
not identify the user's possibly different effect, platform or game state.
No blanket sound timeout or speculative audio change was applied.

QA now accepts `--input-script` and `--audio-trace`; fixtures include `land-orb`,
`replace-orb`, `star`, `star-buy`, and `night-placement`. A build-ready hash check
rejects a stale QA executable after a failed build or changed fixture source.
The first `night-placement-after-21` run used an older fixture and is **daytime
only despite its name**. `night-placement-after-21-verified` reached night but
timed out because its newly human player had the wrong scripted controller port;
it is not counted as a game pass or deadlock. The fixture now assigns pad 0.

The pre-repair source checkpoint is `build/checkpoints/before-space-capsule-fix/`.
Original memory-card data, old paired quick states, and the decomp repository
remain unchanged.

## Reconstructed in the port

- W01 bridge, slide and spring curve callbacks now use a typed native signature.
  The Bezier adapter supplies the correct three-pointer call; Hermite callbacks
  no longer pass through `u32`. Retail curve equations remain unchanged.
- Donkey's event object is allocated/copied with `sizeof(CAPWORK)`, not the
  3,060-byte console size. Koopa's squished-player array uses a named native
  layout member, and the duel guide cleanup uses its actual local object pointer
  instead of a fixed byte offset.
- Native animation registry storage now lives in the captured game arena, so
  restoring globals does not restore a pointer to uncaptured libc storage.
- Hidden console input is disabled and blurred; deferred focus requires the
  console to be visible and open. Android's additional system/Activity IME
  gating described by the private builds is **not yet reconstructed or tested**.
- GX-array registrations are retired on direct/bulk free, successful resize,
  and heap initialization. Authoritative model/allocation binds invalidate old
  learned bindings; only currently indexed attributes receive learned-size
  updates. Rebinding an unknown address discards the old size hint.
- QA runs use separate shader caches via `MP6_GPU_CACHE_PATH`, in addition to
  isolated memory-card, settings and quick-state paths. This prevents a failed
  diagnostic shader from being replayed during the player's next startup.

Validation: **20 unit tests passed**, including native execution of the actual
patched curve functions, capsule layouts, animation registry, and GX-array
binding/lifetime code. Windows GPU and headless release builds passed. Android
native compilation and unsigned release APK assembly/lint passed; lint reports
no new issues, with its existing 53-error/42-warning baseline still present.
No Android device was available: compilation is not gameplay certification.

| Reconstruction run (`build/board-qa-runs/`) | Observed result |
| --- | --- |
| `reconstruct-bridge-01` | Real bridge traversal and turn completion; finite camera. Bridge length 2,378.798 vs typed numerical reference approximately 2,378.825. |
| `reconstruct-slide-visual` | Real slide begin/end; post-slide 3D board visually visible. Intentionally held after the event for inspection, then ended by the wall-clock limit; not a normal-completion gate. |
| `reconstruct-spring-02` | Actual spring **and** slide animations ran and ended; finite vectors and normal turn completion. Spring length approximately 2,594.340 vs reference 2,594.350. |
| `reconstruct-donkey-01` | 3,360-byte allocation/copy; callback ran, capsule handler returned and turn ended without the previous access violation. |
| `reconstruct-red-01`, `reconstruct-triple-01` | Mushroom effects and completed turns, without the previous invalid camera state. These are new routes/seeds, not an exact replay of the historical total-24 case. |
| `reconstruct-night-01` | Targeted day-to-night handoff completed. |
| `reconstruct-save-01`, `reconstruct-load-01` | Production executable saved 18 regions, then a separate process of that same executable restored them and ran to its later tick budget. Binary compatibility checks remained enabled. |
| `reconstruct-cpu-soak-01` | 24 completed player turns, round 7 reached, both day/night states and three completed slides; no invalid player/camera vectors. |
| `reconstruct-cpu-soak-02` | After the array repair: completed 15 rounds, then aborted during Last Five Turns in round 16: `aurora::gfx::gx: unmapped vtx attr 13`. **Not a pass or a stable full match.** |
| `reconstruct-last5-02` | With an isolated fresh cache, a targeted **daytime** Last Five Turns call and subsequent normal turn completed. Does not clear the round-16/night failure. |
| `reconstruct-release-fresh-cache` | Current production Windows executable booted and ran to 11,000 ticks after cache isolation, exit 0. |

`check_board_qa.py` verifies clean process exit, completion markers, requested
event coverage and finite/bounded player/camera vectors. It rejects the held
visual fixture and failed extended soak rather than treating runner exit 0 as
a gameplay pass. It cannot certify rendering or the correctness of every effect.

The Last Five Turns shader configuration persisted in the formerly shared
pipeline cache and caused subsequent startup precompilation failures. The three
derived cache files were moved, not deleted, to
`build/checkpoints/reconstruction-last5-cache/` with a SHA-256 manifest.
The original user memory-card save and paired old quick-state executable remain
unchanged. Old states must still be opened with their matching old executable.

### Still missing or unverified

The later native shadow renderer, caster staging-copy/receiver corrections,
lighting/terrain changes, per-bind texture filtering, durable crash reports/F11
exports and the remaining Android IME/JNI changes have **not** been restored by
this batch. Shop/wipe behavior and message-glyph parity still need a dedicated
comparison. Existing live settings and minigame skipping are preserved, not
evidence that every later private-build fix is present. The Last Five Turns
shader abort and capsule 41 are now repaired, and two complete Towering Treetop
board cycles pass as documented above. All trap/event branches, other boards,
and the actual results screen remain unverified or unavailable.

## Missing post-0.3 port history

These results describe this checkout, not the latest historical private build.
The public source snapshot `ce34aa5` identifies development revision `4681241`
(v0.3.0). The later local decomp update did not import subsequent port work.
The [August 3 private build](https://github.com/iirg4x/mp6-android-builds/releases/tag/decomp-6c44b33e-20260803)
explicitly lists the slide blackout/bridge ABI root-cause fix, crash logging,
F11 snapshots and a savestate animation fix. Therefore the current camera
failure is a missing prior port fix, not a newly discovered historical defect.

Later private release descriptions also record message-glyph rendering,
per-bind texture filtering, Android keyboard gating, live settings, native
shadow mapping and its staging-copy fix, rendering improvements, and shop/wipe,
JNI startup and GX-array lifetime corrections through v0.4.0. The current
0.4.1/0.4.2 preview labels do **not** establish that these changes were included:
both were built from `2a62946` plus local changes to the older port snapshot.

Recovery check on 2026-09-05: public origin still ends at `ce34aa5`; the private
source repository and its surviving local clone end at `06c681b` (July 31).
The v0.4.0 source revision `b7732f5` is absent from that GitHub repository.
The original local-only `port/mp6-native` development path is missing. No
later source checkout or source bundle was found in the inspected port
locations/backups. Release APKs and descriptions are evidence of the missing
features, not a replacement source snapshot. The user authorized reconstruction;
the first implemented batch is listed above. No Git fast-forward was possible,
and no replacement of the entire production source tree was performed.
The starting source tree is retained byte-for-byte under
`build/checkpoints/reconstruction-start/`; the completed critical batch is
retained under `build/checkpoints/reconstruction-critical/`. Both include source
file SHA-256 manifests; neither checkpoint replaces or cleans the dirty Git tree.

## Historical pre-reconstruction blockers

### Bridge/path calculations corrupt the player and camera

The bridge black screen was reproduced visually: the HUD remained visible but
the entire 3D board disappeared. This is not a stuck fade.

In `world01.c`, `W01CurveEval` is an unprototyped callback. The path integrators
pass four pointers and a promoted floating-point argument, but the Bezier
callback `fn_1_14A90` takes three pointers and a `float`. Those calls are not
compatible with the native ABI. The W01 compatibility patch at diagnosis time
did not correct these callback signatures. The reconstruction above now covers
both Bezier and Hermite paths.

Measured on the initialized board:

| Path | Actual computed length | Typed direct numerical reference |
| --- | ---: | ---: |
| `fn_1_13CC` bridge | 26,044,050,322,751,488 | approximately 2,378.83 |
| `fn_1_3214` spring | 197,681,779,673,399,296 | approximately 2,594.35 |

The reference is a 100-segment numerical integral using the real, directly
called derivative function. It is diagnostic only and does not replace any
game calculation. Starting the human player on bridge space 19 led to player
and camera Y coordinates around `1.96e27`. The visual case paused the player
coroutine **after** that corruption to keep the already-broken view available
for inspection; the renderer continued running.

There is also an organic long-movement failure: capsule 1 produced three dice
and a total of 24, but the player became NaN on space 36 and the camera remained
NaN through turn end. This is a confirmed movement/camera failure; the exact
route-specific function behind that instance has not yet been isolated.

Evidence under `build/board-qa-runs/`:

- `bridge-02/game.log`: targeted traversal, out-of-range camera without a hold.
- `bridge-visual/game.log`: matching HUD-only black-screen inspection.
- `capsule-matrix-01/game.log`: three dice followed by invalid camera state.

### Donkey Kong event reads past its native allocation

Natural play crashed in the Donkey event in round 2 or 3. A separately targeted
Donkey call also crashed. Windows exit code was `0xC0000005` (access violation).

`capspecial.c:ev_CapDonkeyStart` allocates and copies a hard-coded `0xBF4`
(3,060 bytes) for the `CAPWORK` object handed to `ev_CapDonkeyOMExec`. Native
`sizeof(CAPWORK)` is 3,360 bytes. The callback's first model ID is at offset
3,188 and the process field is at offset 3,284: both lie beyond the allocated
and copied block. The callback executes and reads that incomplete object.
The port needs a native-sized allocation/copy and a review of similar literals.

Evidence: `baseline-01/game.log` (unmodified release), `red-01/game.log`,
`cycle-02/game.log` (size/offset probe), and `donkey-target-01/game.log`.
The size defect was confirmed in that pass; the repaired run is recorded above.

## Historical baseline coverage and limits

| Case | Observed result |
| --- | --- |
| Capsule 0 / Red Mushroom | Consumed; `diceNum=2`; totals 4 and 11 in separate runs; real movement and turn end completed. |
| Capsule 1 / triple-dice mushroom | Consumed; `diceNum=3`; total 24. Turn ended with a broken camera: **not a pass**. |
| Capsules 2, 3, 7 | Consumed; expected source dice modes 3, 4, 6 were set; rolls and turns completed. Special targeting/protection behavior still needs dedicated checks. |
| Capsule 4 / Bullet Bill | Real movement handler ran; player coins increased from 10 to 40 and the three co-located opponents went from 10 to 0. Turn completed. More separated-player cases remain. |
| Capsule 5 / Warp Pipe | Same-start-space case was inconclusive. Separated-player case swapped spaces 64 and 18 and then completed a roll and turn. |
| Capsule 6 / Wiggler | Moved player from space 64 to space 6; subsequent roll and turn completed. Star purchase outcome was not validated. |
| Four-player round and minigame handoff | Completed a round and used the intentional `minigame.skipped` path. No minigame implementation was tested. |
| Day to night | Advanced the initial time counter to one round before night, used four CPUs, completed all four day turns, reloaded the night board and completed the first night turn with finite camera coordinates (`day-night-02`). Not a full unmodified six-round soak. |
| Orb-placement wait | Inspected the stalled window: it was asking where to throw an Orb. A-only automation had not selected a destination. **Not classified as a game deadlock.** |

Thrown/trap capsules, every event branch, complete match/endgame, night-to-day,
Modern/enhancement settings and real-time pacing remain unverified. Runs used
automation defaults and free-run timing, not the user's saved launcher settings.
No Android device was connected; these results do not certify Android gameplay.

The existing 14 unit tests still passed, illustrating their narrower coverage.
Original memory-card and quick-state files were verified unchanged. Decomp and
shared Aurora sources were not modified. Fresh test data and logs remain under
ignored `build/` paths; no game assets or saves were uploaded.

## Reproduce

Run from the port root after a normal Windows release build. The builder defaults
to the port's extracted disc/include cache; existing `MP6_DISC_ROOT` and
`MP6_DECOMP_INC_DATA` values can override those paths. Use a **new** run name;
the runner refuses to overwrite a prior run. Do not distribute the QA executable.

```sh
python tests/integration/build_board_qa.py
python tests/integration/run_board_qa.py --name red-new --capsule 0 --one-turn --seconds 60
python tests/integration/run_board_qa.py --name bridge-new --route bridge --hold-bad-camera --seconds 65
python tests/integration/run_board_qa.py --name donkey-new --capsule 44 --one-turn --seconds 60
python tests/integration/run_board_qa.py --name warp-new --route warp-separated --capsule 5 --one-turn --seconds 60
python tests/integration/run_board_qa.py --name night-new --route day-night --seconds 100
python tests/integration/run_board_qa.py --name spring-new --route spring --one-turn --seconds 65
python tests/integration/check_board_qa.py build/board-qa-runs/spring-new --event spring.begin --event spring.end
python tests/integration/run_board_qa.py --name soak-new --route cpu-soak --stop-round 17 --seconds 180
python tests/integration/run_board_qa.py --name full-new --route cpu-soak --stop-round 0 --seconds 420 --ticks 250000 --input-script "period:30;timeout:249000;pressuntil:a/qa.complete/done;wait:60"
python tests/integration/check_board_qa.py build/board-qa-runs/full-new --full-match
python tests/integration/run_board_qa.py --name shop-new --route shop --widescreen --window-size 1360x630 --capture-frames 180 --capture-stride 4 --capture-trigger shop.ready --input-script "period:30;timeout:49000;pressuntil:a/qa.shop/ready;wait:120;stick:right;wait:90;stick:right;wait:90;stick:left;wait:90;stick:left;wait:90;press:a;pressuntil:a/qa.complete/done;wait:60"
python tests/integration/check_board_qa.py build/board-qa-runs/shop-new --min-turns 0 --event shop.ready --event shop.selected --event shop.end
```

The bridge fixture chooses the first bridge marker from the real space links;
it does not substitute an animation. Capsule 44 invokes the real Donkey handler
from the player coroutine rather than inventing an obtainable inventory Orb.
`run_capsule_matrix.py` runs IDs 0 through 7 sequentially and prints the relevant
state snapshots; its fixed run-directory names also refuse overwrites.

For unmodified-release play, specify `--exe build/release/mp6native.exe` without
fixture options. The normal binary has no QA completion marker and ends only
at its tick budget, crash or the runner's wall-clock limit. Inspect individual
state changes and the rendered view before assigning any gameplay verdict.
