# Building

Everything is driven by `tools/build.py` (no CMake for the game itself;
the top-level `CMakeLists.txt` is a thin historical shim). The compiler
is the workspace's own zig toolchain (`port/toolchain/`) for Windows and
the Android NDK's clang for Android. Prerequisites in all cases: the
sibling decomp checkout (see `DECOMP_DEPENDENCY.md`) and the extracted
disc tree (`external_refs/repos/marioparty6/orig/GP6E01/{sys,files}`,
pulled from the user's own disc — never checked in).

## Windows

```
# windowed (the deliverable) -> build/mp6native.exe
python tools/build.py

# headless (no Aurora/SDL anywhere in the link) -> build/mp6native_headless.exe
python tools/build.py --headless

# run
build\mp6native.exe            # unlimited ticks, launcher menu
build\mp6native.exe 600        # 600-tick bounded run (automation mode)
build\mp6native_headless.exe 600
```

Useful flags: `--clean`, `-j N`, `--link-only`, `--coro-fibers` (A/B
lever: win32-fiber coroutine backend instead of the default arena-backed
minicoro; separate `_corofib`-suffixed outputs).

The build pipeline stages everything under `build/`: `patched-src/` (the
decomp sources with `patches/decomp/` applied — the decomp checkout is
read-only), `patched-include/` (shadow ABI headers: `GXTlutObj`,
`PADStatus`, the 3-arg `GXSetArray` pin), and `msl_override/` (below).
The link is fixed at image base `0x10000000` with ASLR off — several
subsystems depend on the image sitting below 4GB (see ARCHITECTURE.md),
and the crash handler symbolizes against the PDB emitted next to the
exe.

**MSL override machinery, in one paragraph:** the decomp's
`include/msl/` headers shadow the C standard library with MWCC-era
declarations that clang cannot use, but decomp sources include them by
the standard names (`stdio.h`, `math.h`, ...). `patch_msl_override()`
generates a `build/msl_override/` directory of same-named headers that
each `#include` the toolchain's REAL libc header by absolute path, and
puts that directory first on the include path — so decomp code gets the
host libc under the names it already uses, without touching the decomp
tree. (`humath.h` gets the same treatment for its `math.h` reference;
the generated files embed absolute toolchain paths, which is why each
target builds its own set.)

**SDK shim generation (occasional, not part of every build):**
`platform/null/shims_generated.c` / `shims_generated_aurora.c` (one
logging no-op per SDK symbol nothing else provides) are checked-in
generated output, not produced by `build.py` itself. Re-run
`python tools/gen_shims.py` after adding a new SDK call site or updating
`port/planning/sdk_surface.json`; it scans the decomp's own
`include/dolphin`/`include/msm` headers for each missing symbol's real
prototype (copying the parameter list verbatim so the shim's signature
always matches), skips symbols a macro resolves away or that expand to
pure preprocessor arithmetic with no call at all, and excludes any
symbol in its curated manual-implementation list (those get real
behavior in `platform/null/shims_manual.c`, `platform/os/arena.c`, etc.
instead of a stub).

## Aurora build trees (one-time per Aurora update)

The windowed build links prebuilt Aurora archives from
`external_refs/repos/aurora`, with this port's patch series
(`platform/gx/aurora-patches/`) applied on top. Two trees:

- `aurora/build/` — plain Aurora (gx/vi/pad/mtx/si/card + Dawn/SDL3).
- `aurora/build-rmlui/` — the same, built with RmlUi enabled; the
  launcher links these archives. (abseil is fetched into each tree's
  `_deps/` by Aurora's own CMake.)

Both are ordinary CMake builds using the same zig toolchain; build the
specific target libraries build.py's link line names (it will tell you
what is missing). When invoking CMake from an MSYS2/Git-Bash shell, set
`MSYS2_ARG_CONV_EXCL="*"` so `-D...=path` arguments are not mangled by
path conversion.

## Android

```
# headless .so + on-device launcher -> build/android/
python tools/build.py --target aarch64-android

# windowed (aurora/SDL3/Dawn) .so + APK bootstrap shell -> build/android/aurora/
#   (requires external_refs/repos/aurora/build-android)
python tools/build.py --target aarch64-android --windowed

# APK
cd platforms/android && gradlew.bat assembleDebug
```

Needs an NDK (path resolved via `platforms/android/local.properties` —
create it with `sdk.dir=...` if gradle can't find your SDK). Asset
staging on device: the headless smoke layout lives at
`/data/local/tmp/mp6/GP6E01/...` (build.py prints the exact `adb push`
recipe after an android build); the APK reads the same `GP6E01` tree
from the app's external files dir
(`/sdcard/Android/data/com.mp6.game/files/mp6/`), and for app-private
paths the `run-as` bridge copies staged files into place. The per-merge
android gate is `tools/gate_android.py` (see TESTING.md).

## Troubleshooting

- **`local.properties` missing / gradle can't find SDK** — create
  `platforms/android/local.properties` with `sdk.dir=C:\\...\\Android\\Sdk`.
- **CMake/configure mangles `-D` paths under MSYS2** — export
  `MSYS2_ARG_CONV_EXCL="*"` for the configure step.
- **"couldn't find the real <header>" warnings** — the MSL override
  generator couldn't locate the toolchain libc; check the zig/NDK
  installation the build is pointed at.
- **Disc tree not found at runtime** — the exe resolves the disc
  relative to its own location for this workspace's layout, then falls
  back to the build-time paths; the launcher can also set a content
  root explicitly. The boot log's `[DVD]` lines say which root won.
