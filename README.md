# MP6 Native Port

A native PC (Windows) and Android port of the Mario Party 6 decompilation,
running boot-to-menu as real host-native code -- no emulator involved.
`game/` plus the `bootDll`, `selmenuDll`, `fileseldll`, and `mdseldll`
overlays are compiled directly from the decompiled source (see
[docs/DECOMP_DEPENDENCY.md](docs/DECOMP_DEPENDENCY.md)); the Dolphin SDK
layer they call into (GX/OS/VI/PAD/DVD/CARD/MSM) is reimplemented on top of
[Aurora](https://github.com/encounter/aurora), SDL3, and a small host-OS
seam. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full
breakdown.

## Get it

The easiest way to play doesn't require building anything yourself. This
repo's **web packager** (`web/`, served from GitHub Pages) runs entirely in
your browser: point it at your own Mario Party 6 (USA) disc image, or a
folder [Dolphin](https://dolphin-emu.org/) already extracted, and it
combines your disc's game content with this project's prebuilt engine build
into a folder (or, on browsers without the File System Access API, a single
zip) you can run right away. Nothing about your disc leaves your machine —
there's no upload, no server involved, just the File API reading your disc
locally in the tab. On Android, skip the packager entirely — install the APK
and import your disc's content on-device on first launch.

**Policy, stated plainly** (the same posture as other decompilation-based
native ports, e.g. Ship of Harkinian): this project's GitHub Releases carry
**content-free engine binaries only** — on Windows that's `mp6native.exe`
plus its handful of DLLs and a small resource folder (fonts, UI
stylesheets); on Android, a clean APK. Neither one contains a single byte of
Mario Party 6's code or assets. **Game data is never distributed here, by
this project, in any form, anywhere** — you need your own legally owned
Mario Party 6 (USA) disc, and every install this project helps you produce
(the web packager, the setup tool below, or the Android app's own import) is
assembled locally, on your own machine, from that disc. Please don't rehost
or redistribute the folder/zip/APK-content this produces — it contains
copyrighted assets pulled from your disc, not this project's own work; see
[docs/RELEASING.md](docs/RELEASING.md) for how release assets themselves are
built and what they do and don't contain.

## Quick start — build and play from your own disc

**You supply your own Mario Party 6 (USA) disc. No game data is distributed
here, ever, in any form.** Prebuilt engine binaries are available via
[Get it](#get-it) above, but they're content-free (no game code, no assets)
— turning them into something playable always assembles a runnable build on
your own machine from your own disc plus the decompiled source, so nothing
containing game code or assets is ever redistributed by this project. The
rest of this section builds that same result from source instead of via the
prebuilt engine.

The easiest path is the setup tool. It checks prerequisites, fetches the
toolchain and the decompilation, extracts the assets it needs from your disc
locally, and builds a playable `dist/`:

```
setup.bat --disc "path\to\Mario Party 6 (USA).iso"
```

(or double-click `setup.bat` / run `python setup/setup.py`; add `--android` to
also build the APK). Accepts `.iso` / `.rvz` / `.gcm` or an already-extracted
folder. See [`setup/README.md`](setup/README.md) and
[`docs/SETUP_TOOL.md`](docs/SETUP_TOOL.md); to build by hand instead, see
[`docs/BUILDING.md`](docs/BUILDING.md).

## Status

Boots to a fully navigable title screen, file-select, and mode-select, with
real audio (streamed music and sound effects) and working save/load against
Dolphin-compatible memory card images. Windows ships a pre-boot settings
launcher; Android runs the same game logic touch-driven on device. Gameplay
past the menu (the board and minigames) is out of scope for now -- see
"Non-goals" below.

## Quick start

**Windows**, from this directory:

```
python tools/build.py                    # windowed build -> build/mp6native.exe
build\mp6native.exe                       # run it (opens the launcher menu)
```

**Android** (needs the NDK and a device or emulator):

```
python tools/build.py --target aarch64-android --windowed
cd platforms/android && gradlew.bat assembleDebug
```

Install the resulting APK and launch it; first run walks through picking the
game's disc files on-device. See [docs/BUILDING.md](docs/BUILDING.md) for
prerequisites, the headless/CI build rows, and troubleshooting.

## Launcher and settings

Interactive Windows launches open a pre-boot menu (window mode/size/aspect,
volume, tick rate, content-root override) before handing off to the game;
settings persist to a portable `mp6_config.json` next to the executable.
Any invocation that looks like an automated run (a tick-budget argument, an
input script, or `MP6_AUTO_START_TICKS`) skips the launcher entirely and
boots straight to the game -- see docs/TESTING.md's automation contract.

## Save compatibility

Saves are ordinary Dolphin memory-card images under `saves/USA/Card A/`,
byte-compatible with real hardware and with Dolphin itself -- a card written
by this port loads in Dolphin and vice versa. See docs/ARCHITECTURE.md's
"Save system" section for how the endian marshal keeps that true.

## Non-goals (for now)

Gameplay past the menu is explicitly out of scope. Board gameplay depends on
`board/` and the minigame RELs, and native porting of that code is gated on
those parts of the decompilation reaching the same "100% recovered" standard
already met by game/bootDll/selmenuDll/fileseldll/mdseldll. Until that
dependency is satisfied, this repository's scope stops at the menu.

## Legal

This repository contains no game assets. Assets (models, textures, audio,
save data, and so on) are extracted from the user's own legally owned disc
at build time (or picked on-device for Android) and are never checked into
this repository. Decompiled game source is consumed, read-only, from the
sibling matching (decompilation) repository -- see
[docs/DECOMP_DEPENDENCY.md](docs/DECOMP_DEPENDENCY.md) -- and is not
vendored, copied, or redistributed here.

The pre-boot launcher UI (`platform/gx/ui/`, `res/rml/`) is adapted from the
unlicensed `mariopartyrd/partyboard` project; see
[docs/PARTYBOARD_PROVENANCE.md](docs/PARTYBOARD_PROVENANCE.md) for the full
provenance ledger, including exactly which files were copied, from which
commit, and their licensing status. **That project has no license**, which
is the one open licensing question hanging over this repository -- read the
provenance doc before republishing or redistributing.

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
