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
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
import xml.etree.ElementTree as ET

from . import common


_GRADLE_WRAPPER_PROPERTIES = {
    "distributionBase": "GRADLE_USER_HOME",
    "distributionPath": "wrapper/dists",
    "distributionSha256Sum": "20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78",
    "distributionUrl": r"https\://services.gradle.org/distributions/gradle-8.13-bin.zip",
    "networkTimeout": "10000",
    "validateDistributionUrl": "true",
    "zipStoreBase": "GRADLE_USER_HOME",
    "zipStorePath": "wrapper/dists",
}
_GRADLE_WRAPPER_FILES = {
    "gradlew": "734b3879d3501dce471cf0522d3bcbafe76873d9fc5129345b67fb43bd15e933",
    "gradlew.bat": "57931b17dd228e5c24dac90e815d0bf82477e831a4618dfab4136f5446b42a9f",
    "gradle/wrapper/gradle-wrapper.jar":
        "81a82aaea5abcc8ff68b3dfcb58b3c3c429378efd98e7433460610fecd7ae45f",
}
_GRADLE_VERIFICATION_METADATA_SHA256 = "6bb9bd73b1ae442c10c34a8d65c8691478190ad1382aad1682303ad6db9ac3c9"
_ANDROID_LINT_BASELINE_SHA256 = "6909e65ce01ae87d3a7bef49177602126f75c52b7071c34027f817f688894f6a"
_ANDROID_COMPILE_SDK = "android-36"
_ANDROID_BUILD_TOOLS_VERSION = "35.0.0"
_ANDROID_NDK_VERSION = "27.3.13750724"
_ANDROID_NATIVE_MANIFEST_VERSION = 2
_ANDROID_NATIVE_OPTIMIZATION = {"debug": "-O0", "release": "-O2"}
_ANDROID_RELEASE_SAFETY_FLAGS = ["-fno-strict-aliasing"]
_ANDROID_NATIVE_TARGET = "aarch64-linux-android28"
_ANDROID_PLATFORM_FILES = {
    "android.jar": {"bytes": 27768026, "sha256": "d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a"},
    "core-for-system-modules.jar": {"bytes": 1464749, "sha256": "4afd3df39082ca1ca32a1effeaff7971d878e75bd01a68a4205c105deec4c559"},
    "framework.aidl": {"bytes": 121424, "sha256": "d45522596d198f64877be16fef2c8c3384b095e83eafec46976f5c5836619b20"},
    "data/annotations.zip": {"bytes": 229751, "sha256": "be8729cd1da17ccaf68b8c6fd48fbd7c412ce060413be31488ab1791baf49880"},
    "data/api-versions.xml": {"bytes": 5674817, "sha256": "923f7e6367759e127679b6b5a4156ee801100e3cc07dc1d99cb811e791e45435"},
    "source.properties": {"bytes": 257, "sha256": "72291ff6611dbfc46fdfb6de2821c4a62c87befa5200713847eda426e3a501c2"},
    "package.xml": {"bytes": 18646, "sha256": "0dc071809e3df6e8a1a98bbda66753722804737105f20cd75d1f3c33c4d772b9"},
}
_ANDROID_BUILD_TOOLS_TREE = {
    "files": 170,
    "bytes": 144869412,
    "sha256": "93fb41212246011263f5661044ff5645ffda7a0ac0460a66d37e8bb9dddef246",
}
_ANDROID_JDK_VERSION_OUTPUT = (
    'java version "22.0.2" 2024-07-16\n'
    'Java(TM) SE Runtime Environment (build 22.0.2+9-70)\n'
    'Java HotSpot(TM) 64-Bit Server VM (build 22.0.2+9-70, mixed mode, sharing)'
)
_ANDROID_JDK_TREE = {
    "files": 418,
    "bytes": 327361382,
    "sha256": "66ef963be5c45de41ba26db7c5dd13554838823bfd944bde6199c7dbf930d319",
}


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _bounded_tree_fingerprint(root, label, max_files=4096, max_bytes=1024 * 1024 * 1024):
    root = os.path.realpath(root)
    if not os.path.isdir(root):
        raise common.SetupError(f"required {label} directory is missing: {root}")
    digest = hashlib.sha256()
    count = 0
    total_bytes = 0
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        files.sort()
        for dirname in dirs:
            path = os.path.join(current, dirname)
            if os.path.islink(path):
                raise common.SetupError(f"{label} contains an unsupported directory link: {path}")
        for filename in files:
            path = os.path.join(current, filename)
            if os.path.islink(path) or not os.path.isfile(path):
                raise common.SetupError(f"{label} contains an unsupported file/link: {path}")
            size = os.path.getsize(path)
            count += 1
            total_bytes += size
            if count > max_files or total_bytes > max_bytes:
                raise common.SetupError(
                    f"{label} exceeds fingerprint bounds ({count} files, {total_bytes} bytes)"
                )
            relative = os.path.relpath(path, root).replace("\\", "/").encode("utf-8")
            digest.update(len(relative).to_bytes(4, "big"))
            digest.update(relative)
            digest.update(size.to_bytes(8, "big"))
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b""):
                    digest.update(chunk)
    return {"files": count, "bytes": total_bytes, "sha256": digest.hexdigest()}


