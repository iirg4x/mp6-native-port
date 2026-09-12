# Releasing

Build and tag a reviewed commit in this repository. Release packages contain
the native executable, its redistributable dependencies and launcher resources.
Never include the user's disc image, extracted game assets, saves, config,
test logs or save states.

## Windows

Follow [BUILDING.md](BUILDING.md) to build the optimized renderer and then
`python tools/build.py --configuration release`.

Stage `build/release/mp6native.exe`, the runtime DLLs copied beside it by the
build, and `build/res/` as `res/` beside the packaged executable. The build
driver's `AURORA_RUNTIME_DLLS` list is authoritative; release and Debug PNG
libraries have different filenames. Do not ship the Debug renderer or the
headless test executable as the player build.

Verify the staged folder starts with a fresh configuration and can import a
user-provided disc. Run the tests in [TESTING.md](TESTING.md) and inspect the
archive contents before publishing.

## Android

Use `python tools/release_android.py --help` for the validated Android release
workflow. Gradle packaging lives in `packaging/android/`; signing keys remain
outside the repository. Verify installation on an arm64 device.

## Version and release assets

Keep `VERSION`, `VERSION_CODE` and Android version metadata consistent.
Tag format: `vMAJOR.MINOR.PATCH`. Include the source revision in asset names:

```text
mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-win-x64.zip
mp6-native-port-vMAJOR.MINOR.PATCH-<shorthash>-android-arm64.apk
```

The web packager chooses the Windows zip and Android APK from GitHub's latest
release. Its implementation lives in `packaging/web/js/github-release.js`.

After publishing, download the assets again and smoke-test those exact files.
Check that `packaging/web/packager.html` discovers the release. The Pages
workflow publishes `packaging/web/`; it does not include game content.
