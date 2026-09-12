# Code map

The active build entry point is [tools/build.py](../tools/build.py). Its source
lists define the recovered game, board and overlay files compiled into the port.

| Working on | Start here |
| --- | --- |
| Settings, config, launcher | `src/gx/ui/settings.cpp`, `launcher_core.cpp` |
| FPS, display, input | `src/gx/aurora_bridge.c`, `frame_interp.c` |
| Audio / voice limits | `src/audio/msm_bridge.c` |
| Skipping unavailable minigames | `src/os/minigame_stub.c` |
| Scheduling / heaps | `src/os/process_native.c`, `arena.c`, `heap_scale.c` |
| Saves | `src/os/savestate.c`, `card_native.c`, `save_endian.c` |
| Models, cameras, shadows | `src/hsf/` |
| Disc import / file access | `src/content/`, `src/dvd/` |
| Windows / Android OS calls | `src/host/` |
| Android application | `packaging/android/` |
| Browser packager | `packaging/web/` |
| Dependency adaptations | `compat/`, `include/` |

Edit native implementations in `src/` and port contracts in `include/`.
Recovered game code comes from the pinned, read-only decomp checkout.
`compat/decomp/` is applied to `build/patched-src/`; do not edit that generated
copy or the dependency checkout to implement a Port change.

`minigame_stub.c` replaces the decomp's `board/mgcall.c`. It returns true from
the end-turn call, selecting the board's continue-in-place branch instead of
waiting for an unavailable overlay. It advertises zero selectable minigames.

The numbered Aurora adaptations remain ordered, content-attested build inputs.
See [Compatibility](../compat/README.md), [Building](BUILDING.md) and
[Testing](TESTING.md). Generated files, local assets and diagnostic artifacts
belong under ignored `build/`; history belongs in Git.
