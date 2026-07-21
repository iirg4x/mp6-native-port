# Releasing

This is the checklist for cutting a GitHub Release with downloadable
assets. Read the [README's "Get it"](../README.md#get-it) section first if
you haven't -- the policy it states is load-bearing here: **release assets
are content-free engine binaries only. No game code, no game assets, ever.**
Nothing in this checklist changes that; it's entirely about packaging the
engine this project builds from its own decompiled/reimplemented source.

## 0. Prerequisites

- **Build from a synced snapshot, not a live working tree.** This repo
  (`mp6-port-clean`) is the clean, public snapshot of the port's source --
  make sure the commit you're releasing from is actually pushed and is the
  real HEAD of the branch you're releasing (`main`/`master`), not a local
  working tree with uncommitted changes, stray test artifacts, or logs
  sitting around. If your source lives in a separate internal/working
  checkout, sync it into this repo (commit + push) *before* you build
  anything for release -- a release asset must be reproducible from exactly
  what's in this public repo at the tagged commit, or the whole point of
  publishing the source is undermined.
- Clean build directories (`build/`, `platforms/android/app/build/`, etc.)
  before building release artifacts, so nothing stale leaks in:
  `python tools/build.py --clean`.
- Decide the version number (see below) and make sure
  `platforms/android/app/build.gradle`'s `versionName`/`versionCode` are
  bumped to match if this release includes an Android asset.

## 1. Version + naming scheme

Tag format: `vMAJOR.MINOR.PATCH` (plain semver, e.g. `v0.1.0`), on the
commit being released. Asset filenames embed both that version *and* the
short commit hash they were built from, so a downloaded binary can always
be traced back to the exact source that produced it:

```
mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-win-x64.zip
mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-android-arm64.apk
```

`<shorthash>` is the 7-character short form of the release commit's SHA
(`git rev-parse --short HEAD`). Example:
`mp6-native-port-v0.1.0-8e42e00-win-x64.zip`.

Bump MINOR for a new-capability release (e.g. gameplay past the menu
eventually landing), PATCH for a fix-only release, MAJOR once the project
considers itself feature-complete enough for that to mean something (not
yet).

## 2. Windows zip contents

Build first: `python tools/build.py` (windowed, the deliverable --
see [BUILDING.md](BUILDING.md)). Then assemble a zip containing **exactly**:

```
mp6native.exe
SDL3.dll
webgpu_dawn.dll
dxcompiler.dll
dxil.dll
libpng16d.dll
libzlib1.dll
nod.dll
res/                    (the whole tree: res/fonts/, res/rml/)
```

That DLL list is exactly what `tools/build.py`'s own `AURORA_RUNTIME_DLLS`
copies next to `mp6native.exe` for a normal windowed build (SDL3 + Dawn/
WebGPU + its DirectX shader compiler pair + libpng/libzlib + nod, the disc-
image reader) -- if a future Aurora/dependency bump changes that set,
`tools/build.py` is the source of truth, not this list.

**Explicitly do NOT include:**

- `mp6native.pdb` (or any `.pdb`) -- debug symbols, not needed to run and
  not something to publish alongside an otherwise-content-free binary.
- `mp6native_headless.exe` -- the CI/automation build (no window, no audio
  device), not a player-facing artifact.
- `mp6native_corofib.exe` (and its `.pdb`) -- the win32-fiber coroutine
  A/B variant (`--coro-fibers`), a development lever, not a release target.
- `mp6_config.json`, `saves/`, anything under `build/` other than the exe
  itself -- those are runtime-generated, per-user state, not part of a
  fresh install.

Verify before zipping: `dir` the staged folder and diff it against the list
above by hand -- it's short enough that a mismatch should jump out.

## 3. Android APK

```
cd platforms/android
gradlew.bat assembleRelease
```

Output lands under `platforms/android/app/build/outputs/apk/release/`.
`build.gradle` currently defines a `release` build type with no
`signingConfig`, so this produces an **unsigned** APK -- sign it (a
self-managed keystore is fine; this isn't a Play Store submission) before
publishing, or `apksigner sign --ks <keystore> app-release-unsigned.apk`
by hand if no Gradle signing config exists yet. Confirm it installs on a
real arm64 device afterward -- an unsigned or mis-signed APK will fail to
install rather than failing loudly at build time.

Sanity checks before publishing:

- Size is in the "clean APK" ballpark (~30MB) -- if it's dramatically
  larger, something (a stray asset, a debug build accidentally packaged)
  probably slipped in.
- `unzip -l` the APK and confirm there's nothing under `assets/` beyond
  `assets/res/...` (the launcher's own fonts/RmlUi stylesheets, synced by
  the `syncMp6Assets` Gradle task) -- no `GP6E01`, no `data/`, no game
  content of any kind.
- The `splits.abi` config in `app/build.gradle` restricts this to
  `arm64-v8a` (`universalApk false`) -- confirm the built APK is the arm64
  split, not a stray universal build from a different Gradle invocation.

## 4. Publishing the release

```
git tag vMAJOR.MINOR.PATCH
git push origin vMAJOR.MINOR.PATCH

gh release create vMAJOR.MINOR.PATCH \
  "mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-win-x64.zip" \
  "mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-android-arm64.apk" \
  --title "vMAJOR.MINOR.PATCH" \
  --notes "..."
```

Release notes should restate, briefly, that both assets are content-free
engine builds and link back to the README's "Get it" section for the full
policy -- don't assume every download reads the README first.

`web/js/github-release.js` (the packager's auto-fetch) picks the asset
whose name matches `win.*\.zip$` (falling back to any `*.zip`) for Windows
and `*.apk$` for Android against the repo's `/releases/latest` API, so as
long as the filenames above end that way, naming the rest of the string
however this scheme says is enough -- no site change needed per release.

## 5. After publishing

- [ ] Download both assets fresh (not from local build output) and smoke-
      test them -- the win zip against a real disc via the web packager,
      the APK on a real device.
- [ ] Confirm `web/packager.html`'s auto-fetch finds the new release (it
      calls the public `/releases/latest` API -- no site deploy needed for
      this to pick up a new release).
- [ ] If this is the *first* release: this is also the point to enable
      GitHub Pages (Settings -> Pages -> deploy from the `.github/workflows/
      pages.yml` workflow) so `web/` actually becomes reachable. Until Pages
      is turned on, that workflow exists but is inert.
