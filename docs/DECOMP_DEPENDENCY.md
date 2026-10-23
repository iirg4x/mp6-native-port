# Decomp Dependency

The Mario Party 6 decompilation checked out at
`build/deps/marioparty6` (tracking `main` in
[`iirg4x/marioparty6`](https://github.com/iirg4x/marioparty6), not the original
upstream repository) is the source of truth for all recovered
game code. This port does not vendor, fork, or modify that source; it
consumes it read-only.

## How it's consumed

Setup creates a clean, detached source checkout in the ignored, port-owned
`build/deps/marioparty6` directory. Compilation and patch application read
it without modifying it. `MP6_DECOMP_DIR` can explicitly select another clean
checkout at the same pin; relative overrides resolve from the port root.
The shared `../../external_refs/repos/marioparty6` development checkout is
not updated or changed. This keeps port dependency updates independent of
ongoing decomp work. No game assets are fetched with the dependency.

## Currently tracking

```
8d1bfae112fdd1f48fa7246d1dc162a86fd334d6
```

This must match the output of the following command, run from the Port
repository root with the dependency checked out at the pinned revision:

```sh
git -C build/deps/marioparty6 rev-parse HEAD
```

The dependency was advanced on 2026-10-23 from
`ef257a7e3e9d2ab58d5372653d5d278fdce982f7` to the verified `main` revision
above. Its report marks 89 REL modules complete: 48 numbered minigames, 13
modes, 11 boards and 17 other system/support overlays. This update adds Light
Up My Night (`m617dll`), Trap Ease Artist (`m620Dll`), Cashapult (`m642dll`),
Verbal Assault (`m665dll`) and Shoot Yer Mouth Off (`m666dll`). Talkie Walkie
(`m667dll`) was finished upstream: its code moved between units and its
immediate-mode trail now ends with `GXEnd` itself, so the Port's three
`m667dll` source patches were carried across by hand and its reset lists
follow the moved data. The original `mariopartyrd/marioparty6` repository
had nothing newer that `main` lacks (its latest work, `mic.c` and the GameSpy
symbols, is already recovered here). The 2026-10-21 update (from
`9800bdd`) added Daft Rafts (`m609dll`), Blooper Scooper (`m619Dll`),
Insectiride (`m647dll`) and Boo'd Off the Stage (`m660dll`). The 2026-10-20 updates
(from `fd4bf9b3` via `73a58c0b`) added Snow Brawl (`m626Dll`), Tally Me
Banana (`m674Dll`), Mowtown (`m613Dll`), Pier Factor (`m676Dll`), Sink or
Swim (`m625Dll`), Rocky Road (`m644dll`), Strawberry Shortfuse (`m680Dll`)
and Speak Up (`micquizdll`); upstream then also stopped
including `game/charman.h` from `actman.h`/`seqman.h`; the 17 Port patches on
the files that change were carried across with a three-way merge, unchanged in
content. Source completion
does not imply native availability; the integration inventory tracks that
separately. Upstream source remains read-only. The native patch queue preserves
host pointer widths, big-endian asset decoding, widescreen, AO, interpolation,
and explicit native equivalents for recovered PowerPC helpers.

### Integration boundaries

- Upstream owns recovered gameplay and its typed data. Minigame Mode now uses
  `menu_setup.c` and `particle_effects.c`; the obsolete 98-unit inventory and
  disc-derived data mirror have been removed.
- Struct layouts must agree with the GameCube natively wherever recovered
  code views one object through two definitions or by byte offset.
  `tools/layout_audit.py` compiles every source for a GameCube-like 32-bit
  layout and both native layouts (Windows, and Android, where `BOOL` is one
  byte), and reports opaque pads standing in for pointers and GameCube byte
  offsets whose field moved; `tests/test_layout_audit.py` keeps it at zero.
  Size a skipped span with `offsetof` on the owner's struct, and reach fields
  by name.
- `tools/native_namespace.py` owns deterministic REL-local symbol names.
  Source inventories are explicit and lazy: importing build tooling does not
  require a decomp checkout or disc.
- Native reload behavior belongs to each source's explicit data/BSS owner
  list, using `include/mp6_rel_state.h`. It captures native initial values
  before first entry and restores them on re-entry, never per frame. This
  resets storage only; the existing `omOvlKill` path owns process, model,
  sprite, actor, and `HU_MEMNUM_OVL` allocation teardown before re-entry.
  Each list is port code, kept out of the decomp patch in
  `src/rel_state/<decomp path>.inc`; `tools/apply_patches.py` appends it to the
  generated source so it can name the file's statics. The lists come from the
  compiler: `tools/gen_rel_state.py MODULE` writes them for a new module, and
  `--check` (run by `tests/test_rel_state_lists.py`) fails when a list no
  longer matches the source's mutable file-scope variables.
- Recovered non-minigame modes use the lazy `tools/native_modes.py` inventory.
  Endurance Alley (`mgmrenshodll`) has seven source/reset owners; the bridge
  invokes its aggregate reset before the real prolog. Preview capture keeps
  its native dimensions and uses the shared centered framebuffer origin.
  Both immediate quads have
  explicit native primitive submission. The common unavailable-game return
  skips Endurance's pending result state rather than inventing an outcome.
  Verified cave rims use the shared static-strip extension; native surfaces
  remain unchanged and appended vertices update only on aspect changes.
  Compilation and widescreen audits share `build.native_rel_sources()` so
  newly registered modes cannot silently disappear from the audit inventory.
- Original-disc THP video now uses the streaming `THPSimple` adapter with the
  pinned `stb_image` JPEG decoder. The bus preview requests volume zero; video
  audio mixing is not implemented. Missing-video fallback is an open presentation
  issue; the extracted content must include the original movie assets.
- The DLL bridge registers lifecycles; it does not implement gameplay or
  rendering. New m627, m629, m636, m643, m646, m649, m658, m664, m667 and m675 entries use
  the same registration path. T Minus Five's shared work records and cross-unit prototypes have one
  native owner in its compatibility header; callbacks do not use PPC byte
  offsets or truncate pointers into 32-bit object work slots.
- Pixel Perfect's six state consumers use one typed native record, including
  native-width player, timer and camera pointers. No runtime offset translator
  or per-frame copy is needed. Its shared upstream executor is an explicit
  inventory entry compiled in the module namespace, not a copied entrypoint.
  Four owners reset all 14 mutable globals through the common reset mechanism.
- Jump the Gun uses its recovered typed work records with native `sizeof`
  allocations, not GameCube byte counts. Startup and gameplay own all 21 mutable
  globals; cross-unit declarations live in one shared compatibility header.
  Its inactive debug draw routine uses the existing GX API instead of hardware
  FIFO writes and remains unregistered. No renderer fork or per-frame adapter
  state is added for the module.
- Stamp By Me has one shared native layout for its player, prop, effect,
  light, fog, audio and score records. Allocation sizes, callbacks and pointer
  tables use those types; main owns shared globals and four storage owners
  reset all 59 mutable globals. Typed AI constants replace big-endian byte
  aliases. The compatibility layer corrects a reversed vector-helper call and
  preserves the AI timer query's return value. Its matrix translation call
  uses the existing Aurora implementation through the shared native bridge.
- Talkie Walkie's scene, player, registry and particle layouts have one native
  header owner. Native-sized allocations and typed constant model descriptors
  replace PPC byte counts and big-endian aliases. Two storage owners reset all
  46 mutable globals, while runtime allocations retain normal overlay ownership.
  Its immediate trail rendering uses the shared GX bridge, not a second renderer.
  Shared microphone/controller responses now use native-sized typed records
  throughout the queue and scratch storage; the fix lives in the input system,
  not in a module-specific decoder. Physical speech input remains unavailable.
- Hyper Sniper's collision, attachment, stage and player records have one
  shared native header owner. Consumers use typed fields and native allocation
  sizes instead of PPC byte offsets. A common callback ABI distinguishes stage
  owners from target attachments; target selection enforces pool capacities.
  Eight storage owners reset all 334 mutable globals at entry, not per frame.
  Its gameplay remains upstream-owned and its cameras use the shared adapters.
- Banana Shake's seven source owners share native collision, actor, prop,
  effect, camera, fog and audio contracts. Collision shapes own a common header
  subobject; sphere-only fields are accessed only after the shape check.
  Allocations use native sizes and callbacks retain full-width pointers. All
  85 mutable globals reset at entry through the common lifecycle mechanism.
  The SDK scale/rotation vector-array call adapts the existing scalar Aurora
  operation in the shared GX bridge, including in-place and zero-count cases.
  Recovered interpolation argument order and timer-query returns follow their
  real callers, without per-frame layout translation or a renderer fork.
  Asset-verified widescreen registrations extend only resource 19's static
  sky and shoreline through shared geometry helpers. The sky's full border
  mirrors its boundary art to cover side and lower-edge reveal. Native
  faces, child scenery and collision remain unchanged; the shape-animated water
  mesh is not modified. Registration also happens on cold 4:3 loads so the
  existing aspect-toggle and model-lifetime handling owns restoration.
- Hu3D's shared adapters own widescreen camera mapping, including animated
  split-screen rectangles, AO scopes, and interpolation identities. Module
  patches must not apply a second camera scale. Art-specific geometry changes
  require asset evidence and remain separate from camera mapping.
- Trick or Tree consumes upstream's typed player, camera, timer and particle
  records. One header owns its cross-unit function signatures; allocation,
  clearing and particle strides use native sizes. Three source owners reset
  all 19 mutable globals. An inactive puzzle path no longer reads curve index
  -1. Measured scenery registrations extend only the authored floor and plain
  background gradient through shared helpers, preserving gameplay coordinates,
  native faces and the moving tree pairs.
- Conveyor Bolt uses one cross-unit native callback contract, typed embedded
  contact nodes and native-sized scene/player allocations. Three storage owners
  reset all 11 mutable globals before entry; normal overlay teardown retains
  ownership of runtime objects and allocations. The shared job pool preserves
  full-width owner pointers. Its recovered day/night rules, cameras and scenery
  are unchanged. Source inventories and patches retain upstream's exact path
  casing, including `m631Dll` and `m646Dll`, verified even on Windows.
- Sink or Swim, Rocky Road, Strawberry Shortfuse and Speak Up keep their
  recovered declarations in place, rewritten to match each definition exactly.
  This replaces K&R declarations and narrowed returns (for example `s16`
  results read as `s32`) that only worked through PowerPC register
  conventions. Every sequence and object callback has the parameter list its
  caller uses; no function-pointer casts remain in their tables.
  Sink or Swim declares the Hu3D camera API (float arguments), sizes its wave
  crest list with native pointers and starts its CPU obstacle index in bounds.
  Rocky Road clears its camera record by `sizeof`, not 52 PowerPC bytes.
  Speak Up copies inline window text into module-owned slots through
  `mp6_window_insert_text` instead of packing host pointers into message IDs.
  Small integers in process properties use `intptr_t`, and the PowerPC
  paired-single conversions use the native clamped helpers. Its mode work
  record shares the selector's offsets, which a regression checks.
  Module owners reset 34, 35, 10 and 305 mutable globals respectively.
  Speak Up's namespace aliases exceed Windows' compile command limit, so the
  build moves them into a generated forced-include header for that unit only.
- Snow Brawl, Tally Me Banana, Mowtown and Pier Factor follow the same
  declaration contract, including block-scope declarations and K&R
  definitions (now `(void)`). Their manager and player records are allocated
  by type: several fixed PPC byte counts (Snow Brawl's 48-pointer entry table,
  Pier Factor's collision, score, effect and player records) overflowed
  natively. Immediate-mode effect fans and Tally Me Banana's raw
  write-gather-pipe quads are typed GX calls closed with `GXEnd`. Helpers whose
  retail result was another call's value left in r3 now return it explicitly
  (Tally Me Banana's model cache and sound pan, Snow Brawl's round timer), and
  phase helpers with bare `return;` report their completion flag. MWCC's
  external-inline `tanf` copies are dropped in favour of libm, and the SDK's
  `PSQUATNormalize` has a native equivalent in `src/os/sdk_native.c`.

- Daft Rafts, Blooper Scooper, Insectiride and Boo'd Off the Stage follow
  the same declaration contract (34 cross-unit declarations rewritten to
  their definitions). Boo'd Off the Stage's judge read the round timer
  through a `void` helper whose retail value was `MgTimerDoneCheck` left in
  r3, so natively the time-up check read an undefined value; it now returns
  the check.
  Blooper Scooper's job queue reports success instead of truncating the job
  pointer, actor correction hooks take the pointer-wide `intptr_t`
  parameter, and audio, voice, random and board calls now see their real
  prototypes instead of implicit `int` declarations (several pass `s16`
  values or return pointers). Daft Rafts' dust hook views the engine
  particle through a padded record; static assertions pin its fields to the
  native `HU3D_PARTICLE` offsets. No fixed PPC byte-count allocation holds
  pointers in these modules. Owners reset 38, 10, 80 and 21 mutable globals.

### Native availability inventory (2026-10-21, ef257a7)

All 84 complete modules have selected recovered source and native loader
registrations; the desktop release links 501 translation units. Daft Rafts,
Blooper Scooper, Insectiride and Boo'd Off the Stage each start directly at
the unlocked tick rate, play a full all-CPU round and return to the menu with
no error markers at 4:3, 16:9 and 21:9. Boards play Daft Rafts for real
through the same loader instead of the missing-module fallback. Widescreen
fixes (Blooper Scooper's opening sky, Daft Rafts' falls cliff) are recorded
in [Widescreen QA](WIDESCREEN_QA.md).

### Native availability inventory (2026-10-20, 9800bdd)

All 80 complete modules have selected recovered source and native loader
registrations; the desktop release links 471 translation units. Snow Brawl,
Tally Me Banana, Mowtown and Pier Factor pass the isolated native-entry sweep
with no error markers, and full CPU rounds return at 4:3, 16:9 and 21:9. Their
widescreen fixes (water reflections, Mowtown's results backdrop, Snow Brawl's
sky) are recorded in [Widescreen QA](WIDESCREEN_QA.md).

### Native availability inventory (2026-10-20, 73a58c0)

All 76 complete modules now have selected recovered source and native loader
registrations. The desktop release links 442 translation units.
The isolated native-entry sweep passes Sink or Swim, Rocky Road, Strawberry
Shortfuse and Speak Up: each binds natively, finishes its real prolog and exits
cleanly with no stub or error marker. 16:9 frame captures show each running
its own scene; Speak Up reaches its host introduction. Widescreen review and
fixes for all four are recorded in [Widescreen QA](WIDESCREEN_QA.md). Full
matches, physical microphone input and Android are not certified here.

### Native availability inventory (2026-10-18)

Faire Square (`w03Dll`) now uses the shared native loader and an explicit lazy
board source inventory. Its 70 mutable globals have reload owners; dice callbacks
use typed signatures, and board message insertion preserves native pointers through
queued/live windows and text measurement. The normal desktop release builds, and
4:3, 16:9 and ultrawide captures reach the board and its native Party Results exit.
This is board-entry/lifecycle coverage, not a completed twenty-turn match.

All 72 complete upstream modules now have selected recovered source and native
loader registrations. The desktop release links 422 translation units. This is
source/loader coverage, not full playthrough or visual-audit certification.

The isolated native-entry sweep passes 71 explicitly requested overlays, with
boot verified on every launch: all 72 are covered without a fallback stub.
The 63 board/support/mode units pass strict declared-call,
pointer, integer-conversion and callback checks. Ten focused inventory, reset,
native-layout and text regressions pass. Original upstream sources remain clean.
This does not certify full matches, every menu branch, physical microphone
input, Android device behavior, AO, or widescreen presentation.

| Family | Source/loader coverage | Visual/gameplay validation |
| --- | --- | --- |
| Numbered minigames | 32/32 | Individual evidence; no blanket playthrough claim |
| Modes | 13/13 | Native-entry smoke passed; visual audit deferred |
| Boards | 11/11 | Native-entry smoke passed; full matches not certified |
| System/support | 16/16 | Native-entry smoke passed; visual audit deferred |

`actmanDLL` is now a recovered native overlay as well as using shared actor
services. The boot overlay's `opening.c` is not the separate `openingDll`.
Modules are not enabled just because their source exists upstream. The board
minigame-call stub remains unchanged; the native THP adapter is now available
for original-disc video as described above.

The current Windows graphical Release compiles and links 361 units. This
checkpoint keeps the native lifecycle inventory at 46/72 and includes no W04
board edits. TT Wars adds its recovered game/prolog through the shared mode
inventory, preserving its
upstream configuration/gameplay interface. Its native pointer table, piece
teardown and 45-global reset ownership have executable regression coverage.
The screenshot audit also fixed the shared debug-font palette span and centered
its native framebuffer canvas at wide aspects. All 65 focused checks pass;
captured board/configuration checks and a XeGTAO/interpolation run reach normal
system-exit return. This is not a claim of a completed TT Wars match.

The preceding 356-unit Mount Duel checkpoint
adds eight recovered units, typed cross-unit ownership and seven reset owners.
All 63 focused native-mode, preview, tooling and widescreen checks pass. Captured
menu checks cover 4:3, 16:9 and 21:9; a real Trick or Tree round returns through
the tournament results, and two same-process entries/exits pass with XeGTAO and
actual interpolated presents. The shared backdrop adapter continues only the
sky perimeter at wider aspects. Earlier Conveyor Bolt, Trick or Tree and
Endurance checkpoints remain recorded in `BOARD_QA.md` with their own evidence
and limitations. Android was not rebuilt or tested for these additions.

The checks below describe the preceding 332-unit integration checkpoint.

Windows Release compiles and links 332 units; Android arm64 native Release
compiles and links 334 units with exactly one `-O2` per unit. No APK was
packaged, uploaded, or installed, and no Android device result is claimed.
Hidden graphical checks load m627, m629, and the consolidated Minigame Mode
entrance in 4:3 and widescreen, including AO-on/interpolated presents. The
Minigame Mode backtrack, exit/re-entry and Free Play handoff scenarios pass
with widescreen, AO and unlocked FPS together. Captured settled frames also
verify the new split-screen and Pop Star views with widescreen and AO.
Wrasslin' Rapids (m664) and T Minus Five (m658) also pass hidden 4:3/AO-off
and widescreen/AO-on/unlocked-FPS checks. Their settled captured frames
include interpolated presents. These checks do not claim complete minigame
playthrough coverage.
Pixel Perfect (m636) passes 4:3/AO-off and widescreen/AO-on/unlocked-FPS
rendering checks. Forced-load tests now derive two-team and one-versus-three
assignments from game metadata; normal menu launches keep their own selection.
The focused regression suite passes 119 tests, including native reset/ABI
execution, missing-checkout imports, widescreen, grounding, AO scopes and
interpolation wiring. Executable ABI probes check T Minus Five's pool capacity,
allocation canaries, native pointer payloads and AI fields; Wrasslin' Rapids'
AI rejects invalid target indices before accessing its player array. Both
probes run with undefined-behavior checks. The expanded state-owner check also
covers multi-word type declarations and aligned variables.
Pixel Perfect's executable probe covers native camera/player pointers, voice
position traversal, CPU state, all 48 picture-cell selections, team pixels and
landing callbacks. It checks untouched state bytes for accidental aliasing.
The native adaptation also corrects a player-versus-actor pointer mix-up and
reuses the entry timer instead of allocating an orphaned second timer process.
Jump the Gun (m643) passes hidden 4:3/AO-off and widescreen/XeGTAO-on/unlocked-FPS
checks; captured settled split-screen frames include an interpolated present.
Its native probe executes all nine allocation expressions and the real 60-frame
camera transition for both teams through the shared widescreen mapper at three
aspect ratios, with allocation/state guards and undefined-behavior checks.
Individual owner reset tests verify native data/BSS restoration; the ABI probe
also checks the combined reset entrypoint dispatch. Scene setup, projectile and
rescue callbacks are not covered by this focused executable probe, and the
graphical checks do not certify a full playthrough.
Stamp By Me (m649) passes 4:3/AO-off and widescreen/XeGTAO-on/unlocked-FPS
rendering checks, with a real interpolated present and all four settled views
visually inspected. Its executable probe exercises native allocations and
callbacks, all 64 audio records, the four-player pointer registry, score
storage, AI constants and timer queries. The real opening sequence and camera
callbacks run through the shared widescreen mapper at three aspect ratios,
with undefined-behavior and float-to-integer conversion checks. Empty opening
scissors are handled before conversion to unsigned extents. Matrix translation
also checks separate and aliased source/destination storage.
A separate accelerated, no-input desktop round completes through the normal
overlay teardown and returns to Mode Select with widescreen, XeGTAO and
unlocked FPS enabled. It is an exit-path check, not scoring, human-input,
re-entry, or complete gameplay coverage.

Talkie Walkie (m667) passes 4:3/AO-off and widescreen/XeGTAO-on/unlocked-FPS
rendering checks, including an interpolated present and visual inspection of
the settled stage and HUD. Its executable ABI probe checks scene allocations,
all 135 pooled effects, native pointer callbacks, typed model descriptors,
AI attack-only ticks and the real camera setters at three aspect ratios. The
shared response-queue probe checks the final slot, full-width pointers,
ten-candidate scratch copies and the actual minigame direction callback.
A separate accelerated no-input round reaches normal teardown and returns to
Mode Select. These are not complete gameplay, controller-selector interaction,
physical microphone, or Android device acceptance results. Character-motion
ARAM misses fall back to the existing disc loader; no fatal, GPU-validation or
script errors occurred in these bounded checks.

Hyper Sniper (m646) passes 4:3/AO-off and widescreen/XeGTAO-on/unlocked-FPS
rendering checks, including an interpolated present and visual inspection of
the settled stage, HUD and aiming markers. Its executable ABI probe exercises
native allocations, all 160 collision slots, full-width contact-edge pointers,
stage versus attachment contact callbacks, target detachment, score limits,
worst-case target capacities and the real camera setters at three aspect
ratios. It runs with undefined-behavior and float-to-integer conversion checks.
A separate accelerated no-input round reaches normal overlay teardown and
returns to Mode Select, with widescreen, XeGTAO and unlocked FPS enabled.
These bounded checks do not certify human-input gameplay, re-entry or Android
device behavior.

Banana Shake (m675) passes hidden 4:3/AO-off and widescreen/XeGTAO-on/unlocked-FPS
rendering checks, including an interpolated present. Its native probe executes
collision and region capacities, typed shape callbacks, native allocation
guards, all 64 audio records, fog callbacks, shadow-camera fields, player and
prop initialization, emitter lookups, effect registration and score limits.
It uses undefined-behavior and float-to-integer conversion checks. A separate
accelerated no-input round reaches normal teardown and returns to Mode Select.
These checks do not certify human-input gameplay, re-entry or Android device
behavior.

Newly recovered families remain opt-in, not auto-enabled.
Other boards/modes likewise require their own native integration and testing.
Local diagnostic receipts are under `build/decomp-update-20261018/` (not
distributed or linked as repository documentation).

### Previous update: 2026-10-09

The preceding dependency update advanced to `abd33205633a540e2130a6672c2edf27b52b79e2` from
`52a0209cc553a59a0f60726d2818ccddfd9695fe`. The clean, detached checkout is
port-owned; the shared decomp development checkout remains untouched.
The lead change in this update is the recovered instruction overlay
(`instdll`): its five native units are `prolog.c`, `setup.c`, `stage.c`,
`display.c` and `name.c`; `runtime.c` remains excluded. Native ABI coverage
includes glyph-pointer allocation, process payloads and `GXEnd`. Each native
owner restores initialized data and clears BSS on every load, and the real DLL
bridge route binds `dll/instdll.rel`.
The update also includes the recovered Mini-game Tour / Free Play menu
(`mgmfreedll`), Mode Select updates, and the current m616, m632, m633 and m670
sources.
The verification below belongs to that historical `abd33205` revision,
not to the current dependency pin.

The new menu's two recovered source units are explicitly selected by
`tools/native_mgmfree.py` and bound to their real lifecycle in the DLL bridge.
The PPC runtime wrapper is excluded, as for the other native overlays.
At that historical revision, the THP decoder was unavailable. Native
instructions then used a recovered still-image preview fallback behind an
explicit initialization-failure guard; that historical fallback made no
movie-playback claim. The current streaming adapter is documented above; the
other five `mgm*` modes remain unavailable, and the board minigame-call stub is
unchanged.

Existing Mini-game Tour menu adapters decode the packed player-order table into
host-endian halfwords, replace byte-buffer pointer aliases with typed
host-width pointers, and restore BSS/mutable initial data on every overlay
load. Existing native
patches were rebased without weakening strict patch validation, including the
m632 `sceneModels` rename and Mode Select's AO/widescreen behavior.
Windows graphical Release (397 units) and Windows headless Release (367 units)
pass. Android arm64 native Release now has 399 units compiled and linked, with
exactly one `-O2` per unit in the compile profile. No APK was
packaged, uploaded or installed, and no device result is claimed.
Verification details are in
[board QA](BOARD_QA.md).
The current widescreen backdrop fix centers the quad/viewport and uses a
64-unit texture repeat; its compiled real-GX draw probe passes. Final hidden
4:3 Back and Start/return/fresh-reload routes and widescreen Back QA pass with
six frames per scenario. The final 4:3 Start frame `f000005` and widescreen
Back frame `f000002` were visually inspected with correct UI, a fully tiled
background and no black strip. Local-only receipts are
`build/inst-qa/runs/20261009-031634-077824` and
`build/inst-qa/runs/20261009-031646-580806`.
Final combined verification passes 115/115, including two widescreen camera
tests, with no failures, errors or skips.

The previous update on 2026-09-28 advanced from
`c51eb379134c4f56603cf2068bd893faf6df656b`. Recovered m633, m659 and m668 are
compiled and bound to their real lifecycles (35 additional C units). The m659
exclusion in the previous update was incorrect: its lifecycle is in `camera.c`,
not a file named `prolog.c`. The window callback now follows upstream's signed
16-bit character parameter. Native adaptations fix m668's pointer-sized player
allocation, compiler-specific inline helpers and missing SDK declarations.

Minigame Mode's recovered entrance (`mdminidll`, 98 C units) is now linked to
its real lifecycle. `tools/native_mdmini.py` converts the verified, locally
supplied GP6E01 REL data into typed native initializers in the build cache:
scalars are endian-decoded, pointers use the host width, and mutable state is
reset on each entrance load. No disc data is checked into the repository.
Native adaptations correct camera clear sizes, a material callback's hardcoded
PPC pointer offset, and hardware FIFO writes in the transition effect.

At that revision, this enabled the greeting, player/character setup and mode
selection, **not complete playable Minigame Mode**. Its six downstream `mgm*` modules were then
unavailable; selecting Free Play safely returned to mode select. The board
minigame-call stub is unchanged. Windows and Android native Release builds and
259 engine/setup tests pass. Muted graphical entrance tests cover Back,
quit/re-entry and the Free Play handoff. The earlier headless m633/m659 fixtures
finish; m668 completion remains unverified. Exact scope and reproduction
commands are recorded in [board QA](BOARD_QA.md).

The previous update on 2026-09-23 advanced to `c51eb37` from
`85404041ba1f3ded03a5cdf65bed7fe081f6a5c7`. Newly recovered complete minigame
modules are listed explicitly in `tools/native_minigames.py`; m659 was wrongly
excluded by a filename-based lifecycle check, corrected above. Fifteen modules
(65 C units) bound their recovered
lifecycles. Native adaptations preserve pointer-sized actor callback payloads,
use canonical host layouts for m638's partial structure views, and provide
storage for m650's four-row matrix initializer. The native scheduler now
re-enters returning process functions as the original LR bootstrap does.
Windows and Android native Release builds and 249 regression/setup tests pass;
all fifteen modules plus the existing m616 and m670 complete isolated CPU
matches. Board minigame selection/instructions remain stubbed. Existing port
fixes and player files are preserved. Verification and the retained upstream
m640 undefined-return limitation are recorded in [board QA](BOARD_QA.md).

The previous update on 2026-09-13 advanced to `8540404` from
`994fb7c47b0a627201d2d44f8a7a936d7a132a26`. The complete recovered `m670dll`
module is integrated with its real lifecycle. The partial `m651dll` recovery
at that revision did not yet provide its main scene source, so it remained
stubbed until the update above. Board minigame selection/instructions are still unavailable. Existing
m616 integration, native loader, rendering and Windows startup fixes remain.
See [board QA](BOARD_QA.md) for the verification scope.

The previous update on 2026-09-13 advanced to `994fb7c` from
`a1aa433c8d3b1c589e896372a40f5fd559117eb4`. The recovered `m616dll` gameplay,
setup, utility and CPU units are linked with their real lifecycle. Its PPC
runtime wrapper is supplied by the host compiler, as for the other overlays.
The unavailable board minigame selection/instruction flow remains explicitly
stubbed; this update does not enable every minigame. The pin also includes
MusyX, speech SDK and MSL runtime recoveries; the port retains its native
audio, memory and runtime backends instead of compiling hardware-specific
implementations. Existing rendering, Android and widescreen adaptations and
the local Windows startup memory-reservation fix are preserved.
Verification for this update is recorded in [board QA](BOARD_QA.md).

The previous update on 2026-09-12 advanced to `a1aa433` from
`3b0a20d3239d31f20921287d7ffa17f4219026f7`. This consumes the recovered Board
Masu and Board Single owners and the shared game-work API changes. The Masu
byte-order and AO decal adaptations and the game-work minigame stub were
relocated without changing their behavior. Board Single is compiled directly
from upstream; the explicit minigame-call stub remains. The pin also includes
the QR/DCT speech recovery, but the native build does not compile the speech
SDK. Existing renderer, Android and widescreen changes are preserved.
Windows and Android native Release builds, 176 engine/native tests and 55
setup/release tests pass. A bounded widescreen/AO/FXAA board-rendering check
reached Towering Treetop; this does not certify a full match or Solo Mode.
See [board QA](BOARD_QA.md) for coverage. No APK upload or push was performed.

The previous update on 2026-09-11 advanced to `3b0a20d` from
`a15c000ba1aac3c05f2db5f8b9a7665552050716`. The changed gameplay owner is Board
Star, together with its W01 caller declaration. The recovered implementation
uses `FLAG_BOARD_STAR_RESET` for Star/Ztar awards and provides its constants
directly. The now-unused nine-definition `src/os/board_constants.c` bridge and
its build entry were removed; a pre-update source/runtime backup is retained.

The native patch preserves the particle alpha hardware-conversion guard,
declares `abs` and the guide model's actual `int` return, and retains the port's
explicit vector input for positioned Star/Ztar awards. Upstream's matching
parameter-address casts depend on the PPC stack: reading a 64-bit host pointer
object as a three-float vector is invalid. This native-only adaptation neither
changes the shared decomp nor claims to reproduce that retail stack quirk.
Existing AO, Android link optimizations, widescreen and minigame stubs remain.
Windows/Android Release builds, regression checks and four bounded live Star
event tests pass; see [board QA](BOARD_QA.md) for exact coverage and limits.
No APK upload or push was performed.

The previous update on 2026-09-11 advanced to `a15c000` from
`9651a1e5252c4631d30f9ad4632fb665429e13c4`. It consumes the recovered Board
Opening, Coin, Dice and Effect owners, including the particle rotation,
colour-register and emission corrections. Existing native fade layout,
widescreen coverage, particle-array registration, coin-table bounds and
dice-object lifetime adaptations remain. Opening's curve callback has an
explicit float-argument prototype and retains full-width function pointers on
the native targets. Obsolete Dice and Opening constant definitions were
removed from the port; their recovered owners now supply the values directly.
The native Opening declarations match the actual `int` returns from the guide
and space-list owners. Missing standard-library/branch declarations are supplied
without changing gameplay. Both native Release builds, 144 regression tests
and 55 setup tests pass; bounded opening and DK-dice play checks are recorded
in [board QA](BOARD_QA.md). No push or APK upload was performed.

The previous update on 2026-09-10 advanced to `9651a1e` from
`bf77696ce47c4914e39fe726972c837065cdedde`. It consumes the recovered board math
directly, including the corrected normalized-to-screen Y conversion. Upstream
provides portable C alongside its hardware-specific kernels; Windows and
Android compile the portable branch. The axis arguments now consistently use
`u8`, including the port's W01 radian-rotation declaration. The board-effect
compatibility hunks were relocated without changing their behavior, preserving
the native fade layout, full-width fade, and particle array registration.
The prior CapSpecial integration and explicit minigame stubs remain intact.
Windows/Android native Release builds and 158 regression/setup tests pass;
see [board QA](BOARD_QA.md) for traversal evidence and test limits.

The previous update on 2026-09-10 advanced to `bf77696` from
`5f71cac24c910a0b061a0ed53bfa8ef53784dfca`. It includes the recovered CapSpecial
owner and associated character/light API changes. The port retains Boo's live
widescreen capture/projection, non-fading material reset and the duel-minigame
stub. The upstream source now owns the DK one-shot reaction, native-sized DK
work copy, field-relative Bowser event data and its constants; their obsolete
port replacements were removed. The embedded duel guide pointer remains local
to its coroutine in the port so the 96-byte event payload stays compatible with
the other native CAPWORK views. Character-manager and HSF-manager compatibility
hunks were rebased without changing their contents.

Native API checks also align the guide-model and coin-display return types,
Boo theft callback, and Bowser dice hook with their implementations. Bowser's
squish-count caller uses `mbev_CapPlayerSquishVoiceSet(..., FALSE)`, the exact
implementation called by the old void forwarder: native C cannot recover its
discarded return register as an integer count. These changes preserve the real
effects and do not synthesize an event result. A reproduced DK dice wait also
exposed stale dice-number references after object deletion; the compatibility
patch now detaches those references before the native object slot is reused.
See [board QA](BOARD_QA.md) for the integration tests and their limits.

The previous update on 2026-09-07 advanced to `5f71cac` from
`777b6b44e7758ef817e01f5d79532c25878daf81`. This update recovers the party-results
scene's main and utility units in `src/REL/mdpresultdll/` and its shared header.
Both units are compiled for every native target. The DLL bridge now binds their
recovered lifecycle for `dll/mdpresultdll.rel`; board completion no longer uses
the unavailable-results return. Seven measured REL-local symbol collisions are
namespaced consistently across both units. The two port patch additions only
close native GX graph/trail submissions explicitly (GXEnd is empty on hardware).
The original ceremony, ranking, statistics, save and return logic is retained.
The existing sprite widescreen patch also now preserves and recenters explicit
clipping rectangles instead of treating every sprite as full-screen. The new
results statistics use those rectangles to hide off-panel columns.

The previous SNPC integration rebased the port patch queue on 2026-09-06 from
`8f9c3c010da32352908b637e7d6c46e8d99989e7` to `777b6b4`. SNPC was the only gameplay
source changed in that update. Its recovered fade hook already uses
`GX_TG_POS`, so the port's old Last Five Turns texture-input patch was
removed; the regression test now verifies upstream directly. SNPC's obsolete
native constant definitions have also been removed. Its sole remaining port
patch corrects the `mbCameraMoveMasu` caller prototype to the `s16 maxTime`
declared by `camera.h` and implemented by `camera.c` (no gameplay change).
See [SNPC integration](SNPC_INTEGRATION.md) for widescreen coverage. The setup tool
reads this value at run time, so keeping the pin and patch queue synchronized
is a build-integrity requirement rather than informational bookkeeping.

The old board `.sdata2` bridge supplied bit-exact values while their recovered
owners still imported original binary storage. Star was its last consumer;
all of those definitions now come from recovered C, so the bridge is gone.
