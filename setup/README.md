# mp6-native local setup tool

Turns "I own a Mario Party 6 disc" into a playable build **on your own
machine**. You run this tool, point it at your own legally-owned disc, and
it fetches the decompiled game's source code and a standard compiler
toolchain, then builds the game locally. See
[`docs/SETUP_TOOL.md`](../docs/SETUP_TOOL.md) for the full design writeup
and why this exists instead of a downloadable executable.

## Quick start (Windows)

```
setup.bat --disc "D:\path\to\Mario Party 6 (USA).iso"
```

or just double-click `setup.bat` / right-click `setup.ps1` -> Run with
PowerShell, and it will prompt you for the disc path. When it finishes:

```
dist\mp6native.exe
```

is a complete, runnable build (the exe, its runtime DLLs, and `res/`) --
copy the whole `dist\` folder anywhere you like.

## What it does, step by step

1. **Prerequisite check** -- git, Python 3, ninja (hard requirements) and
   cmake (only needed if Aurora has to be built from scratch). Reports
   exactly what's missing and how to install it; never silently continues
   past a missing hard requirement.
2. **Toolchain** -- makes sure the `zig` compiler is present under
   `port/toolchain/` (the exact version `tools/build.py` expects, parsed
   from that file so this can never drift out of sync with it). If
   missing, downloads it from the **official** `ziglang.org` release index
   (`https://ziglang.org/download/index.json`), verifies its sha256, and
   prints the exact URL it used. Also makes sure `nod` (the GC/Wii
   disc-image library) is present, via the port's own
   `tools/fetch_nod.py`.
3. **Decomp source** -- clones (or fast-forwards) the pinned decompilation
   repository at the exact commit named in
   [`docs/DECOMP_DEPENDENCY.md`](../docs/DECOMP_DEPENDENCY.md), parsed at
   run time (never hardcoded), to the sibling path `tools/build.py` itself
   expects. This is **source code only** -- a from-scratch reimplementation
   of the game's logic, the normal way decompilation projects work, fetched
   the same way any open-source git dependency would be.
4. **Your disc** -- validates it's really Mario Party 6 (USA / `GP6E01`)
   and extracts it (via `nod`, the same disc-image library
   `platform/content/content_import.cpp` uses for the Android on-device
   import) into the decomp checkout's `orig/GP6E01/` folder -- never into
   this repository, never anywhere else. By default this skips
   `files/movie/` (336MB of FMVs not needed to build or boot to the menu
   slice this port currently covers) -- pass `--include-movies` for a
   completely full extraction. Also runs the decomp's own (pre-existing)
   `dtk dol split` step just far enough to materialize the small data
   blobs (`build/GP6E01/include/*.inc`) several source files `#include`
   directly -- this needs only the small `dtk` CLI (which the decomp
   project's own tooling fetches from its GitHub release automatically,
   exactly as it always has), never the MWCC/binutils/sjiswrap toolchain
   the decomp's own byte-matching gate uses (this port never needs that
   gate at all).
5. **Aurora** (the GX/VI/PAD/CARD/RmlUi backend) -- detects an existing,
   ready-to-link build and reuses it (the common case, and the only case
   this project's own verification exercised: building Aurora is a heavy,
   one-time, machine-local step, utterly unlike the per-player steps
   around it). If it isn't built yet, prints the exact manual recipe
   (`docs/BUILDING.md`); pass `--build-aurora` to attempt doing that
   automatically instead (best-effort -- see `docs/SETUP_TOOL.md`).
6. **Build** -- runs `tools/build.py` for both the windowed
   (`mp6native.exe`, links Aurora for a real window) and headless
   (`mp6native_headless.exe`, CI/automation, no Aurora/SDL at all) modes,
   then assembles everything into `dist/` (whatever `build.py` staged next
   to the exe -- runtime DLLs, `res/` fonts + RmlUi stylesheets).
7. **Android** (`--android`, optional) -- see "Android" below.

## Flags

| Flag | Meaning |
|---|---|
| `--disc PATH` | Your disc image or an already-extracted folder. Prompted for if omitted. |
| `--include-movies` | Also extract `files/movie/` (336MB, not currently needed). |
| `--force-disc` | Re-extract even if `orig/GP6E01` already looks complete. |
| `--decomp-url URL` | Override the decomp git remote (default: the fork, or `$MP6_DECOMP_URL`). |
| `--decomp-ref SHA` | Override the pinned commit (default: read from `docs/DECOMP_DEPENDENCY.md`). |
| `--aurora-url URL` | Override the Aurora git remote (default: `encounter/aurora`, or `$MP6_AURORA_URL`). |
| `--build-aurora` | Attempt a from-scratch Aurora build if one isn't already present. |
| `--headless-only` / `--windowed-only` | Build only one of the two exe modes (default: both). |
| `--dist-dir PATH` | Where to assemble the runnable build (default: `dist/` next to this file's parent). |
| `-j`, `--jobs N` | Parallel compile jobs (default: `tools/build.py`'s own CPU-count default). |
| `--android` | Also run the Android lane (SDK/NDK detection, `.so` builds, APK). |
| `--skip-apk` | With `--android`: build the `.so` rows but skip the gradle APK assembly. |
| `-y`, `--yes` | Assume yes / don't block on prompts (for scripted, non-interactive runs). |

Every step is idempotent: re-running the tool skips whatever's already
done (already-cloned decomp at the right commit, already-extracted disc,
already-built Aurora, ...), so it's safe to just re-run it after fixing
whatever it complained about.

## Platform status

- **Windows: fully solid.** This is the only host `tools/build.py` currently
  targets -- its toolchain-path resolution (zig, Aurora's CMake wrapper
  scripts, the NDK's `windows-x86_64` prebuilt host tools) is Windows-host
  specific throughout, independent of which platform you're building *for*.
  This is a fact about the port's current build driver, not a limitation
  this setup tool papers over.
- **Android: optional, detect-and-reuse, same honesty policy as Aurora.**
  `--android` detects an existing SDK/NDK and Aurora android trees and
  reuses them; if they're missing, it prints the exact provisioning recipe
  and stops rather than guessing. It deliberately does **not** run
  `sdkmanager --licenses` or install the SDK for you -- accepting Google's
  SDK license is something only you can do. The disc itself is never
  touched by this tool for Android: the APK's own first-run onboarding
  (`platform/gx/ui/content_setup.cpp`) handles
  that on-device.
- **Linux/macOS (`setup.sh`): partial by design.** The prerequisite check,
  decomp clone, and disc extraction (`nod` ships Linux/macOS builds too)
  all run natively. The actual native build step currently requires a
  Windows host (see above) -- `setup.sh` says so plainly rather than
  failing confusingly deep into the pipeline.

## Legal

No Nintendo material -- no game code, no game assets -- is ever included in
this repository or in this tool. You must legally own a Mario Party 6
(USA) disc to use it. Game code is a from-scratch decompilation, fetched
as source; game assets come only from your own disc, extracted locally and
never redistributed. See [`docs/SETUP_TOOL.md`](../docs/SETUP_TOOL.md) for
the full rationale and [`../README.md`](../README.md)'s own "Legal" section.