def verify_android_sdk(sdk_root, platform_files=None, build_tools_tree=None):
    """Authenticate the SDK inputs that compile, lint, dex, align, and sign APKs."""
    sdk_root = os.path.realpath(os.fspath(sdk_root))
    platform_files = _ANDROID_PLATFORM_FILES if platform_files is None else platform_files
    build_tools_tree = _ANDROID_BUILD_TOOLS_TREE if build_tools_tree is None else build_tools_tree
    platform_root = os.path.join(sdk_root, "platforms", _ANDROID_COMPILE_SDK)
    for relative, expected in sorted(platform_files.items()):
        path = os.path.join(platform_root, *relative.split("/"))
        try:
            size = os.path.getsize(path)
            digest = _sha256_file(path)
        except OSError as exc:
            raise common.SetupError(f"cannot read pinned Android SDK platform input {path}: {exc}") from exc
        actual = {"bytes": size, "sha256": digest}
        if actual != expected:
            raise common.SetupError(
                f"Android SDK {_ANDROID_COMPILE_SDK} integrity failure for {path}: "
                f"expected {expected!r}, found {actual!r}"
            )
    build_tools_root = os.path.join(sdk_root, "build-tools", _ANDROID_BUILD_TOOLS_VERSION)
    actual_tree = _bounded_tree_fingerprint(build_tools_root, "Android Build Tools")
    if actual_tree != build_tools_tree:
        raise common.SetupError(
            f"Android Build Tools {_ANDROID_BUILD_TOOLS_VERSION} integrity failure: "
            f"expected {build_tools_tree!r}, found {actual_tree!r}"
        )
    common.ok(
        f"verified Android SDK {_ANDROID_COMPILE_SDK} compile/lint inputs and "
        f"Build Tools {_ANDROID_BUILD_TOOLS_VERSION} full tree"
    )
    return True


def _java_version_output(java_exe):
    try:
        proc = subprocess.run(
            [java_exe, "-version"], capture_output=True, text=True,
            encoding="utf-8", errors="replace",
        )
    except OSError as exc:
        raise common.SetupError(f"cannot run pinned Android packaging JDK {java_exe}: {exc}") from exc
    output = "\n".join(part.strip() for part in (proc.stdout, proc.stderr) if part.strip())
    if proc.returncode != 0:
        raise common.SetupError(f"pinned Android packaging JDK failed: {output}")
    return output


def verify_android_jdk(jdk_home=None, expected_tree=None, expected_version=None):
    if jdk_home is None:
        jdk_home = os.environ.get("MP6_JAVA_HOME") or os.environ.get("JAVA_HOME")
        if not jdk_home and common.IS_WINDOWS:
            jdk_home = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Java", "jdk-22")
    if not jdk_home:
        raise common.SetupError(
            "the pinned Android packaging JDK is unavailable; set MP6_JAVA_HOME to Oracle JDK 22.0.2"
        )
    jdk_home = os.path.realpath(os.fspath(jdk_home))
    expected_tree = _ANDROID_JDK_TREE if expected_tree is None else expected_tree
    expected_version = _ANDROID_JDK_VERSION_OUTPUT if expected_version is None else expected_version
    actual_tree = _bounded_tree_fingerprint(jdk_home, "Android packaging JDK")
    if actual_tree != expected_tree:
        raise common.SetupError(
            f"Android packaging JDK integrity failure at {jdk_home}: "
            f"expected {expected_tree!r}, found {actual_tree!r}"
        )
    java_exe = os.path.join(jdk_home, "bin", "java.exe" if common.IS_WINDOWS else "java")
    actual_version = _java_version_output(java_exe)
    if actual_version != expected_version:
        raise common.SetupError(
            f"Android packaging JDK version mismatch: expected {expected_version!r}, "
            f"found {actual_version!r}"
        )
    common.ok("verified pinned Oracle JDK 22.0.2 full runtime tree")
    return jdk_home


