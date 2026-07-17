# The local setup tool (`setup/`)

## 1. Why this exists

This port cannot distribute a prebuilt `mp6native.exe` or APK. Either one
would contain the *decompilation* -- the recovered game logic -- compiled
into a binary, which is not this project's code to redistribute, and the
built binary would also embed real Mario Party 6 assets.

So instead of shipping a binary, this repository ships a **setup tool**:
you run it, point it at your own legally-owned disc, and it fetches the
decomp *source* and a standard compiler toolchain and builds the playable
port right here, on your machine. Concretely, and this is the entire point,
get it right:

- **This repository + this tool contain zero game code and zero game
  assets**, before or after you run the tool. `git grep`/inspect it
  yourself -- there's nothing to find.
- **Game code** comes from `external_refs/repos/marioparty6`, the pinned
  decompilation repository, fetched as *source* over a normal `git clone`
  -- exactly the way any open-source dependency is consumed, and exactly
  the norm in the GC/Wii decompilation scene (see `docs/DECOMP_DEPENDENCY.md`).
  A decompilation is a from-scratch reimplementation recovered by reverse
  engineering, not a copy of Nintendo's code.
- **Game assets** (models, textures, audio, text, save format) come only
  from the disc you point the tool at. They are extracted once, locally,
  into a git-ignored folder, and never leave your machine through this
  tool.
- **The playable binary is compiled on your machine**, by a standard,
  freely redistributable open-source toolchain (`zig`, fetched from its
  official distributor, `ziglang.org`) -- the same category of dependency
  as `ninja` or `cmake`, not something this project authored.
- Every fetch this tool performs prints the exact URL it used, so none of
  this is a black box.

## 2. What it does (mechanically)

