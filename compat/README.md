# Dependency adaptations

These are active build inputs, not historical patch proposals.

- `decomp/`: one unified diff per recovered source file, mirrored by source
  path. `tools/apply_patches.py` verifies removed lines against the pinned
  source and writes only to `build/patched-src/`. Zero-context diffs keep
  unchanged recovered game code out of this repository.
- `aurora/deps/`: the only Aurora-side patches left. They adapt third-party
  sources that Aurora's CMake fetches (abseil, RmlUi, Dear ImGui for the web
  build) and are applied to those fetched trees. Aurora itself is no longer a
  patch series: its port-modified source lives in `third_party/aurora/`
  (see `third_party/aurora/MP6_PROVENANCE.md`) and is edited directly.

Do not add patch fragments or temporary full-source overrides. Extend the
existing per-file decomp diff and verify the resulting source. Keep native
implementations in `src/`, with a named build substitution when appropriate
(for example `src/os/board_mgcall_abi.c`).

Changing backend code requires rebuilding its attested archives. Moving files
does not authorize weakening input hashes, toolchain checks or save-state
compatibility checks. Shared dependency checkouts remain untouched.