def _verify_and_prune_gradle_artifact_cache(native_root, gradle_home):
    metadata_path = os.path.join(
        os.fspath(native_root), "platforms", "android", "gradle", "verification-metadata.xml"
    )
    if not os.path.isfile(metadata_path):
        return 0
    namespace = "https://schema.gradle.org/dependency-verification"
    q = lambda name: f"{{{namespace}}}{name}"
    root = ET.parse(metadata_path).getroot()
    expected = {}
    for component in root.findall(f".//{q('component')}"):
        coordinate = (
            component.get("group"), component.get("name"), component.get("version")
        )
        for artifact in component.findall(q("artifact")):
            hashes = {child.get("value") for child in artifact.findall(q("sha256"))}
            expected[coordinate + (artifact.get("name"),)] = hashes
    cache_root = os.path.join(gradle_home, "caches", "modules-2", "files-2.1")
    if not os.path.isdir(cache_root):
        return 0
    verified = 0
    for current, _dirs, files in os.walk(cache_root):
        for filename in files:
            path = os.path.join(current, filename)
            relative = os.path.relpath(path, cache_root).split(os.sep)
            if len(relative) != 5:
                raise common.SetupError(f"unexpected Gradle artifact-cache path: {path}")
            key = (relative[0], relative[1], relative[2], relative[4])
            allowed = expected.get(key)
            if not allowed:
                # Resolution probes (for example lint's latest-version check)
                # are not build inputs. Remove them so they cannot persist as
                # an unauthenticated input to a later invocation.
                os.remove(path)
                continue
            actual = _sha256_file(path)
            if actual not in allowed:
                raise common.SetupError(
                    f"Gradle dependency cache integrity failure for {path}: "
                    f"expected one of {sorted(allowed)}, found {actual}"
                )
            verified += 1
    return verified


def _purge_gradle_derived_state(native_root, gradle_home):
    caches = os.path.join(gradle_home, "caches")
    targets = [
        os.path.join(os.fspath(native_root), "platforms", "android", ".gradle"),
        os.path.join(caches, "jars-9"),
        os.path.join(caches, "8.13", "transforms"),
        os.path.join(caches, "8.13", "generated-gradle-jars"),
        os.path.join(caches, "8.13", "groovy-dsl"),
        os.path.join(caches, "8.13", "kotlin-dsl"),
    ]
    modules = os.path.join(caches, "modules-2")
    if os.path.isdir(modules):
        targets.extend(
            os.path.join(modules, name) for name in os.listdir(modules)
            if name.startswith("metadata-")
        )
    for target in targets:
        if os.path.isdir(target):
            shutil.rmtree(target)


def _controlled_gradle_environment(native_root):
    jdk_home = verify_android_jdk()
    gradle_home = os.path.join(os.fspath(native_root), "build", "gradle-user-home")
    os.makedirs(gradle_home, exist_ok=True)
    forbidden = [
        os.path.join(gradle_home, "init.gradle"),
        os.path.join(gradle_home, "init.gradle.kts"),
        os.path.join(gradle_home, "gradle.properties"),
    ]
    init_dir = os.path.join(gradle_home, "init.d")
    if os.path.isdir(init_dir):
        forbidden.extend(os.path.join(init_dir, name) for name in os.listdir(init_dir))
    present = [path for path in forbidden if os.path.exists(path)]
    if present:
        raise common.SetupError(
            "controlled Gradle user home contains forbidden init/property files: "
            + ", ".join(present)
        )
    verified = _verify_and_prune_gradle_artifact_cache(native_root, gradle_home)
    _purge_gradle_derived_state(native_root, gradle_home)
    env = dict(os.environ)
    blocked = {
        "GRADLE_OPTS", "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS",
        "GRADLE_USER_HOME", "JAVA_HOME",
    }
    for name in list(env):
        if name.upper() in blocked or name.upper().startswith("ORG_GRADLE_PROJECT_"):
            env.pop(name, None)
    env["JAVA_HOME"] = jdk_home
    env["GRADLE_USER_HOME"] = gradle_home
    env["PATH"] = os.path.join(jdk_home, "bin") + os.pathsep + env.get("PATH", "")
    if verified:
        common.ok(f"verified {verified} cached Gradle artifacts; purged derived script/transform state")
    return env


