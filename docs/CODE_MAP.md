# Code Map

Companion to [ARCHITECTURE.md](ARCHITECTURE.md): that doc explains *how* the
port works; this one is the shallow map of *where things are*. Read this
first.

## Where things live

- **`platform/`** — the port's own C/C++, reimplementing the Dolphin SDK
  layer the decompiled game calls into. Split by subsystem:
  - `gx/` — the Aurora bridge (GX/VI/PAD, frame pacing, the 60Hz tick
    throttle). Inside it, `gx/ui/` is a separate thing: the RmlUi
    pre-boot launcher/settings menu.
  - `os/` — the low-4GB arena, the HuPrc process scheduler, CARD wiring,
    the save-endian marshal.
  - `hsf/` — the native HSF model deserializer.
  - `dvd/` — FST parsing and on-disk file access.
  - `content/` — the first-run disc-content importer shared by the
    launcher UI, the setup tool, and Android.
  - `android/` — the on-device bootstrap (maps `libmp6game.so` low, the
    SAF content-URI bridge, touch input).
  - Also `host/` (the OS seam: win32/android backends) and `audio/`
    (the msm bridge, ADPCM decode, SDL output).
- **`shim/include/`** — headers that let the unmodified decompiled `.c`
  files compile against this port: byte-swap helpers (`be.h`), Dolphin-type
  compat typedefs, declarations for the generated stub layer.
- **`patches/decomp/src/`** — unified diffs applied to the decompiled game
  source at build time (`tools/apply_patches.py`, into a staging copy —
  the decomp checkout itself is never written), one `.patch` per source
  file, mirroring its path. They're **zero-context diffs** (no surrounding
  unchanged lines) on purpose, to keep how much decompiled source sits in
  this public repo to a minimum. Honest tradeoff: that makes a patch file
  genuinely hard to read on its own, without the real source file open
  beside it.
- **`tools/`** — the build driver (`build.py`) and its support scripts:
  patch application, shim generation, the Android gate, the save-format
  self-test, the headless log-diff gate.
- **`setup/`** — the from-your-own-disc installer (`setup.py` +
  `setup/lib/`): checks prerequisites, fetches the toolchain and the
  decompilation, extracts what it needs from your disc, builds `dist/`.
- **`res/`** — launcher UI assets: bundled fonts (`res/fonts/`) and RmlUi
  stylesheets (`res/rml/*.rcss`).
- **`platforms/android/`** — the Android app shell: Gradle project, SDL
  activity glue, manifest. Links against `libmp6game.so`, built from
  `platform/android/` plus the rest of `platform/`.
- **`web/`** — the browser packager (`web/js/`, served from GitHub Pages):
  reads your disc client-side and combines it with a prebuilt engine build
  into a runnable folder or zip. No native code.
- **`docs/`** — this doc plus the deep-dives: architecture, building,
  testing gates, debugging, the decomp dependency, the setup tool,
  releasing, UI provenance.

(`CMakeLists.txt` at the repo root is inactive scaffolding, not the real
build — see its own header comment.)

## Where to start reading

1. **`README.md`** — what this is, how to get it running.
2. **`docs/ARCHITECTURE.md`** — how the pieces fit together (module map,
   memory model, scheduler).
3. **`tools/build.py`'s source lists** (`game_sources()`, `REL_SOURCES`) —
   the fastest way to see what actually compiles into the exe.
4. **One small `platform/` file** — e.g. `platform/dvd/dvd_files.c` or
   `platform/os/log.c` — to see the reimplementation style before tackling
   something bigger like `aurora_bridge.c`.

## How a change flows

1. Edit port code in `platform/`, **or** add/extend a patch in
   `patches/decomp/src/` for a decompiled-source fix.
2. `python tools/build.py` (`--headless` / `--target aarch64-android` as
   needed).
3. Prove it against the gates in [TESTING.md](TESTING.md).

The game half (`game/`, `bootDll`, `selmenuDll`, `fileseldll`, `mdseldll`)
compiles from a **separately-cloned decompilation repo**, consumed
read-only — see [DECOMP_DEPENDENCY.md](DECOMP_DEPENDENCY.md). It is not
vendored in this repository.
