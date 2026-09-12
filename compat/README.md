# Dependency adaptations

These are active build inputs, not historical patch proposals.

- `decomp/`: one unified diff per recovered source file, mirrored by source
  path. `tools/apply_patches.py` verifies removed lines against the pinned
  source and writes only to `build/patched-src/`. Zero-context diffs keep
  unchanged recovered game code out of this repository.
- `aurora/base/`: ordered backend portability, cache-lifetime and rendering
  adaptations required by the verified dependency build. Numbered filenames
  are stable artifact-provenance identities; do not reorder them casually.
- `aurora/windows/`: Port-local backend extensions, applied to a source copy
  under `build/`. They support optimized uncapped presentation and live AA.

Do not add patch fragments or temporary full-source overrides. Extend the
existing per-file decomp diff and verify the resulting source. Keep native
implementations in `src/`, with a named build substitution when appropriate
(for example `src/os/minigame_stub.c`).

Changing backend code requires rebuilding its attested archives. Moving files
does not authorize weakening input hashes, toolchain checks or save-state
compatibility checks. Shared dependency checkouts remain untouched.
