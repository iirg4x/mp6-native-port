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
a1aa433c8d3b1c589e896372a40f5fd559117eb4
```

This must match the output of the following command, run from the Port
repository root with the dependency checked out at the pinned revision:

```sh
git -C build/deps/marioparty6 rev-parse HEAD
```

The port dependency was updated to this `main` revision on 2026-09-12 from
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
