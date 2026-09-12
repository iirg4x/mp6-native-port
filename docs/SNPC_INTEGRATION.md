# SNPC integration

The port consumes `iirg4x/marioparty6` main at
`777b6b44e7758ef817e01f5d79532c25878daf81` (checked 2026-09-07).
Only `src/board/snpc.c` changed in the gameplay source since the previous pin.
The private dependency clone is in `build/deps/marioparty6`; the shared
decomp development checkout was not modified.

## Ownership

- Compile the recovered SNPC source, including its new C-owned constants.
  Remove the now-duplicate/unused SNPC section from `src/os/board_constants.c`.
- Upstream now owns the `GX_TG_POS` fade correction used during Last Five Turns.
  The regression test executes it directly and rejects a deliberately broken
  `GX_TG_TEX0` mutation. The port no longer carries that correction.
- The remaining SNPC compatibility patch only fixes the local
  `mbCameraMoveMasu` declaration to match the authoritative camera header and
  implementation (`s16 maxTime`).

## Widescreen effects

SNPC uses the existing board rendering paths, so no additional hardcoded
aspect ratio or artificial mesh stretching is needed:

- The board camera applies live horizontal expansion to projection, viewport
  and scissor. Native camera framing and gameplay positions stay unchanged.
- Banana, fire and star effects are world-space models/particles drawn through
  that camera. The board particle adapter supplies complete native array spans.
- Star/fade/metal/electric material hooks operate on model positions/normals;
  they are not 640-pixel screen overlays and must not be horizontally scaled.
- SNPC's shared paper transitions, dissolve capture and board fades retain the
  existing full-width adaptations in `board/wipe.c` and `board/effect.c`.

## Verification

`tests/integration/build_board_qa.py` provides an explicitly test-only
`snpc-effects` fixture. It invokes recovered DK banana, Bowser fire, star gain
and star loss effects on W01's existing scene; it does not replace an
undecompiled board or install substitute gameplay hooks in releases.

Windows runs `snpc-wide-a` (1280x720) and `snpc-ultrawide` (1680x720) each
completed all four effects, with AO Strong and FXAA enabled. Captures show
unscaled characters and fire drawing into the expanded side regions. The
fixture uses its own camera framing, not a claim of full Clockwork Castle
gameplay coverage. Output is isolated under `build/board-qa-runs/`.

49 CPU/native regression tests and 9 Vulkan AO tests passed. The camera test
executes the real port adapter at 4:3, 16:9 and ultrawide widths, repeated/live
width changes, and checks unchanged authored camera fields. Android native
compilation is verified separately; no Android device is attached for
on-device gameplay verification.
