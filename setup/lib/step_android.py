"""setup/lib/step_android.py -- step 6 (optional, --android): the Android
lane.

Mirrors the Windows detect-and-reuse philosophy: the Android SDK/NDK and
Aurora's two android CMake trees (build-android, build-android-rmlui) are
heavy, one-time, machine-local artifacts, so this step detects and reuses
them if present and gives clear, correct instructions when they aren't --
it does NOT silently install an SDK or accept Google's SDK license on the
user's behalf (license acceptance is always a distinct, visible,
user-confirmed step: see _ensure_sdk_licenses()'s docstring).

On-device asset import (the user's disc, picked/streamed on the phone
itself) is handled entirely by the APK's own first-run onboarding dialog
(platform/gx/ui/content_setup.cpp) -- this setup
tool's job stops at producing an installable APK.
"""
import os
import sys

from . import common


def _load_build_module():
    tools_dir = os.path.join(common.NATIVE_ROOT, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import build
    return build


def _find_ndk():
    env = os.environ.get("ANDROID_NDK_ROOT")
    if env and os.path.isdir(env):
        return env
    ndk_base = os.path.join(common.PORT_ROOT, "android-sdk", "ndk")
    if os.path.isdir(ndk_base):
        vers = sorted(d for d in os.listdir(ndk_base) if os.path.isdir(os.path.join(ndk_base, d)))
        if vers:
            return os.path.join(ndk_base, vers[-1])
    return None


def _find_sdk_root():
    env = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if env and os.path.isdir(env):
        return env
    default = os.path.join(common.PORT_ROOT, "android-sdk")
    return default if os.path.isdir(default) else None


def _print_sdk_install_recipe():
    common.banner("Android SDK/NDK not found")
    print("""
  This tool does not silently install the Android SDK or accept Google's
  SDK license on your behalf -- that's a deliberate choice (license
  acceptance is something only you can agree to). To provision it:

  1. Install Android Studio (https://developer.android.com/studio), OR just
     the command-line tools (https://developer.android.com/studio#command-tools).
  2. Using its SDK Manager (GUI) or `sdkmanager` (CLI), install:
       platform-tools, build-tools;35.0.0, platforms;android-36,
       ndk;27.3.13750724
  3. Review and accept the licenses yourself:
       sdkmanager --licenses
  4. Either set ANDROID_SDK_ROOT / ANDROID_NDK_ROOT, or point this project's
     port/android-sdk/ at your install (see platforms/android/local.properties).

  Re-run this tool with --android once that's done.
""")


def ensure_android_toolchain():
    ndk = _find_ndk()
    sdk = _find_sdk_root()
    if not ndk or not sdk:
        _print_sdk_install_recipe()
        return None, None
    common.ok(f"Android SDK: {sdk}")
    common.ok(f"Android NDK: {ndk}")
    return sdk, ndk


def _ensure_local_properties(sdk_root):
    local_props = os.path.join(common.NATIVE_ROOT, "platforms", "android", "local.properties")
    if os.path.exists(local_props):
        return
    common.info(f"writing {local_props} (sdk.dir)")
    with open(local_props, "w", encoding="utf-8") as f:
        f.write(f"sdk.dir={sdk_root.replace(chr(92), '/')}\n")


def build_headless(native_root, jobs=None):
    common.info("building the Android headless row: libmp6game.so + mp6launcher -> build/android/")
    cmd = [sys.executable, os.path.join(native_root, "tools", "build.py"), "--target", "aarch64-android"]
    if jobs:
        cmd += ["-j", str(jobs)]
    common.run(cmd, cwd=native_root)
    so = os.path.join(native_root, "build", "android", "libmp6game.so")
    if not os.path.exists(so):
        raise common.SetupError(f"android headless build reported success but {so} doesn't exist")
    common.ok(f"built {so}")
    return so


def windowed_ready():
    build = _load_build_module()
    ok = os.path.isdir(build.AURORA_BUILD_ANDROID_RMLUI) and os.path.exists(build.NOD_ANDROID_LIB)
    return ok, build.AURORA_BUILD_ANDROID_RMLUI, build.NOD_ANDROID_LIB


def build_windowed(native_root, jobs=None):
    """Raises rather than returning None when the graphics deps are absent: the
    user asked for --android, and without this row there is no APK to install,
    so a "skip" here is a FAILURE of the requested target, not a success."""
    ready, aurora_tree, nod_lib = windowed_ready()
    if not ready:
        missing = []
        if not os.path.isdir(aurora_tree):
            missing.append(f"{aurora_tree} -- see setup/README.md for the cmake recipe "
                           "(same shape as the Windows Aurora trees, NDK-toolchained)")
        if not os.path.exists(nod_lib):
            missing.append(f"{nod_lib} -- run `python tools/fetch_nod.py --android` "
                           "(needs the self-contained rust toolchain; see that script's docstring)")
        raise common.SetupError(
            "the Android windowed (aurora/SDL3/Dawn) graphics build can't run here, so no APK can be "
            "produced:\n    " + "\n    ".join(missing),
            hint="provision the above and re-run with --android (the headless row that already built "
                 "is still in build/android/)")

    common.info("building the Android windowed row: libmp6game.so (aurora/SDL3/Dawn) -> build/android/aurora/, "
                "staging jniLibs for platforms/android")
    cmd = [sys.executable, os.path.join(native_root, "tools", "build.py"),
           "--target", "aarch64-android", "--windowed"]
    if jobs:
        cmd += ["-j", str(jobs)]
    common.run(cmd, cwd=native_root)
    jnilibs = os.path.join(native_root, "platforms", "android", "app", "src", "main", "jniLibs", "arm64-v8a")
    if not os.path.isdir(jnilibs) or not os.listdir(jnilibs):
        raise common.SetupError(f"windowed android build reported success but {jnilibs} is empty")
    common.ok(f"staged jniLibs: {jnilibs}")
    return jnilibs


def build_apk(native_root):
    android_dir = os.path.join(native_root, "platforms", "android")
    gradlew = os.path.join(android_dir, "gradlew.bat" if common.IS_WINDOWS else "gradlew")
    if not os.path.exists(gradlew):
        raise common.SetupError(f"{gradlew} not found")
    sdk_root = _find_sdk_root()
    if sdk_root:
        _ensure_local_properties(sdk_root)  # defensive: correct even if called before run_android_lane()
    common.info("running gradle assembleDebug (this can take a while the first time -- gradle "
                "downloads its own wrapper distribution + dependencies)")
    if not common.IS_WINDOWS:
        common.run(["chmod", "+x", gradlew], check=False)
    common.run([gradlew, "assembleDebug"], cwd=android_dir)
    # The exact APK filename depends on the project's ABI-split/flavor config
    # (this project's is arm64-v8a-only, so AGP names it
    # app-arm64-v8a-debug.apk, not the no-split app-debug.apk one might
    # guess) -- search the debug output dir instead of hardcoding a name.
    debug_out = os.path.join(android_dir, "app", "build", "outputs", "apk", "debug")
    apks = [os.path.join(debug_out, f) for f in os.listdir(debug_out)] if os.path.isdir(debug_out) else []
    apks = [f for f in apks if f.lower().endswith(".apk")]
    if not apks:
        raise common.SetupError(f"gradle reported success but no .apk found under {debug_out}")
    apk = apks[0]
    common.ok(f"built {apk}")
    return apk


def print_onboarding_note():
    common.banner("Android: how the disc gets onto the device")
    print("""
  This setup tool does NOT extract your disc for Android -- the APK does
  that itself, on first run, via its own onboarding flow:
    platform/gx/ui/content_setup.cpp / .hpp   (the onboarding dialog + import
                                                trigger, reusing the same nod
                                                library this tool used for Windows)

  Install the APK, launch it, and follow the on-device prompt to pick your
  disc image (or an already-extracted folder via Android's document picker).
""")


def run_android_lane(native_root, jobs=None, assume_yes=False, skip_apk=False):
    """--android was explicitly asked for, so every row it implies is REQUIRED:
    a missing SDK/NDK or missing graphics deps raises instead of quietly
    returning an empty result the caller then prints "Done" over."""
    sdk, ndk = ensure_android_toolchain()
    if not sdk or not ndk:
        print_onboarding_note()
        raise common.SetupError(
            "--android: no Android SDK/NDK found, so the Android lane produced nothing",
            hint="follow the SDK/NDK recipe printed above, then re-run with --android "
                 "(drop --android to build the desktop rows only)")
    _ensure_local_properties(sdk)

    result = {"headless": None, "windowed": None, "apk": None}
    result["headless"] = build_headless(native_root, jobs=jobs)
    result["windowed"] = build_windowed(native_root, jobs=jobs)
    if not skip_apk:
        result["apk"] = build_apk(native_root)
    print_onboarding_note()
    return result