def verify_gradle_wrapper(native_root):
    """Fail closed before executing any part of the checked-in wrapper."""
    android_dir = os.path.join(os.fspath(native_root), "platforms", "android")
    properties_path = os.path.join(android_dir, "gradle", "wrapper", "gradle-wrapper.properties")
    try:
        with open(properties_path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as exc:
        raise common.SetupError(f"cannot read Gradle wrapper properties {properties_path}: {exc}") from exc

    actual_properties = {}
    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", "!")):
            continue
        if "=" not in stripped:
            raise common.SetupError(
                f"invalid Gradle wrapper property at {properties_path}:{line_number}"
            )
        key, value = (part.strip() for part in stripped.split("=", 1))
        if not key or key in actual_properties:
            raise common.SetupError(
                f"duplicate/empty Gradle wrapper property at {properties_path}:{line_number}"
            )
        actual_properties[key] = value
    if actual_properties != _GRADLE_WRAPPER_PROPERTIES:
        raise common.SetupError(
            "Gradle wrapper properties differ from the audited Gradle 8.13 contract: "
            f"expected {_GRADLE_WRAPPER_PROPERTIES!r}, found {actual_properties!r}"
        )

    for relative, expected_hash in _GRADLE_WRAPPER_FILES.items():
        path = os.path.join(android_dir, *relative.split("/"))
        try:
            actual_hash = _sha256_file(path)
        except OSError as exc:
            raise common.SetupError(f"cannot read required Gradle wrapper file {path}: {exc}") from exc
        if actual_hash != expected_hash:
            raise common.SetupError(
                f"Gradle wrapper integrity failure for {path}: expected SHA-256 "
                f"{expected_hash}, found {actual_hash}"
            )
    common.ok("verified Gradle 8.13 wrapper scripts, bootstrap JAR, and distribution checksum")
    return True


def verify_gradle_dependency_metadata(native_root, expected_hash=None):
    path = os.path.join(
        os.fspath(native_root), "platforms", "android", "gradle", "verification-metadata.xml"
    )
    expected_hash = (_GRADLE_VERIFICATION_METADATA_SHA256
                     if expected_hash is None else expected_hash)
    try:
        actual_hash = _sha256_file(path)
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise common.SetupError(f"cannot read strict Gradle dependency metadata {path}: {exc}") from exc
    if actual_hash != expected_hash:
        raise common.SetupError(
            f"Gradle dependency metadata integrity failure: expected SHA-256 "
            f"{expected_hash}, found {actual_hash}"
        )
    namespace = "https://schema.gradle.org/dependency-verification"
    q = lambda name: f"{{{namespace}}}{name}"
    if root.tag != q("verification-metadata"):
        raise common.SetupError("Gradle dependency metadata has the wrong schema/root")
    configuration = root.find(q("configuration"))
    if (configuration is None
            or configuration.findtext(q("verify-metadata")) != "true"
            or configuration.findtext(q("verify-signatures")) != "false"):
        raise common.SetupError("Gradle dependency metadata is not configured for metadata verification")
    components = root.findall(f".//{q('component')}")
    artifacts = root.findall(f".//{q('artifact')}")
    if len(components) != 283 or len(artifacts) != 488:
        raise common.SetupError(
            f"Gradle dependency graph changed: expected 283 components/488 artifacts, "
            f"found {len(components)}/{len(artifacts)}"
        )
    required_agp = False
    for component in components:
        if component.attrib == {
                "group": "com.android.tools.build", "name": "gradle", "version": "8.13.2"}:
            required_agp = True
        for artifact in component.findall(q("artifact")):
            children = list(artifact)
            if (len(children) != 1 or children[0].tag != q("sha256")
                    or not re.fullmatch(r"[0-9a-f]{64}", children[0].get("value", ""))):
                raise common.SetupError(
                    f"Gradle artifact lacks exactly one SHA-256 pin: {component.attrib!r} "
                    f"{artifact.attrib!r}"
                )
    if not required_agp:
        raise common.SetupError("Gradle metadata does not authenticate AGP 8.13.2")
    baseline = os.path.join(
        os.fspath(native_root), "platforms", "android", "app", "lint-baseline.xml"
    )
    try:
        baseline_hash = _sha256_file(baseline)
    except OSError as exc:
        raise common.SetupError(f"cannot read reviewed Android lint baseline {baseline}: {exc}") from exc
    if baseline_hash != _ANDROID_LINT_BASELINE_SHA256:
        raise common.SetupError(
            f"reviewed Android lint baseline changed: expected SHA-256 "
            f"{_ANDROID_LINT_BASELINE_SHA256}, found {baseline_hash}"
        )
    common.ok("verified strict Gradle graph: AGP 8.13.2, 283 components, 488 SHA-256 artifacts")
    return True


def _load_build_module():
    tools_dir = os.path.join(common.NATIVE_ROOT, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import build
    return build


def _find_ndk():
    env = os.environ.get("ANDROID_NDK_ROOT")
    candidate = env if env else os.path.join(
        common.PORT_ROOT, "android-sdk", "ndk", _ANDROID_NDK_VERSION
    )
    candidate = os.path.realpath(candidate)
    if not os.path.isdir(candidate) or os.path.basename(candidate) != _ANDROID_NDK_VERSION:
        return None
    properties = os.path.join(candidate, "source.properties")
    try:
        with open(properties, "r", encoding="utf-8") as f:
            revisions = [
                line.split("=", 1)[1].strip() for line in f
                if "=" in line and line.split("=", 1)[0].strip() == "Pkg.Revision"
            ]
    except OSError:
        return None
    return candidate if revisions == [_ANDROID_NDK_VERSION] else None


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
    escaped_sdk = sdk_root.replace(chr(92), "/").replace(":", r"\:")
    wanted = f"sdk.dir={escaped_sdk}"
    lines = []
    if os.path.exists(local_props):
        with open(local_props, "r", encoding="utf-8", errors="replace") as f:
            lines = [line.rstrip("\r\n") for line in f]
    updated = [line for line in lines if not line.startswith("sdk.dir=")]
    updated.append(wanted)
    content = "\n".join(updated) + "\n"
    old = None
    if os.path.exists(local_props):
        with open(local_props, "r", encoding="utf-8", errors="replace") as f:
            old = f.read()
    if old == content:
        return
    common.info(f"writing current sdk.dir to {local_props}")
    tmp = local_props + f".part-{os.getpid()}"
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        os.replace(tmp, local_props)
    finally:
        try:
            os.remove(tmp)
        except FileNotFoundError:
            pass


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
    nod_ok, nod_problem = build.fetch_nod.check_android_install()
    ok = os.path.isdir(build.AURORA_BUILD_ANDROID_RMLUI) and nod_ok
    return ok, build.AURORA_BUILD_ANDROID_RMLUI, build.NOD_ANDROID_LIB, nod_problem


def build_windowed(native_root, jobs=None):
    """Raises rather than returning None when the graphics deps are absent: the
    user asked for --android, and without this row there is no APK to install,
    so a "skip" here is a FAILURE of the requested target, not a success."""
    ready, aurora_tree, nod_lib, nod_problem = windowed_ready()
    if not ready:
        missing = []
        if not os.path.isdir(aurora_tree):
            missing.append(f"{aurora_tree} -- see setup/README.md for the cmake recipe "
                           "(same shape as the Windows Aurora trees, NDK-toolchained)")
        if nod_problem:
            missing.append(f"{nod_lib}: {nod_problem} -- run `python tools/fetch_nod.py --android` "
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
    names = {name for name in os.listdir(jnilibs)} if os.path.isdir(jnilibs) else set()
    expected = {"libmain.so", "libmp6game.so"}
    if names != expected or any(not os.path.isfile(os.path.join(jnilibs, name)) for name in expected):
        raise common.SetupError(
            f"windowed android build staged the wrong native manifest under {jnilibs}: "
            f"expected {sorted(expected)}, found {sorted(names)}"
        )
    common.ok(f"staged jniLibs: {jnilibs}")
    return jnilibs


def verify_android_native_build_manifest(native_root, variant):
    """Bind Gradle's variant to freshly staged native bytes and compile mode."""
    variant = str(variant).lower()
    expected_optimization = _ANDROID_NATIVE_OPTIMIZATION.get(variant)
    if expected_optimization is None:
        raise common.SetupError(f"unsupported Android variant {variant!r}")
    manifest_path = os.path.join(
        os.fspath(native_root), "build", "android", "aurora", "native-build-manifest.json"
    )
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        raise common.SetupError(
            f"missing/invalid Android native build manifest {manifest_path}: {exc}",
            hint=f"run tools/build.py --target aarch64-android --windowed "
                 f"--configuration {variant} --clean first",
        ) from exc
    expected_header = {
        "manifest_version": _ANDROID_NATIVE_MANIFEST_VERSION,
        "target": _ANDROID_NATIVE_TARGET,
        "ndk_version": _ANDROID_NDK_VERSION,
        "configuration": variant,
        "game_optimization": expected_optimization,
        "game_compile_flags": ([expected_optimization]
                               + (_ANDROID_RELEASE_SAFETY_FLAGS if variant == "release" else [])),
        "windowed": True,
    }
    wrong = {key: (expected, manifest.get(key)) for key, expected in expected_header.items()
             if manifest.get(key) != expected}
    if wrong:
        raise common.SetupError(
            f"Android native build profile does not match Gradle {variant}: {wrong!r}",
            hint=f"rebuild native libraries with --configuration {variant} --clean",
        )
    compile_profile = manifest.get("compile_profile")
    if (not isinstance(compile_profile, dict)
            or not isinstance(compile_profile.get("units"), int)
            or compile_profile["units"] <= 0
            or compile_profile.get("required_flags") != expected_header["game_compile_flags"]
            or not re.fullmatch(r"[0-9a-f]{64}",
                                str(compile_profile.get("command_records_sha256", "")))
            or not re.fullmatch(r"[0-9a-f]{64}", str(manifest.get("link_command_sha256", "")))):
        raise common.SetupError(f"Android native build manifest has no valid compile/link proof: {manifest_path}")

    helper_profiles = manifest.get("helper_profiles")
    expected_helper_flags = expected_header["game_compile_flags"]
    if not isinstance(helper_profiles, dict) or set(helper_profiles) != {"libmain.so"}:
        raise common.SetupError(
            f"Android native build manifest has the wrong helper profile set: {helper_profiles!r}"
        )
    helper = helper_profiles["libmain.so"]
    command = helper.get("command") if isinstance(helper, dict) else None
    if not isinstance(command, list) or not all(isinstance(arg, str) for arg in command):
        raise common.SetupError("Android libmain.so profile has no complete compile command")
    encoded_command = json.dumps(command, separators=(",", ":")).encode("utf-8")
    optimization_flags = [arg for arg in command
                          if re.fullmatch(r"-O(?:0|1|2|3|g|s|z|fast)", arg)]
    target_indices = [index for index, arg in enumerate(command) if arg == "-target"]
    output_indices = [index for index, arg in enumerate(command) if arg == "-o"]
    source_names = [arg.replace("\\", "/").rsplit("/", 1)[-1] for arg in command]
    helper_problem = (
        helper.get("required_flags") != expected_helper_flags
        or helper.get("command_sha256") != hashlib.sha256(encoded_command).hexdigest()
        or optimization_flags != [expected_optimization]
        or any(command.count(flag) != 1 for flag in expected_helper_flags[1:])
        or len(target_indices) != 1
        or target_indices[0] + 1 >= len(command)
        or command[target_indices[0] + 1] != _ANDROID_NATIVE_TARGET
        or command.count("-shared") != 1
        or command.count("-fPIC") != 1
        or len(output_indices) != 1
        or output_indices[0] + 1 >= len(command)
        or command[output_indices[0] + 1].replace("\\", "/").rsplit("/", 1)[-1] != "libmain.so"
        or source_names.count("mp6shell.c") != 1
    )
    if helper_problem:
        raise common.SetupError(
            f"Android libmain.so compile profile does not prove the {variant} native mode"
        )

    expected_names = {"libmp6game.so", "libmain.so"}
    unstripped_dir = os.path.dirname(manifest_path)
    staged_dir = os.path.join(
        os.fspath(native_root), "platforms", "android", "app", "src", "main",
        "jniLibs", "arm64-v8a",
    )

    def identities(root):
        try:
            names = {name for name in os.listdir(root)
                     if os.path.isfile(os.path.join(root, name)) and name.endswith(".so")}
        except OSError as exc:
            raise common.SetupError(f"cannot inspect Android native directory {root}: {exc}") from exc
        if names != expected_names:
            raise common.SetupError(
                f"Android native library set mismatch in {root}: expected {sorted(expected_names)}, "
                f"found {sorted(names)}"
            )
        return {
            name: {"bytes": os.path.getsize(os.path.join(root, name)),
                   "sha256": _sha256_file(os.path.join(root, name))}
            for name in sorted(names)
        }

    actual_artifacts = identities(unstripped_dir)
    actual_staged = identities(staged_dir)
    if manifest.get("artifacts") != actual_artifacts or manifest.get("staged") != actual_staged:
        raise common.SetupError(
            "Android staged native bytes do not match the authenticated native build manifest",
            hint=f"rebuild native libraries with --configuration {variant} --clean",
        )
    common.ok(
        f"verified Android {variant} native provenance: {compile_profile['units']} "
        f"game/platform TUs + libmain.so at {expected_optimization}, "
        "2 exact staged libraries"
    )
    return True


def verify_android_version_files(native_root):
    version_path = os.path.join(os.fspath(native_root), "VERSION")
    code_path = os.path.join(os.fspath(native_root), "VERSION_CODE")
    try:
        with open(version_path, "r", encoding="utf-8") as f:
            version = f.read().strip()
        with open(code_path, "r", encoding="ascii") as f:
            code_text = f.read().strip()
    except OSError as exc:
        raise common.SetupError(f"cannot read Android release version metadata: {exc}") from exc
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?", version):
        raise common.SetupError(f"invalid semantic version in {version_path}: {version!r}")
    if not re.fullmatch(r"[1-9]\d*", code_text):
        raise common.SetupError(f"invalid positive Android version code in {code_path}: {code_text!r}")
    code = int(code_text)
    if code > 2_100_000_000:
        raise common.SetupError(f"Android version code exceeds PackageManager limit: {code}")
    common.ok(f"verified explicit Android version metadata: {version} / code {code}")
    return version, code


def verify_apk_version_metadata(native_root, sdk_root, apk, version, code,
                                badging_output=None, git_hash=None):
    """Read the built binary manifest with the pinned SDK's aapt tool."""
    if git_hash is None:
        try:
            proc = subprocess.run(
                ["git", "-C", os.fspath(native_root), "rev-parse", "--short=7", "HEAD"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15,
            )
            candidate = proc.stdout.strip().lower()
            git_hash = candidate if proc.returncode == 0 and re.fullmatch(r"[0-9a-f]{7}", candidate) else "nogit"
        except (OSError, subprocess.TimeoutExpired):
            git_hash = "nogit"
    if badging_output is None:
        aapt = os.path.join(
            os.fspath(sdk_root), "build-tools", _ANDROID_BUILD_TOOLS_VERSION,
            "aapt.exe" if common.IS_WINDOWS else "aapt",
        )
        try:
            proc = subprocess.run(
                [aapt, "dump", "badging", os.fspath(apk)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise common.SetupError(f"cannot inspect built APK metadata with pinned aapt: {exc}") from exc
        if proc.returncode != 0:
            raise common.SetupError(
                f"pinned aapt could not parse built APK {apk}: {(proc.stderr or proc.stdout).strip()}"
            )
        badging_output = proc.stdout
    package_line = next((line for line in badging_output.splitlines()
                         if line.startswith("package: ")), "")
    expected_name = f"{version}-{git_hash}"
    expected_fields = {
        "name": "com.mp6.game",
        "versionCode": str(code),
        "versionName": expected_name,
    }
    actual_fields = {
        key: (match.group(1) if (match := re.search(rf"\b{key}='([^']*)'", package_line)) else None)
        for key in expected_fields
    }
    if actual_fields != expected_fields:
        raise common.SetupError(
            f"built APK version metadata mismatch: expected {expected_fields!r}, "
            f"found {actual_fields!r} in {package_line!r}"
        )
    common.ok(f"verified APK metadata: versionCode={code}, versionName={expected_name}")
    return True


def inspect_apk(native_root, apk):
    """Validate the exact native/resource manifest Gradle packaged."""
    expected_libs = {"lib/arm64-v8a/libmain.so", "lib/arm64-v8a/libmp6game.so"}
    staged_lib_root = os.path.join(
        native_root, "platforms", "android", "app", "src", "main", "jniLibs", "arm64-v8a"
    )
    res_root = os.path.join(native_root, "res")
    expected_asset_paths = {
        "assets/res/" + os.path.relpath(os.path.join(root, name), res_root).replace("\\", "/"):
            os.path.join(root, name)
        for root, _dirs, files in os.walk(res_root)
        for name in files if name != ".DS_Store"
    }
    expected_assets = set(expected_asset_paths)

    def zip_identity(zf, name):
        digest = hashlib.sha256()
        with zf.open(name, "r") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"bytes": zf.getinfo(name).file_size, "sha256": digest.hexdigest()}

    def file_identity(path):
        try:
            return {"bytes": os.path.getsize(path), "sha256": _sha256_file(path)}
        except OSError as exc:
            raise common.SetupError(f"cannot authenticate APK source input {path}: {exc}") from exc

    try:
        with zipfile.ZipFile(apk) as zf:
            infos = zf.infolist()
            names = [info.filename for info in infos]
            if len(names) != len(set(names)):
                raise common.SetupError(f"APK contains duplicate ZIP entry names: {apk}")
            actual_libs = {name for name in names if name.startswith("lib/") and name.endswith(".so")}
            if actual_libs != expected_libs:
                raise common.SetupError(
                    f"APK native manifest mismatch: expected {sorted(expected_libs)}, "
                    f"found {sorted(actual_libs)}"
                )
            by_name = {info.filename: info for info in infos}
            compressed_libs = [name for name in expected_libs
                               if by_name[name].compress_type != zipfile.ZIP_STORED]
            if compressed_libs:
                raise common.SetupError(
                    f"APK native libraries must be stored uncompressed for mmap loading: {compressed_libs}"
                )
            for entry in sorted(expected_libs):
                staged = os.path.join(staged_lib_root, entry.rsplit("/", 1)[-1])
                if zip_identity(zf, entry) != file_identity(staged):
                    raise common.SetupError(
                        f"APK native library bytes differ from the authenticated staged file: {entry}"
                    )
            actual_assets = {name for name in names if name.startswith("assets/res/") and not name.endswith("/")}
            if actual_assets != expected_assets:
                missing = sorted(expected_assets - actual_assets)
                extra = sorted(actual_assets - expected_assets)
                raise common.SetupError(
                    f"APK launcher-resource manifest mismatch (missing={missing}, extra={extra})"
                )
            for entry, source in sorted(expected_asset_paths.items()):
                if zip_identity(zf, entry) != file_identity(source):
                    raise common.SetupError(
                        f"APK launcher-resource bytes differ from the source tree: {entry}"
                    )
            bad = zf.testzip()
            if bad:
                raise common.SetupError(f"APK ZIP CRC validation failed at {bad}")
    except (OSError, zipfile.BadZipFile) as exc:
        raise common.SetupError(f"invalid/unreadable APK {apk}: {exc}") from exc
    common.ok(f"APK manifest verified: 2 native libraries + {len(expected_assets)} launcher assets")
    return True


def build_apk(native_root, variant="debug"):
    android_dir = os.path.join(native_root, "platforms", "android")
    variant = variant.lower()
    if variant not in _ANDROID_NATIVE_OPTIMIZATION:
        raise common.SetupError(f"unsupported Android variant {variant!r}")
    version, version_code = verify_android_version_files(native_root)
    verify_android_native_build_manifest(native_root, variant)
    verify_gradle_wrapper(native_root)
    verify_gradle_dependency_metadata(native_root)
    gradlew = os.path.join(android_dir, "gradlew.bat" if common.IS_WINDOWS else "gradlew")
    if not os.path.exists(gradlew):
        raise common.SetupError(f"{gradlew} not found")
    sdk_root = _find_sdk_root()
    if sdk_root:
        verify_android_sdk(sdk_root)
        _ensure_local_properties(sdk_root)  # defensive: correct even if called before run_android_lane()
    else:
        raise common.SetupError("Android SDK is unavailable; cannot authenticate or build the APK")
    task = "assemble" + variant.capitalize()
    tasks = ["clean", task] + (["lintRelease"] if variant == "release" else [])
    output_dir = os.path.join(android_dir, "app", "build", "outputs", "apk", variant)
    # Do not let an old APK satisfy a successful-but-nonproducing Gradle run.
    if os.path.isdir(output_dir):
        shutil.rmtree(output_dir)
    started_ns = time.time_ns()
    common.info(f"running gradle {' + '.join(tasks)} (this can take a while the first time -- gradle "
                "downloads its own wrapper distribution + dependencies)")
    if not common.IS_WINDOWS:
        common.run(["chmod", "+x", gradlew], check=False)
    gradle_env = _controlled_gradle_environment(native_root)
    common.run([
        gradlew, *tasks, "--dependency-verification=strict", "--no-daemon",
        "--no-build-cache", "--no-configuration-cache", "--refresh-dependencies",
    ], cwd=android_dir, env=gradle_env)
    metadata_path = os.path.join(output_dir, "output-metadata.json")
    try:
        with open(metadata_path, "r", encoding="utf-8") as f:
            metadata = json.load(f)
    except (OSError, ValueError) as exc:
        raise common.SetupError(
            f"gradle reported success but produced no valid {metadata_path}: {exc}"
        ) from exc
    output_files = [element.get("outputFile") for element in metadata.get("elements", [])
                    if isinstance(element, dict) and element.get("outputFile")]
    if len(output_files) != 1:
        raise common.SetupError(
            f"expected exactly one {variant} APK in Gradle metadata, found {output_files}"
        )
    apk = os.path.join(output_dir, output_files[0])
    actual_apks = {
        name for name in os.listdir(output_dir)
        if name.lower().endswith(".apk") and os.path.isfile(os.path.join(output_dir, name))
    }
    if actual_apks != {output_files[0]} or not os.path.isfile(apk):
        raise common.SetupError(
            f"Gradle {variant} output is ambiguous/stale: metadata={output_files}, "
            f"directory={sorted(actual_apks)}"
        )
    # FAT/exFAT and a few archive-backed workspaces have coarse mtimes, so
    # allow two seconds while still excluding any file that predated cleanup.
    if os.stat(apk).st_mtime_ns + 2_000_000_000 < started_ns:
        raise common.SetupError(f"Gradle selected an APK older than this build invocation: {apk}")
    inspect_apk(native_root, apk)
    verify_apk_version_metadata(native_root, sdk_root, apk, version, version_code)
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