`setup/setup.py` is a plain-stdlib Python 3 driver (`setup/lib/*.py`);
`setup.bat` / `setup.ps1` / `setup.sh` are thin per-platform launchers that
just locate a Python 3 interpreter and run it. Six steps, each idempotent
(a re-run skips whatever's already correct):

1. **Prerequisites** -- git, Python 3, ninja (hard requirements: the tool
   refuses to proceed without them, with install hints per platform);
   cmake (soft -- only needed for the rare from-scratch Aurora build).
2. **Toolchain** -- `zig` (parsed version requirement straight out of
   `tools/build.py`'s own `ZIG` constant, so this can never fetch a
   version other than the one the build driver will actually use),
   fetched from the **official** `ziglang.org/download/index.json` release
   index with a sha256 check against the same index, and `nod` (the
   GC/Wii disc-image library) via the port's own, pre-existing
   `tools/fetch_nod.py`.
3. **Decomp source** -- clones/resyncs `external_refs/repos/marioparty6`
   to the exact commit named in `docs/DECOMP_DEPENDENCY.md`, parsed at run
   time (see section 4 for a real bug this caught).
4. **Your disc** -- validates the game ID (`GP6E01` -- the same check,
   same two error messages, as `platform/content/content_import.cpp`'s
   `validate_game_id()`) and extracts it via a small ctypes binding
   against `nod`'s C FFI (`setup/lib/nod_ffi.py`) directly into the decomp
   checkout's `orig/GP6E01/{sys,files}` -- full disc minus `files/movie/`
   by default (336MB of FMVs the current boot-to-menu slice never reads;
   `--include-movies` opts back in). Then runs the decomp's own,
   pre-existing `dtk dol split` (via its `configure.py` + a single `ninja`
   target) just far enough to materialize `build/GP6E01/include/*.inc` --
   small data blobs several source files `#include` directly, which
   `tools/build.py` assumes already exist. This needs only the small `dtk`
   CLI (fetched by the decomp's own tooling, unchanged); never the
   MWCC/binutils/sjiswrap toolchain the decomp's own byte-matching gate
   uses, which this port has never needed.
5. **Aurora** -- detect-and-reuse an existing build (the normal case);
   otherwise print the exact manual recipe or, with `--build-aurora`,
   attempt it (best-effort -- see section 5).
6. **Build + dist/** -- runs `tools/build.py` for both build modes,
   harvests whatever it staged next to the exe into a flat `dist/` folder.
7. **Android** (`--android`, optional) -- see section 6.

## 3. Design choices worth explaining

**Why extraction targets `<decomp>/orig/GP6E01`, not a separate copy.**
`tools/build.py` bakes `<decomp>/orig/GP6E01/{files,sys/fst.bin}` in as the
running exe's DVD-read root (`MP6_DVD_FILES_ROOT`/`MP6_DVD_FST_PATH`) --
there is exactly one place the extracted disc needs to live, and it's
inside the (already git-ignored) decomp checkout, never inside this
repository.

**Why the decomp's own `dtk dol split` has to run at all.** This was not
obvious going in: `tools/build.py` searches
`<decomp>/build/GP6E01/include` for a handful of `#include`d data blobs
(font bitmaps, the boot-warning screens' packed data, ...). That directory
is *inside* the decomp's own gitignored `build/`, so a fresh decomp clone
never has it. It turns out to be produced, as an unmodeled side effect, by
one specific ninja edge -- `dtk dol split orig/GP6E01/sys/main.dol
build/GP6E01` (`tools/project.py`'s `"split"` rule) -- which needs the
real disc's `main.dol` + `.rel` files (hence: disc extraction must finish
*before* this runs) but needs nothing else the decomp's real byte-matching
gate needs (no compiler, no linker). Targeting that one ninja output
(`build/GP6E01/config.json`) instead of the default `ninja` target (which
would chase the full matching-gate build, and everything MWCC/binutils/
sjiswrap that implies) keeps this step to a few seconds.

**Why Aurora is detect-and-reuse, not build-every-time.** Building Aurora
is a ~20-60 minute, network-heavy CMake project (Dawn/RmlUi/abseil
FetchContent fetches) that changes only when Aurora itself is updated --
categorically different from the per-player decomp/disc/build steps
around it. `setup/lib/step_aurora.py`'s readiness check literally imports
`tools/build.py` and calls its own `_resolve_aurora_link_items()`, so it
can never drift from what the link step actually needs (no re-derived
~90-item archive list to go stale).

**Why the disc image is never copied into any repository.** The nod-based
extractor (`setup/lib/nod_ffi.py`) reads bytes out of the source image/
folder and writes exactly the wanted subset straight to
`<decomp>/orig/GP6E01/...`; nothing in the pipeline ever stages a copy of
the source image itself anywhere, and `orig/` is git-ignored by the decomp
repo's own `.gitignore` regardless.

**Long-path defense.** `git clone`/`checkout` are invoked with `-c
core.longpaths=true` -- found necessary the hard way (section 4) once a
deeply-nested destination path pushed a real clone past Win32's classic
`MAX_PATH`.

## 4. A real bug this project's own docs had, found while building this

While implementing step 3 (decomp source), reading `docs/DECOMP_DEPENDENCY.md`
literally would have pinned a stale commit. The doc said
`b05ede1d53f5763539a4a33ab0505b4d7749b96d`, but the branch's own tip commit
(`0b9dbb4`, present in the canonical `mp6-native` this setup-tool clone was
made from) is titled *"decomp-overrides: drop stale mdparty.c/stage.c pins
-- decomp settled at 4a67610"* -- i.e. the port had already moved to
requiring decomp commit `4a6761094935be3588ca2b1eda0a71a0988f8efb` and
dropped the local override shim that used to compensate for the
in-progress state at the older pin, but the dependency doc was never
bumped to say so. Building against the stale pin would have fed the port a
`mdparty.c`/`stage.c` with neither the old shim nor the new upstream
content it now assumes -- a real, reproducible break, not a hypothetical
one.

Fixed as part of this change (`docs/DECOMP_DEPENDENCY.md` now reads
`4a6761094935be3588ca2b1eda0a71a0988f8efb`, with a note explaining why),
since `setup/lib/step_decomp.py` reads that file at run time -- keeping it
accurate is load-bearing for this tool, not just informational.

## 5. Aurora-from-scratch: what's real, what's unexercised

`--build-aurora` clones Aurora at its pinned commit (refusing to touch an
*existing* Aurora checkout -- it stays read-only once present, same as
every other lane on this project touches it), generates the zig CMake
compiler-wrapper scripts, configures + builds both trees (plain and
RmlUi-enabled), and applies `platform/gx/aurora-patches/*.patch` -- against
the Aurora tree directly where the patch's own `--- a/...` target resolves
there, or against the matching CMake `FetchContent` dependency source
(searched for under each `_deps/*-src` after configure) otherwise. The
exact CMake invocations were ground-truthed against this workspace's own
`external_refs/repos/aurora/{build,build-rmlui}/CMakeCache.txt` (i.e. what
actually configured the two trees this project already has), not guessed
from prose.

**What this project's own verification did NOT exercise**: a real,
from-scratch `--build-aurora` run. Aurora already being fully built and
shared read-only across every lane on this development machine, and a real
run being 20-60+ minutes of network-heavy CMake, running it live here would
have both risked the shared resource (explicitly out of scope -- "aurora
... READ-ONLY") and told us nothing the detect-and-reuse path (which every
step of this pipeline *did* exercise, repeatedly) hadn't already. The
patch-target-resolution logic and the CMake argument lists are real code
following a real, ground-truthed recipe, not a stub -- but treat this one
path as less battle-tested than everything else in this document.

## 6. Android status

`--android` follows the same honesty policy as Aurora: detect the SDK/NDK
and Aurora's two Android CMake trees (`build-android`,
`build-android-rmlui`) and reuse them if present; print the exact
provisioning recipe and stop if not. It deliberately never runs
`sdkmanager --licenses` or silently installs the SDK -- accepting Google's
SDK license is something only the user can do.

This workspace happened to have a fully-provisioned Android toolchain
already (NDK 27.3.13750724, both Aurora android trees, the android `nod`
staticlib), so this project's verification exercised the **entire** Android
lane for real, end to end, not a stub -- including finding and fixing a
real bug along the way (below):

- `step_android.ensure_android_toolchain()` found the existing SDK/NDK.
- `step_android.build_headless()` ran `tools/build.py --target
  aarch64-android` for real (82 translation units, 0 failures) and
  produced `build/android/libmp6game.so` + `mp6launcher`.
- `step_android.build_windowed()` ran `tools/build.py --target
  aarch64-android --windowed` for real (109 translation units, 0
  failures), linked against the RmlUi-enabled android Aurora tree, and
  staged stripped `libmp6game.so` (131.7MB -> 30.2MB) + `libmain.so` into
  `platforms/android/app/src/main/jniLibs/arm64-v8a/`, with the
  post-strip `dynsym` probe confirming `mp6_android_main`/`GameMain` still
  resolve.
- `step_android.build_apk()` ran gradle's `assembleDebug` for real and
  produced an installable `app-arm64-v8a-debug.apk` (31.7MB).
- The full `run_android_lane()` orchestrator (the actual `--android`
  code path) was then run once, clean, end to end, confirming all of the
  above work together, not just in isolation.

**A real bug this caught**: `build_apk()`'s first version hardcoded the
output path `app/build/outputs/apk/debug/app-debug.apk`. Gradle's actual
output (this project's AGP config splits per-ABI) was
`app-arm64-v8a-debug.apk` -- a different name -- so the hardcoded check
failed immediately after a genuinely successful gradle build ("BUILD
SUCCESSFUL... 36 actionable tasks: 36 executed", then this tool's own
post-check raised anyway). Fixed by searching the debug output directory
for whatever `.apk` gradle actually produced instead of guessing its name.

On-device asset import is not this tool's job at all: the APK's own
first-run onboarding (`platform/gx/ui/content_setup.cpp`,
`docs/A4_ANDROID_UI.md`) reads the user's disc directly on the device. The
one piece genuinely not exercised here is the on-device smoke itself (no
attached device in this environment) -- `gate_android.py`'s own tier-2/3
language already treats that as an expected, non-blocking skip.

## 7. End-to-end verification transcript

See the parent conversation/report for the full quoted console output.
Summary of what was actually run, all on this development machine:

**Stage A -- genuine fresh-user simulation** (isolated directory tree,
*not* the shared canonical checkouts): a fresh clone of this setup-enabled
port, with `port/toolchain` and `external_refs/repos/aurora` reused
read-only via Windows directory junctions (exactly the "reuse toolchain /
reuse prebuilt Aurora" the task allows), and a **genuinely fresh** decomp
clone -- `external_refs/repos/marioparty6` did not exist in that tree
before this run. Pointed `--disc` at the real, read-only
`Mario Party 6 (USA).iso`. Result: prereqs OK -> toolchain reused -> decomp
cloned fresh from `github.com/iirg4x/marioparty6` at the (corrected) pin
-> disc validated (`GP6E01`) and extracted (819 files, 416.5MB, movies
skipped) -> decomp's `dtk dol split` ran (fetched `dtk` v0.9.2 fresh from
its GitHub release, split 137 modules into 1380 objects in ~3.3s,
`build/GP6E01/include` populated with 23 files) -> Aurora detected ready
(via the junction) -> both build modes compiled (107 / 82 translation
units, zero failures) and linked -> `dist/` assembled (exe pair + PDBs + 7
DLLs + `res/`) -> exit 0.

**Boot proof**: `dist/mp6native_headless.exe 600` exited 0, and
`tools/ua1_logdiff.py` against the project's own committed baseline
(`docs/ua1/win_headless_600.log`) reported **PASS: 233 normalized
game-flow lines are IDENTICAL** -- not just "didn't crash," but
byte-for-byte the same game-flow log as the known-good reference, produced
by a binary built from a disc extraction and a decomp clone that did not
exist five minutes earlier.

**Stage B -- windowed smoke, GPU-lock-coordinated**: built the windowed
`mp6native.exe` directly (via `tools/build.py`, read-only against the
shared canonical decomp+Aurora -- deliberately *not* re-running the
disc-split step against that shared checkout, to keep `dtk`/`ninja` from
ever touching tracked files in a repository other lanes are actively
using) in the real `port/mp6-native-setup` checkout, then ran it through
`tools/vtest.sh`'s mandatory acquire/kill/release protocol. This had to
*actually wait* -- `[vtest] GPU lock held by [b1 bonus-sprite repro
(extended) pid=4410 epoch=...] -- waiting...` -- for another concurrent
lane's windowed test on this shared machine to finish, then acquired,
launched, and confirmed the real thing: D3D12 initialized against the
machine's actual RTX 4090, real streamed audio, a real disc read (`[DVD]
fst.bin loaded: 961 entries`), and the game flow reaching
`objman>Call New Ovl 1(1)` and well beyond (bootDll linking, its
prolog/epilog, `objman>ObjectSetup end`, the boot-warning input-wait loop)
before exiting 0 at the full 600-tick budget -- zero `[LAUNCHER]` lines
(the automation contract holds), clone-scoped kill correctly reporting
nothing left to kill, lock released cleanly.

**Idempotency**: re-running the tool against the already-fully-set-up
Stage A tree (no `--disc` this time) hit every fast path --
`decomp checkout already at the pinned commit ... skipping clone/fetch`,
`disc already extracted at ... skipping`, `Aurora already built and
ready`, and a `tools/build.py` pass reporting `Compiled: 0
Skipped(up-to-date): 107` / `82` -- confirming a second run costs nothing.

**Android** (bonus, beyond the core acceptance criteria -- see section 6):
the full `--android` lane (SDK/NDK detect, headless `.so`, windowed `.so`
against the RmlUi-enabled android Aurora tree, gradle `assembleDebug`) was
also run for real against this workspace's pre-provisioned Android
toolchain, catching and fixing the APK-filename bug described in section 6.

**Toolchain fetch logic** (partially exercised): the zig toolchain was
already present, so the full download+extract path wasn't re-run (same
reasoning as Aurora) -- but the `ziglang.org/download/index.json` lookup
itself *was* exercised for real: fetched the live index (24 versions),
resolved the exact pinned version (`0.16.0`) for `x86_64-windows`, and got
back a real tarball URL
(`https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip`), a
real sha256, and a real size (97,217,739 bytes) -- confirming the
URL-construction/version-parsing logic against the live official source,
short of re-downloading ~97MB this machine already has.

## 8. Bugs found and fixed while building and verifying this tool

Both found by actually running the pipeline, not by inspection:

1. **Stale decomp pin** (`docs/DECOMP_DEPENDENCY.md`, section 4) -- the
   documented pin was one commit behind what the port's own tip commit
   already required. A literal reading would have produced a real,
   reproducible build break.
2. **Windows path length** (`git clone`/`checkout`, section 3) -- a
   deeply-nested destination directory pushed a real `git clone` past
   Win32's classic `MAX_PATH` ("Filename too long"). Fixed with `-c
   core.longpaths=true` on every decomp/Aurora clone and checkout.
3. **Wrong APK output path** (`setup/lib/step_android.py`, section 6) --
   hardcoded `app-debug.apk` name; this project's actual (per-ABI-split)
   gradle config produces `app-arm64-v8a-debug.apk`. Fixed by searching
   the output directory instead of guessing the filename.
