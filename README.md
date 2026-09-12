# Mario Party 6 Native Port

A native Windows and Android port using recovered Mario Party 6 code,
Aurora, SDL3 and Dawn/WebGPU. No emulator is involved.

Menus, audio, memory-card saves and Towering Treetop board gameplay run natively.
Minigames are not available yet: the board skips their transition and continues
to the next turn. Other incomplete overlays are not playable.

## Build and play

You need your own Mario Party 6 (USA) disc image or extracted disc folder.
This repository and its engine packages contain no game assets.

For a configured Windows checkout, use the optimized gameplay build:

```powershell
python tools/build_windows_release.py
python tools/build.py --configuration release
build\release\mp6native.exe
```

VSync off + Unlocked FPS uncaps rendering while keeping game logic at 60 Hz.
**F10:** settings. **F9:** console. **F5/F8:** quick save/load.
Save states require the same linked build; memory-card GCIs are durable saves.

Android includes a full GameCube touch controller. Open the gear > Touch Controls
to resize, fade, hide or move controls. See [Touch controls](docs/SETTINGS.md#android-touch-controller).

First-time setup: `setup.bat --disc "path\to\Mario Party 6 (USA).iso"`.
See [Building](docs/BUILDING.md) for prerequisites, Port-local asset overrides,
Debug/headless builds and Android. Setup can provision dependencies; ordinary
builds consume the pinned decomp checkout read-only. Read
[Setup](docs/SETUP_TOOL.md) before running it in a shared workspace.

## Repository map

| Directory | Purpose |
| --- | --- |
| `src/` | Native runtime, graphics/UI, audio, host OS and board support |
| `include/` | Port APIs and game compatibility headers |
| `compat/` | Active dependency adaptations, not historical patch proposals |
| `packaging/` | Android app and browser-based local disc packager |
| `res/` | Launcher stylesheets and licensed fonts |
| `tools/`, `setup/` | Build, validation, release and setup commands |
| `tests/` | Runtime and repository-contract regressions |
| `docs/` | Current instructions and subsystem references |
| `build/` | Ignored binaries, generated files, local assets and diagnostics |

Start with [Code map](docs/CODE_MAP.md), [Architecture](docs/ARCHITECTURE.md),
[Testing](docs/TESTING.md), or [Settings](docs/SETTINGS.md).
Git history holds retired scaffolding; it does not belong beside active source.

The [browser packager](packaging/web/) combines engine binaries with files read
locally from your disc. Android can import content on-device. Do not redistribute
extracted game files or the resulting content folder. See [Releasing](docs/RELEASING.md).

## Legal

This repository contains no game assets. Assets (models, textures, audio,
save data, and so on) are extracted from the user's own legally owned disc
at build time (or picked on-device for Android) and are never checked into
this repository. Decompiled game source is consumed, read-only, from the
sibling matching (decompilation) repository -- see
[docs/DECOMP_DEPENDENCY.md](docs/DECOMP_DEPENDENCY.md) -- and is not
vendored, copied, or redistributed here.

The pre-boot launcher UI (`src/gx/ui/`, `res/rml/`) is adapted from the
unlicensed `mariopartyrd/partyboard` project; see
[docs/PARTYBOARD_PROVENANCE.md](docs/PARTYBOARD_PROVENANCE.md) for the full
provenance ledger, including exactly which files were copied, from which
commit, and their licensing status. On 2026-09-12, the maintainer confirmed
permission to redistribute both the adapted launcher UI and N64 Party font
with this port. This confirmation does not constitute a public upstream
license; the original credits and provenance notice are retained.

## Credits

This port stands on the work of a lot of other people:

- The GC/Wii decompilation community, whose tools and conventions this
  project's [decompilation dependency](docs/DECOMP_DEPENDENCY.md) builds on.
- The [Aurora](https://github.com/encounter/aurora) developers (encounter),
  for the GameCube GX/VI/PAD/CARD-over-modern-GPU backend this port renders
  and reads input through.
- The Dusk developers.
- Mario Party R&D and the contributors to
  [mariopartyrd/partyboard](https://github.com/mariopartyrd/partyboard), the
  Mario Party 4 PC port this port's launcher/settings UI was adapted from --
  see [docs/PARTYBOARD_PROVENANCE.md](docs/PARTYBOARD_PROVENANCE.md) for the
  full ledger, including the original per-file credit lines.
- [ImWhoreHay](https://x.com/ImWhoreHay), for the "N64 Party" display font
  used in the launcher.
- justcamtro, for the launcher's UI visual design.
- The open fonts bundled under `res/fonts/`: **Inter** and **Alegreya SC**
  (SIL Open Font License), and **Material Symbols** (Apache-2.0). Inter is
  the UI body font, replacing a commercial face the upstream UI used. See
  [res/fonts/LICENSES.txt](res/fonts/LICENSES.txt).
- This port's direct dependencies: [RmlUi](https://github.com/mikke89/RmlUi)
  (mikke89), [nod](https://github.com/encounter/nod) (encounter), SDL3, and
  Dawn/WebGPU.
