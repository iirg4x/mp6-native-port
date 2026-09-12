"""Focused regressions for setup/build integrity and transactional staging."""
import contextlib
import hashlib
import io
import json
import os
import pathlib
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(NATIVE_ROOT, "tools")
if NATIVE_ROOT not in sys.path:
    sys.path.insert(0, NATIVE_ROOT)
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import build  # noqa: E402
import fetch_nod  # noqa: E402
import release_android  # noqa: E402
from setup.lib import common, nod_ffi, step_android, step_aurora, step_build, step_decomp, step_toolchain  # noqa: E402


class PortLocalAssetPathTests(unittest.TestCase):
    def _paths(self, overrides):
        env = dict(os.environ)
        env.pop("MP6_DISC_ROOT", None)
        env.pop("MP6_DECOMP_INC_DATA", None)
        env.update(overrides)
        script = (
            "import json; from tools import build; "
            "print(json.dumps([build.DECOMP, build.DECOMP_INC_DATA, "
            "build.MP6_DVD_FILES_ROOT, build.MP6_DVD_FST_PATH, "
            "build.COMMON_FLAGS, build.android_common_flags()]))"
        )
        # Import from a different cwd to prove overrides are Port-relative.
        env["PYTHONPATH"] = NATIVE_ROOT
        return json.loads(subprocess.check_output(
            [sys.executable, "-c", script], cwd=TOOLS, env=env, text=True,
        ))

    def test_explicit_port_local_asset_paths(self):
        decomp, includes, files, fst, windows_flags, android_flags = self._paths({
            "MP6_DISC_ROOT": "build/disc cache/orig/GP6E01",
            "MP6_DECOMP_INC_DATA": "build/disc cache/split/include",
        })
        local = pathlib.Path(NATIVE_ROOT, "build", "disc cache")
        self.assertEqual(includes, str(local / "split" / "include"))
        self.assertEqual(files, (local / "orig" / "GP6E01" / "files").as_posix())
        self.assertEqual(fst, (local / "orig" / "GP6E01" / "sys" / "fst.bin").as_posix())
        self.assertIn(includes, windows_flags)
        self.assertIn(includes, android_flags)
        self.assertIn("-DMP6_DVD_ROOT_EXPLICIT=1", windows_flags)
        self.assertNotIn("-DMP6_DVD_ROOT_EXPLICIT=1", android_flags)
        self.assertEqual(decomp, common.DECOMP_DIR)

    def test_default_asset_paths_are_unchanged(self):
        decomp, includes, files, fst, windows_flags, _ = self._paths({})
        self.assertEqual(includes, str(pathlib.Path(decomp, "build", "GP6E01", "include")))
        self.assertEqual(files, pathlib.Path(decomp, "orig", "GP6E01", "files").as_posix())
        self.assertEqual(fst, pathlib.Path(decomp, "orig", "GP6E01", "sys", "fst.bin").as_posix())
        self.assertIn("-DMP6_DVD_ROOT_EXPLICIT=0", windows_flags)


class AuroraArtifactIntegrityTests(unittest.TestCase):
    def test_zig_link_uses_real_system_com_support(self):
        items = build._resolve_aurora_link_items()
        self.assertNotIn("-lcomsuppw", items)
        self.assertIn("-loleaut32", items)
        self.assertIn("-lole32", items)
        self.assertIn("-lwbemuuid", items)

    def test_checkout_commit_mismatch_is_rejected(self):
        results = [
            subprocess.CompletedProcess([], 0, stdout="a" * 40 + "\n", stderr=""),
            subprocess.CompletedProcess([], 0, stdout="b" * 40 + "\n", stderr=""),
        ]
        with mock.patch.object(step_aurora.subprocess, "run", side_effect=results):
            with self.assertRaisesRegex(RuntimeError, "checkout is at"):
                step_aurora.verified_checkout_commit("pinned-ref")

    def test_archive_mutation_with_restored_mtime_breaks_stamp_manifest(self):
        with tempfile.TemporaryDirectory() as root:
            archive = pathlib.Path(root, "libaurora_gx.a")
            archive.write_bytes(b"archive-A")
            recorded = step_aurora._artifact_manifest([archive])
            self.assertIsNone(step_aurora._artifact_profile_problem([archive], recorded))
            old = os.stat(archive)
            archive.write_bytes(b"archive-B")  # same length
            os.utime(archive, ns=(old.st_atime_ns, old.st_mtime_ns))
            problem = step_aurora._artifact_profile_problem([archive], recorded)
            self.assertIn("identity mismatch", problem)

    def test_windows_cmake_tool_paths_are_normalized_and_explicit(self):
        commands = []
        with (
            mock.patch.object(step_aurora.common, "AURORA_DIR", r"C:\work with spaces\aurora"),
            mock.patch.object(step_aurora.common, "NATIVE_ROOT", r"C:\work with spaces\port"),
            mock.patch.object(
                step_aurora.common,
                "run",
                side_effect=lambda command, **_kwargs: commands.append(command),
            ),
        ):
            step_aurora._configure_and_build_tree(
                r"C:\Program Files\CMake\bin\cmake.exe",
                r"C:\work with spaces\aurora\build",
                r"C:\work with spaces\tools\zig-cc-wrappers",
                False,
                mock.Mock(),
                [],
            )

        configure, build_command = commands
        self.assertEqual(configure[0], "C:/Program Files/CMake/bin/cmake.exe")
        self.assertIn(
            "-DCMAKE_RC_COMPILER=C:/work with spaces/tools/zig-cc-wrappers/zigrc.bat",
            configure,
        )
        self.assertIn(
            "-DCMAKE_AR=C:/work with spaces/tools/zig-cc-wrappers/zigar.bat",
            configure,
        )
        self.assertIn(
            "-DCMAKE_RANLIB=C:/work with spaces/tools/zig-cc-wrappers/zigranlib.bat",
            configure,
        )
        self.assertFalse(any("\\" in arg for arg in configure))
        flags = dict(arg[2:].split("=", 1) for arg in configure if arg.startswith("-D"))
        self.assertEqual(
            shlex.split(flags["CMAKE_C_FLAGS"]),
            ["-include", "C:/work with spaces/port/include/mp6_host_section.h"],
        )
        self.assertEqual(flags["CMAKE_C_FLAGS"], flags["CMAKE_CXX_FLAGS"])
        self.assertEqual(
            shlex.split(flags["CMAKE_EXE_LINKER_FLAGS"]),
            ["-LC:/work with spaces/tools/zig-cc-wrappers/stub-libs"],
        )
        self.assertEqual(
            build_command[:3],
            [
                "C:/Program Files/CMake/bin/cmake.exe",
                "--build",
                "C:/work with spaces/aurora/build",
            ],
        )

    def test_zig_archive_tool_wrappers_are_generated(self):
        with tempfile.TemporaryDirectory() as root:
            with mock.patch.object(step_aurora.common, "TOOLCHAIN_DIR", root):
                wrappers = pathlib.Path(
                    step_aurora._write_wrapper_scripts(r"C:\zig tools\zig.exe")
                )
            self.assertIn(
                r'"C:\zig tools\zig.exe" ar %*',
                (wrappers / "zigar.bat").read_text(encoding="utf-8"),
            )
            self.assertIn(
                r'"C:\zig tools\zig.exe" ranlib %*',
                (wrappers / "zigranlib.bat").read_text(encoding="utf-8"),
            )

    def test_manual_aurora_recipe_quotes_paths_with_spaces(self):
        output = io.StringIO()
        with (
            mock.patch.object(
                step_aurora.common,
                "AURORA_DIR",
                r"C:\workspace with spaces\aurora",
            ),
            mock.patch.object(
                step_aurora.common,
                "TOOLCHAIN_DIR",
                r"C:\workspace with spaces\toolchain",
            ),
            mock.patch.object(
                step_aurora.common,
                "NATIVE_ROOT",
                r"C:\workspace with spaces\port",
            ),
            mock.patch.object(step_aurora, "read_aurora_pin", return_value="abc123"),
            contextlib.redirect_stdout(output),
        ):
            step_aurora._print_manual_recipe(["test problem"])
            expected = step_aurora._configure_args(
                "cmake",
                "C:/workspace with spaces/aurora/build",
                "C:/workspace with spaces/toolchain/zig-cc-wrappers",
                False,
            )

        recipe = output.getvalue()
        self.assertIn('-S "C:/workspace with spaces/aurora"', recipe)
        self.assertIn(
            '"-DCMAKE_C_COMPILER=C:/workspace with spaces/toolchain'
            '/zig-cc-wrappers/zigcc.bat"',
            recipe,
        )
        self.assertIn(
            subprocess.list2cmdline([next(arg for arg in expected if arg.startswith("-DCMAKE_C_FLAGS="))]),
            recipe,
        )
        if os.name == "nt":
            # Exercise cmd.exe and the real Windows argv parser, not only the
            # printed spelling: the nested quotes must reach CMake intact.
            printed = recipe.split("       cmake -S", 1)[1].split("\n       cmake --build", 1)[0]
            printed = "-S" + printed.replace(" ^\n         ", " ")
            probe = subprocess.list2cmdline([
                sys.executable, "-c", "import json, sys; print(json.dumps(sys.argv[1:]))",
            ]) + " " + printed
            result = subprocess.run(probe, shell=True, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(result.stdout), expected[1:])


class AndroidNativeBuildProfileTests(unittest.TestCase):
    def test_compile_records_require_exact_requested_optimization(self):
        with tempfile.TemporaryDirectory() as root:
            obj = pathlib.Path(root, "game.o")
            obj.write_bytes(b"object")
            record = {
                "contract": build._COMPILE_CONTRACT_VERSION,
                "command": ["clang", "-O2", "-fno-strict-aliasing",
                            "-c", "game.c", "-o", str(obj)],
            }
            pathlib.Path(str(obj) + ".cmd.json").write_text(
                json.dumps(record), encoding="utf-8"
            )
            units = [("game.c", [], "game.o", "common")]
            with mock.patch.object(build, "OBJ_DIR", root):
                proof = build._verified_android_compile_profile(
                    units, "-O2", ("-fno-strict-aliasing",)
                )
                self.assertEqual(proof["units"], 1)
                record["command"].insert(2, "-O0")
                pathlib.Path(str(obj) + ".cmd.json").write_text(
                    json.dumps(record), encoding="utf-8"
                )
                with self.assertRaisesRegex(RuntimeError, "profile mismatch"):
                    build._verified_android_compile_profile(
                        units, "-O2", ("-fno-strict-aliasing",)
                    )

    def test_standalone_helper_requires_the_requested_optimization(self):
        command = ["clang", "-target", build.ANDROID_TRIPLE, "-O0",
                   "-shared", "mp6shell.c", "-o", "libmain.so"]
        proof = build._verified_android_helper_profile(
            "libmain.so", command, "-O0"
        )
        self.assertEqual(proof["required_flags"], ["-O0"])
        self.assertEqual(proof["command"], command)
        with self.assertRaisesRegex(RuntimeError, "helper profile mismatch"):
            build._verified_android_helper_profile(
                "libmain.so", command, "-O2", ("-fno-strict-aliasing",)
            )

    def test_gradle_variant_is_bound_to_staged_native_bytes_and_profile(self):
        with tempfile.TemporaryDirectory() as root:
            native_dir = pathlib.Path(root, "build", "android", "aurora")
            staged_dir = pathlib.Path(
                root, "packaging", "android", "app", "src", "main", "jniLibs", "arm64-v8a"
            )
            native_dir.mkdir(parents=True)
            staged_dir.mkdir(parents=True)
            for directory in (native_dir, staged_dir):
                (directory / "libmp6game.so").write_bytes(b"game")
                (directory / "libmain.so").write_bytes(b"shell")

            def identities(directory):
                return {
                    name: {
                        "bytes": (directory / name).stat().st_size,
                        "sha256": hashlib.sha256((directory / name).read_bytes()).hexdigest(),
                    }
                    for name in ("libmain.so", "libmp6game.so")
                }

            helper_command = [
                "clang", "-target", step_android._ANDROID_NATIVE_TARGET, "-O0",
                "-shared", "-fPIC", "mp6shell.c", "-o", "libmain.so",
            ]
            manifest = {
                "manifest_version": step_android._ANDROID_NATIVE_MANIFEST_VERSION,
                "target": step_android._ANDROID_NATIVE_TARGET,
                "ndk_version": step_android._ANDROID_NDK_VERSION,
                "configuration": "debug",
                "game_optimization": "-O0",
                "game_compile_flags": ["-O0"],
                "windowed": True,
                "compile_profile": {"units": 1, "required_flags": ["-O0"],
                                    "command_records_sha256": "a" * 64},
                "helper_profiles": {
                    "libmain.so": {
                        "required_flags": ["-O0"],
                        "command": helper_command,
                        "command_sha256": hashlib.sha256(json.dumps(
                            helper_command, separators=(",", ":")
                        ).encode("utf-8")).hexdigest(),
                    },
                },
                "link_command_sha256": "b" * 64,
                "artifacts": identities(native_dir),
                "staged": identities(staged_dir),
            }
            (native_dir / "native-build-manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            self.assertTrue(step_android.verify_android_native_build_manifest(root, "debug"))
            with self.assertRaisesRegex(common.SetupError, "does not match Gradle release"):
                step_android.verify_android_native_build_manifest(root, "release")
            helper_command[3] = "-O2"
            manifest["helper_profiles"]["libmain.so"]["command_sha256"] = hashlib.sha256(
                json.dumps(helper_command, separators=(",", ":")).encode("utf-8")
            ).hexdigest()
            (native_dir / "native-build-manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            with self.assertRaisesRegex(common.SetupError, "libmain.so compile profile"):
                step_android.verify_android_native_build_manifest(root, "debug")
            helper_command[3] = "-O0"
            manifest["helper_profiles"]["libmain.so"]["command_sha256"] = hashlib.sha256(
                json.dumps(helper_command, separators=(",", ":")).encode("utf-8")
            ).hexdigest()
            (native_dir / "native-build-manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            (staged_dir / "libmp6game.so").write_bytes(b"GAME")
            with self.assertRaisesRegex(common.SetupError, "do not match"):
                step_android.verify_android_native_build_manifest(root, "debug")


class AndroidVersionMetadataTests(unittest.TestCase):
    def test_explicit_version_code_is_git_history_independent_and_bounded(self):
        gradle = pathlib.Path(
            NATIVE_ROOT, "packaging", "android", "app", "build.gradle"
        ).read_text(encoding="utf-8")
        self.assertNotIn("['git', 'rev-list'", gradle)
        self.assertIn("VERSION_CODE", gradle)
        version, code = step_android.verify_android_version_files(NATIVE_ROOT)
        self.assertRegex(version, r"^\d+\.\d+\.\d+")
        self.assertGreater(code, 0)
        self.assertLessEqual(code, 2_100_000_000)
        badging = (
            "package: name='com.mp6.game' versionCode='20000' "
            "versionName='0.2.0-abcdef0'\n"
        )
        self.assertTrue(step_android.verify_apk_version_metadata(
            NATIVE_ROOT, "unused-sdk", "unused.apk", "0.2.0", 20000,
            badging_output=badging, git_hash="abcdef0",
        ))
        with self.assertRaisesRegex(common.SetupError, "metadata mismatch"):
            step_android.verify_apk_version_metadata(
                NATIVE_ROOT, "unused-sdk", "unused.apk", "0.2.0", 20001,
                badging_output=badging, git_hash="abcdef0",
            )

        with tempfile.TemporaryDirectory() as root:
            pathlib.Path(root, "VERSION").write_text("1.2.3\n", encoding="utf-8")
            pathlib.Path(root, "VERSION_CODE").write_text("0\n", encoding="ascii")
            with self.assertRaisesRegex(common.SetupError, "invalid positive"):
                step_android.verify_android_version_files(root)


class GradleWrapperIntegrityTests(unittest.TestCase):
    def _copy_wrapper(self, root):
        source = pathlib.Path(NATIVE_ROOT, "packaging", "android")
        destination = pathlib.Path(root, "packaging", "android")
        relative_paths = [
            "gradle/wrapper/gradle-wrapper.properties",
            *step_android._GRADLE_WRAPPER_FILES,
        ]
        for relative in relative_paths:
            target = destination.joinpath(*relative.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source.joinpath(*relative.split("/")), target)
        return destination

    def test_checked_in_gradle_813_wrapper_matches_all_pins(self):
        self.assertTrue(step_android.verify_gradle_wrapper(NATIVE_ROOT))
        self.assertTrue(step_android.verify_gradle_dependency_metadata(NATIVE_ROOT))

    def test_mutated_wrapper_jar_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            android = self._copy_wrapper(root)
            jar = android / "gradle" / "wrapper" / "gradle-wrapper.jar"
            jar.write_bytes(jar.read_bytes() + b"tampered")
            with self.assertRaisesRegex(common.SetupError, "integrity failure"):
                step_android.verify_gradle_wrapper(root)

    def test_mutated_distribution_checksum_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            android = self._copy_wrapper(root)
            properties = android / "gradle" / "wrapper" / "gradle-wrapper.properties"
            text = properties.read_text(encoding="utf-8")
            properties.write_text(text.replace(
                step_android._GRADLE_WRAPPER_PROPERTIES["distributionSha256Sum"],
                "0" * 64,
            ), encoding="utf-8")
            with self.assertRaisesRegex(common.SetupError, "audited Gradle 8.13 contract"):
                step_android.verify_gradle_wrapper(root)

    def test_dependency_metadata_requires_sha256_for_every_artifact(self):
        with tempfile.TemporaryDirectory() as root:
            source = pathlib.Path(
                NATIVE_ROOT, "packaging", "android", "gradle", "verification-metadata.xml"
            )
            target = pathlib.Path(
                root, "packaging", "android", "gradle", "verification-metadata.xml"
            )
            target.parent.mkdir(parents=True)
            text = source.read_text(encoding="utf-8").replace("<sha256 ", "<sha1 ", 1)
            target.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(common.SetupError, "exactly one SHA-256"):
                step_android.verify_gradle_dependency_metadata(
                    root, expected_hash=step_android._sha256_file(target)
                )

    def test_tampered_cached_gradle_artifact_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as root:
            metadata = pathlib.Path(
                root, "packaging", "android", "gradle", "verification-metadata.xml"
            )
            metadata.parent.mkdir(parents=True)
            cache = pathlib.Path(
                root, "gradle-home", "caches", "modules-2", "files-2.1",
                "com.example", "demo", "1.0", "cache-key", "demo-1.0.jar",
            )
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"dependency-A")
            digest = step_android._sha256_file(cache)
            metadata.write_text(
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<verification-metadata xmlns="https://schema.gradle.org/dependency-verification">'
                '<components><component group="com.example" name="demo" version="1.0">'
                '<artifact name="demo-1.0.jar"><sha256 value="' + digest + '"/>'
                '</artifact></component></components></verification-metadata>',
                encoding="utf-8",
            )
            gradle_home = pathlib.Path(root, "gradle-home")
            self.assertEqual(
                step_android._verify_and_prune_gradle_artifact_cache(root, gradle_home), 1
            )
            old = os.stat(cache)
            cache.write_bytes(b"dependency-B")  # same length
            os.utime(cache, ns=(old.st_atime_ns, old.st_mtime_ns))
            with self.assertRaisesRegex(common.SetupError, "cache integrity failure"):
                step_android._verify_and_prune_gradle_artifact_cache(root, gradle_home)


class AndroidSdkIntegrityTests(unittest.TestCase):
    def test_packaging_tool_content_mutation_with_restored_mtime_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            platform_root = pathlib.Path(
                root, "platforms", step_android._ANDROID_COMPILE_SDK
            )
            build_tools_root = pathlib.Path(
                root, "build-tools", step_android._ANDROID_BUILD_TOOLS_VERSION
            )
            platform_root.mkdir(parents=True)
            build_tools_root.mkdir(parents=True)
            android_jar = platform_root / "android.jar"
            aapt2 = build_tools_root / "aapt2.exe"
            android_jar.write_bytes(b"android-api")
            aapt2.write_bytes(b"packager-A")
            platform_files = {
                "android.jar": {
                    "bytes": android_jar.stat().st_size,
                    "sha256": step_android._sha256_file(android_jar),
                },
            }
            build_tools_tree = step_android._bounded_tree_fingerprint(
                build_tools_root, "fixture Build Tools"
            )
            self.assertTrue(step_android.verify_android_sdk(
                root, platform_files=platform_files, build_tools_tree=build_tools_tree
            ))
            old = os.stat(aapt2)
            aapt2.write_bytes(b"packager-B")  # same length
            os.utime(aapt2, ns=(old.st_atime_ns, old.st_mtime_ns))
            with self.assertRaisesRegex(common.SetupError, "Build Tools.*integrity failure"):
                step_android.verify_android_sdk(
                    root, platform_files=platform_files, build_tools_tree=build_tools_tree
                )

    def test_jdk_tree_mutation_with_restored_mtime_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            java = pathlib.Path(root, "bin", "java.exe" if os.name == "nt" else "java")
            java.parent.mkdir()
            java.write_bytes(b"java-runtime-A")
            expected_tree = step_android._bounded_tree_fingerprint(root, "fixture JDK")
            with mock.patch.object(
                    step_android, "_java_version_output",
                    return_value=step_android._ANDROID_JDK_VERSION_OUTPUT):
                self.assertEqual(step_android.verify_android_jdk(
                    root, expected_tree=expected_tree
                ), os.path.realpath(root))
                old = os.stat(java)
                java.write_bytes(b"java-runtime-B")  # same length
                os.utime(java, ns=(old.st_atime_ns, old.st_mtime_ns))
                with self.assertRaisesRegex(common.SetupError, "JDK integrity failure"):
                    step_android.verify_android_jdk(root, expected_tree=expected_tree)

    def test_gradle_environment_scrubs_ambient_overrides_and_refuses_init_scripts(self):
        with tempfile.TemporaryDirectory() as root, \
                mock.patch.object(step_android, "verify_android_jdk", return_value=root), \
                mock.patch.dict(os.environ, {
                    "GRADLE_OPTS": "-I attacker.gradle",
                    "JAVA_TOOL_OPTIONS": "-javaagent:attacker.jar",
                    "ORG_GRADLE_PROJECT_injected": "yes",
                    "GRADLE_USER_HOME": "attacker-home",
                }):
            env = step_android._controlled_gradle_environment(root)
            self.assertNotIn("GRADLE_OPTS", env)
            self.assertNotIn("JAVA_TOOL_OPTIONS", env)
            self.assertNotIn("ORG_GRADLE_PROJECT_injected", env)
            self.assertEqual(env["JAVA_HOME"], root)
            self.assertEqual(env["GRADLE_USER_HOME"], os.path.join(root, "build", "gradle-user-home"))
            init = pathlib.Path(env["GRADLE_USER_HOME"], "init.gradle")
            init.write_text("throw new Error('injected')\n", encoding="utf-8")
            with self.assertRaisesRegex(common.SetupError, "forbidden init/property"):
                step_android._controlled_gradle_environment(root)


class AndroidNdkSelectionTests(unittest.TestCase):
    def test_only_exact_r27d_path_and_source_revision_are_accepted(self):
        with tempfile.TemporaryDirectory() as root:
            exact = pathlib.Path(root, fetch_nod.ANDROID_NDK_VERSION)
            exact.mkdir()
            (exact / "source.properties").write_text(
                f"Pkg.Desc = Android NDK\nPkg.Revision = {fetch_nod.ANDROID_NDK_VERSION}\n",
                encoding="utf-8",
            )
            with mock.patch.dict(os.environ, {"ANDROID_NDK_ROOT": str(exact)}):
                self.assertEqual(build._find_ndk_root(), str(exact.resolve()))
                self.assertEqual(fetch_nod._find_ndk_root(), str(exact.resolve()))
                self.assertEqual(step_android._find_ndk(), str(exact.resolve()))

            newer = pathlib.Path(root, "99.0.0")
            newer.mkdir()
            (newer / "source.properties").write_text(
                "Pkg.Revision = 99.0.0\n", encoding="utf-8"
            )
            with mock.patch.dict(os.environ, {"ANDROID_NDK_ROOT": str(newer)}):
                self.assertIsNone(build._find_ndk_root())
                self.assertIsNone(fetch_nod._find_ndk_root())
                self.assertIsNone(step_android._find_ndk())

            (exact / "source.properties").write_text(
                "Pkg.Revision = 27.2.0\n", encoding="utf-8"
            )
            with mock.patch.dict(os.environ, {"ANDROID_NDK_ROOT": str(exact)}):
                self.assertIsNone(build._find_ndk_root())
                self.assertIsNone(fetch_nod._find_ndk_root())
                self.assertIsNone(step_android._find_ndk())


class NodAndroidToolchainIdentityTests(unittest.TestCase):
    def test_cargo_profile_env_and_parent_config_cannot_enter_build(self):
        ambient = {
            "CARGO_PROFILE_RELEASE_OPT_LEVEL": "0",
            "CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS": "--cfg injected",
            "CRATE_CC_NO_DEFAULTS": "1",
            "HOST_CC": "attacker-cc",
            "PKG_CONFIG": "attacker-pkg-config",
            "RUSTC_BOOTSTRAP": "1",
            "RUSTFLAGS": "--cfg injected",
            "VCPKG_ROOT": "attacker-vcpkg",
            "ZSTD_SYS_USE_PKG_CONFIG": "1",
        }
        with mock.patch.dict(os.environ, ambient):
            env = fetch_nod._rust_environment(fetch_nod.ANDROID_RUST_TOOLCHAIN)
        for name in ambient:
            self.assertNotIn(name, env)
        self.assertEqual(env["RUSTUP_TOOLCHAIN"], fetch_nod.ANDROID_RUST_TOOLCHAIN)

        with tempfile.TemporaryDirectory() as root:
            parent = pathlib.Path(root, "attacker")
            source = parent / "source"
            cargo_home = pathlib.Path(root, "controlled-cargo-home")
            (parent / ".cargo").mkdir(parents=True)
            (parent / ".cargo" / "config.toml").write_text(
                "[build]\nrustflags=['--cfg','injected']\n", encoding="utf-8"
            )
            source.mkdir()
            (source / "Cargo.toml").write_text("[workspace]\n", encoding="utf-8")
            cargo_home.mkdir()
            command, cwd = fetch_nod._cargo_build_invocation(
                {"cargo": "cargo-fixture"}, str(source), str(cargo_home)
            )
            self.assertEqual(os.path.realpath(cwd), os.path.realpath(cargo_home / "work"))
            self.assertFalse(os.path.commonpath((os.path.realpath(parent), os.path.realpath(cwd)))
                             == os.path.realpath(parent))
            self.assertIn("--manifest-path", command)
            self.assertEqual(
                command[command.index("--manifest-path") + 1],
                os.path.realpath(source / "Cargo.toml"),
            )

    def test_rustc_host_and_target_stdlib_content_mutations_change_identity(self):
        with tempfile.TemporaryDirectory() as root:
            rust_root = pathlib.Path(root, "rust")
            proxy_bin = rust_root / "cargo" / "bin"
            sysroot = rust_root / "rustup" / "toolchains" / fetch_nod.ANDROID_RUST_TOOLCHAIN
            compiler_bin = sysroot / "bin"
            target_libdir = sysroot / "lib" / "rustlib" / fetch_nod.ANDROID_RUST_TARGET / "lib"
            host_libdir = sysroot / "lib" / "rustlib" / fetch_nod.ANDROID_RUST_HOST / "lib"
            proxy_bin.mkdir(parents=True)
            compiler_bin.mkdir(parents=True)
            target_libdir.mkdir(parents=True)
            host_libdir.mkdir(parents=True)
            suffix = ".exe" if os.name == "nt" else ""
            tools = {}
            for name in ("cargo", "rustc", "rustup", "target_cc", "clang", "ar", "ranlib"):
                path = proxy_bin / (name + suffix)
                path.write_bytes((name + "-proxy").encode("ascii"))
                tools[name] = str(path)
            actual_rustc = compiler_bin / ("rustc" + suffix)
            actual_cargo = compiler_bin / ("cargo" + suffix)
            stdlib = target_libdir / "libstd-fixture.rlib"
            host_stdlib = host_libdir / "libstd-host-fixture.rlib"
            actual_rustc.write_bytes(b"compiler-A")
            actual_cargo.write_bytes(b"cargo-tool")
            stdlib.write_bytes(b"stdlib-A")
            host_stdlib.write_bytes(b"hostlib-A")

            def capture(command, _env):
                arguments = command[1:]
                if arguments == ["show", "active-toolchain"]:
                    return fetch_nod.ANDROID_RUST_TOOLCHAIN + " (environment override)"
                if arguments == ["-Vv"]:
                    return (f"rustc {fetch_nod.ANDROID_RUST_RELEASE} fixture\n"
                            f"commit-hash: {fetch_nod.ANDROID_RUST_COMMIT}\n"
                            f"host: {fetch_nod.ANDROID_RUST_HOST}\n"
                            f"release: {fetch_nod.ANDROID_RUST_RELEASE}") if command[0] == tools["rustc"] \
                        else "cargo 1.0.0 fixture\nrelease: 1.0.0"
                if arguments == ["--print", "sysroot"]:
                    return str(sysroot)
                if arguments == ["--print", "target-libdir", "--target", fetch_nod.ANDROID_RUST_TARGET]:
                    return str(target_libdir)
                if arguments == ["--print", "target-libdir", "--target", fetch_nod.ANDROID_RUST_HOST]:
                    return str(host_libdir)
                raise AssertionError(command)

            with mock.patch.object(fetch_nod, "RUST_ROOT", str(rust_root)), \
                    mock.patch.object(fetch_nod, "_capture_tool_output", side_effect=capture):
                before = fetch_nod._android_toolchain_identity(tools)
                old = os.stat(actual_rustc)
                actual_rustc.write_bytes(b"compiler-B")  # same length
                os.utime(actual_rustc, ns=(old.st_atime_ns, old.st_mtime_ns))
                after_rustc = fetch_nod._android_toolchain_identity(tools)
                self.assertNotEqual(before["rust"]["rustc"]["sha256"],
                                    after_rustc["rust"]["rustc"]["sha256"])
                self.assertNotEqual(before["rust"]["compiler_bin"]["sha256"],
                                    after_rustc["rust"]["compiler_bin"]["sha256"])

                old = os.stat(stdlib)
                stdlib.write_bytes(b"stdlib-B")  # same length
                os.utime(stdlib, ns=(old.st_atime_ns, old.st_mtime_ns))
                after_stdlib = fetch_nod._android_toolchain_identity(tools)
                self.assertNotEqual(after_rustc["rust"]["target_stdlib"]["sha256"],
                                    after_stdlib["rust"]["target_stdlib"]["sha256"])

                old = os.stat(host_stdlib)
                host_stdlib.write_bytes(b"hostlib-B")  # same length
                os.utime(host_stdlib, ns=(old.st_atime_ns, old.st_mtime_ns))
                after_host = fetch_nod._android_toolchain_identity(tools)
                self.assertNotEqual(after_stdlib["rust"]["host_stdlib"]["sha256"],
                                    after_host["rust"]["host_stdlib"]["sha256"])

    def test_unpacked_registry_source_is_ignored_and_archive_mutation_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            nod_dir = pathlib.Path(root, "nod")
            rust_root = pathlib.Path(root, "rust")
            verified = nod_dir / "_cargo-crates"
            unpacked = rust_root / "cargo" / "registry" / "src" / "fixture" / "demo-1.0.0"
            verified.mkdir(parents=True)
            unpacked.mkdir(parents=True)
            archive = verified / "demo-1.0.0.crate"
            source = unpacked / "src.rs"
            archive.write_bytes(b"verified-archive-A")
            source.write_bytes(b"source-A")
            checksum = fetch_nod._sha256_file(archive)
            lock_bytes = (
                'version = 4\n\n[[package]]\nname = "demo"\nversion = "1.0.0"\n'
                'source = "registry+https://github.com/rust-lang/crates.io-index"\n'
                f'checksum = "{checksum}"\n'
            ).encode("utf-8")
            with mock.patch.object(fetch_nod, "NOD_DIR", str(nod_dir)), \
                    mock.patch.object(fetch_nod, "RUST_ROOT", str(rust_root)):
                _records, before = fetch_nod._verified_crate_archives(
                    lock_bytes, allow_download=False
                )
                old = os.stat(source)
                source.write_bytes(b"source-B")  # same length; never a build input
                os.utime(source, ns=(old.st_atime_ns, old.st_mtime_ns))
                _records, after_source = fetch_nod._verified_crate_archives(
                    lock_bytes, allow_download=False
                )
                self.assertEqual(before, after_source)

                old = os.stat(archive)
                archive.write_bytes(b"verified-archive-B")  # same length
                os.utime(archive, ns=(old.st_atime_ns, old.st_mtime_ns))
                with self.assertRaisesRegex(RuntimeError, "crate cache mismatch"):
                    fetch_nod._verified_crate_archives(lock_bytes, allow_download=False)

class ZigInstallTests(unittest.TestCase):
    def test_install_manifest_rejects_mutated_bundled_library(self):
        with tempfile.TemporaryDirectory() as root:
            dirname = "zig-x86_64-windows-0.16.0"
            install = pathlib.Path(root, dirname)
            library = install / "lib" / "compiler_rt.zig"
            library.parent.mkdir(parents=True)
            (install / "zig.exe").write_bytes(b"fake-zig-driver")
            library.write_bytes(b"runtime-A")
            release = {"url": "https://example.invalid/zig.zip", "sha256": "ab" * 32}
            with mock.patch.object(step_toolchain.common, "TOOLCHAIN_DIR", root), \
                    mock.patch.object(step_toolchain.common, "run_capture", return_value=(0, "0.16.0")):
                manifest = {
                    "manifest_version": step_toolchain.ZIG_INSTALL_MANIFEST_VERSION,
                    "version": "0.16.0",
                    "host": "x86_64-windows",
                    "archive_url": release["url"],
                    "archive_sha256": release["sha256"],
                    "zig_exe_sha256": step_toolchain._sha256_file(install / "zig.exe"),
                    "tree": step_toolchain.zig_tree_fingerprint(install),
                }
                step_toolchain._write_json_atomic(
                    step_toolchain._zig_manifest_path(dirname), manifest
                )
                self.assertEqual(
                    step_toolchain._check_zig_install(
                        dirname, "0.16.0", "x86_64-windows", release
                    ),
                    (True, None),
                )
                library.write_bytes(b"runtime-B")
                valid, problem = step_toolchain._check_zig_install(
                    dirname, "0.16.0", "x86_64-windows", release
                )
                self.assertFalse(valid)
                self.assertIn("full-tree", problem)


class BuildHeaderOverrideTests(unittest.TestCase):
    def test_windows_float_shadow_avoids_mingw_include_next_recursion(self):
        if not os.path.isfile(build.ZIG):
            self.skipTest("pinned Zig toolchain is not installed")

        with tempfile.TemporaryDirectory() as root:
            build.patch_msl_override(dst_dir=root)
            wrapper = pathlib.Path(root, "float.h").read_text(encoding="utf-8")
            self.assertIn('#pragma push_macro("__MINGW32__")', wrapper)
            self.assertIn("#undef __MINGW32__", wrapper)
            self.assertIn('/lib/include/float.h"', wrapper.replace("\\", "/"))
            self.assertIn('#pragma pop_macro("__MINGW32__")', wrapper)
            self.assertNotIn("any-windows-any/float.h", wrapper.replace("\\", "/"))

            probe = pathlib.Path(root, "float_probe.c")
            probe.write_text("#include <float.h>\n", encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "cc", "-target", build.TARGET, "-I", root,
                 "-E", "-H", "-dM", str(probe)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("#define FLT_EPSILON __FLT_EPSILON__", result.stdout)
            trace = result.stderr.replace("\\", "/")
            self.assertIn("/lib/include/float.h", trace)
            self.assertNotIn("any-windows-any/float.h", trace)
            self.assertNotIn("include depth", result.stderr.lower())


class ReleaseIntegrityTests(unittest.TestCase):
    def _repo(self):
        temp = tempfile.TemporaryDirectory()
        root = temp.name
        subprocess.run(["git", "init", "-q", root], check=True)
        pathlib.Path(root, "VERSION").write_text("1.2.3\n", encoding="utf-8")
        pathlib.Path(root, "source.c").write_text("int clean;\n", encoding="utf-8")
        pathlib.Path(root, ".gitignore").write_text("*.png\nbuild/\n*.log\n", encoding="utf-8")
        subprocess.run(["git", "-C", root, "add", "VERSION", "source.c", ".gitignore"], check=True)
        subprocess.run([
            "git", "-C", root, "-c", "user.name=MP6 Test", "-c",
            "user.email=mp6-test@example.invalid", "commit", "-qm", "fixture",
        ], check=True)
        return temp, root

    def test_release_checkout_must_match_head_but_allows_generated_outputs(self):
        temp, root = self._repo()
        with temp:
            self.assertIsNone(release_android.release_checkout_problem(root))

            pathlib.Path(root, "build", "android").mkdir(parents=True)
            pathlib.Path(root, "build", "android", "libmain.so").write_bytes(b"generated")
            pathlib.Path(root, "gate.log").write_text("generated\n", encoding="utf-8")
            self.assertIsNone(release_android.release_checkout_problem(root))

            pathlib.Path(root, "source.c").write_text("int dirty;\n", encoding="utf-8")
            problem = release_android.release_checkout_problem(root)
            self.assertIn("differs from HEAD", problem)
            self.assertIn("source.c", problem)

    def test_release_checkout_rejects_untracked_source(self):
        temp, root = self._repo()
        with temp:
            pathlib.Path(root, "new_source.c").write_text("int new_file;\n", encoding="utf-8")
            problem = release_android.release_checkout_problem(root)
            self.assertIn("differs from HEAD", problem)
            self.assertIn("new_source.c", problem)

    def test_release_checkout_rejects_ignored_files_outside_the_generated_allowlist(self):
        temp, root = self._repo()
        with temp:
            pathlib.Path(root, "res").mkdir()
            pathlib.Path(root, "res", "surprise.log").write_bytes(b"unexpected release input")
            problem = release_android.release_checkout_problem(root)
            self.assertIn("differs from HEAD", problem)
            self.assertIn("surprise.log", problem)

    def test_release_driver_requests_o2_native_profile_and_release_apk(self):
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.object(sys, "argv", ["release_android.py"]), \
                mock.patch.object(release_android, "_require_release_checkout", return_value=True), \
                mock.patch.object(release_android.subprocess, "run", return_value=completed) as run_mock, \
                mock.patch.object(release_android.step_android, "build_apk",
                                  return_value="release.apk") as apk_mock:
            self.assertEqual(release_android.main(), 0)
        native_cmd = run_mock.call_args.args[0]
        self.assertIn("--configuration", native_cmd)
        self.assertEqual(native_cmd[native_cmd.index("--configuration") + 1], "release")
        self.assertIn("--clean", native_cmd)
        apk_mock.assert_called_once_with(release_android.NATIVE_ROOT, variant="release")


class IncrementalBuildTests(unittest.TestCase):
    def _seed(self, root):
        compiler = os.path.join(root, "fake compiler.exe")
        source = os.path.join(root, "source.c")
        header_dir = os.path.join(root, "headers with spaces")
        header = os.path.join(header_dir, "shared.h")
        obj = os.path.join(root, "source.o")
        os.makedirs(header_dir)
        for path, body in ((compiler, b"compiler-A"), (source, b'#include "shared.h"\n'),
                           (header, b"value-one"), (obj, b"object")):
            with open(path, "wb") as f:
                f.write(body)
        depfile = obj + ".d"
        escaped_header = header.replace("\\", "/").replace(" ", "\\ ")
        source_dep = source.replace("\\", "/").replace(" ", "\\ ")
        with open(depfile, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{obj}: {source_dep} \\\n  {escaped_header}\n")
        cmd = [compiler, "-MD", "-MF", depfile, "-c", source, "-o", obj]
        record = build._command_record(cmd)
        record["dependencies"] = build._dependency_record(depfile)
        build._write_command_record(obj + ".cmd.json", record)
        return compiler, source, header, obj, cmd

    def test_depfile_parser_handles_spaces_backslashes_and_continuations(self):
        with tempfile.TemporaryDirectory() as root:
            _compiler, source, header, obj, _cmd = self._seed(root)
            self.assertEqual(build._parse_depfile(obj + ".d"), [source.replace("\\", "/"), header.replace("\\", "/")])

    def test_header_content_change_with_restored_mtime_invalidates(self):
        with tempfile.TemporaryDirectory() as root:
            _compiler, _source, header, obj, cmd = self._seed(root)
            self.assertFalse(build.needs_rebuild(_source, obj, cmd))
            old = os.stat(header)
            time.sleep(0.01)
            with open(header, "wb") as f:
                f.write(b"value-two")  # same byte length
            os.utime(header, ns=(old.st_atime_ns, old.st_mtime_ns))
            build._reset_fingerprint_caches()  # next build invocation
            self.assertTrue(build.needs_rebuild(_source, obj, cmd))

    def test_command_and_compiler_content_fingerprints_invalidate(self):
        with tempfile.TemporaryDirectory() as root:
            compiler, source, _header, obj, cmd = self._seed(root)
            self.assertTrue(build.needs_rebuild(source, obj, cmd + ["-DCHANGED=1"]))
            old = os.stat(compiler)
            time.sleep(0.01)
            with open(compiler, "wb") as f:
                f.write(b"compiler-B")
            os.utime(compiler, ns=(old.st_atime_ns, old.st_mtime_ns))
            build._reset_fingerprint_caches()  # next build invocation
            self.assertTrue(build.needs_rebuild(source, obj, cmd))

    def test_non_executable_zig_tree_change_invalidates_command_fingerprint(self):
        with tempfile.TemporaryDirectory() as root:
            zig = os.path.join(root, "zig.exe")
            library = os.path.join(root, "lib", "compiler_rt.zig")
            os.makedirs(os.path.dirname(library))
            pathlib.Path(zig).write_bytes(b"fake-zig-driver")
            pathlib.Path(library).write_bytes(b"runtime-A")
            cmd = [zig, "cc", "-c", "unit.c", "-o", "unit.o"]
            build._reset_fingerprint_caches()
            before = build._command_record(cmd)

            cache_file = pathlib.Path(root, ".zig-cache", "transient.bin")
            cache_file.parent.mkdir()
            cache_file.write_bytes(b"not a toolchain input")
            build._reset_fingerprint_caches()
            after_cache = build._command_record(cmd)
            self.assertEqual(before["compiler"]["toolchain_tree"],
                             after_cache["compiler"]["toolchain_tree"])

            old = os.stat(library)
            pathlib.Path(library).write_bytes(b"runtime-B")  # same length
            os.utime(library, ns=(old.st_atime_ns, old.st_mtime_ns))
            build._reset_fingerprint_caches()
            after = build._command_record(cmd)

            self.assertNotEqual(before["compiler"]["toolchain_tree"]["sha256"],
                                after["compiler"]["toolchain_tree"]["sha256"])
            self.assertNotEqual(before["fingerprint"], after["fingerprint"])

    def test_real_savestate_objects_record_the_source_and_reject_old_content(self):
        source = os.path.realpath(os.path.join(NATIVE_ROOT, "src", "os", "savestate.c"))
        current = build._sha256_file(source)
        stamps = list(pathlib.Path(os.path.join(NATIVE_ROOT, "build")).rglob(
            "plat_savestate*.o.cmd.json"
        ))
        if not stamps:
            self.skipTest("no shared-workspace savestate command stamps yet")
        for stamp_path in stamps:
            with open(stamp_path, "r", encoding="utf-8") as f:
                record = json.load(f)
            matches = [dep for dep in record.get("dependencies", [])
                       if os.path.normcase(dep["path"]) == os.path.normcase(source)]
            if not matches:
                # A command stamp from before the src/ move must invalidate,
                # not be mistaken for a corrupt newly-produced command stamp.
                legacy = os.path.realpath(os.path.join(NATIVE_ROOT, "platform", "os", "savestate.c"))
                migrated = [dep for dep in record.get("dependencies", [])
                            if os.path.normcase(dep["path"]) == os.path.normcase(legacy)]
                if len(migrated) == 1:
                    obj = str(stamp_path)[:-len(".cmd.json")]
                    build._reset_fingerprint_caches()
                    self.assertTrue(build.needs_rebuild(source, obj, record["command"]))
                    continue
            self.assertEqual(len(matches), 1, f"{stamp_path} must hash savestate.c itself")
            if matches[0]["sha256"] != current:
                obj = str(stamp_path)[:-len(".cmd.json")]
                build._reset_fingerprint_caches()
                self.assertTrue(
                    build.needs_rebuild(source, obj, record["command"]),
                    f"{obj} accepted old savestate.c content",
                )


class ArtifactStagingTests(unittest.TestCase):
    def test_dist_stages_only_the_selected_executables_pdb(self):
        with tempfile.TemporaryDirectory() as root:
            build_dir = pathlib.Path(root, "build")
            build_dir.mkdir()
            (build_dir / "selected.exe").write_bytes(b"exe")
            (build_dir / "selected.pdb").write_bytes(b"selected symbols")
            (build_dir / "other_mode.pdb").write_bytes(b"stale symbols")
            with mock.patch.object(
                    step_build, "required_artifacts",
                    return_value=[("selected.exe", "exe")]):
                dist = step_build.assemble_dist(
                    root, headless=False, windowed=True
                )
            self.assertEqual(
                {path.name for path in pathlib.Path(dist).iterdir()},
                {"selected.exe", "selected.pdb"},
            )

    def test_apk_inspection_requires_exact_staged_and_resource_bytes(self):
        with tempfile.TemporaryDirectory() as root:
            native = pathlib.Path(root)
            staged = native / "packaging/android/app/src/main/jniLibs/arm64-v8a"
            resource = native / "res/rml/test.rcss"
            staged.mkdir(parents=True)
            resource.parent.mkdir(parents=True)
            (staged / "libmain.so").write_bytes(b"main-native")
            (staged / "libmp6game.so").write_bytes(b"game-native")
            resource.write_bytes(b"resource-bytes")
            apk = native / "fixture.apk"

            def write_apk(game=b"game-native", asset=b"resource-bytes"):
                with zipfile.ZipFile(apk, "w", compression=zipfile.ZIP_STORED) as zf:
                    zf.writestr("lib/arm64-v8a/libmain.so", b"main-native")
                    zf.writestr("lib/arm64-v8a/libmp6game.so", game)
                    zf.writestr("assets/res/rml/test.rcss", asset)

            write_apk()
            self.assertTrue(step_android.inspect_apk(root, apk))
            write_apk(game=b"stale-game")
            with self.assertRaisesRegex(common.SetupError, "native library bytes differ"):
                step_android.inspect_apk(root, apk)
            write_apk(asset=b"stale-resource")
            with self.assertRaisesRegex(common.SetupError, "resource bytes differ"):
                step_android.inspect_apk(root, apk)

    def test_required_copy_uses_content_not_newer_destination_mtime(self):
        with tempfile.TemporaryDirectory() as root:
            src, dst = os.path.join(root, "source.dll"), os.path.join(root, "dest.dll")
            with open(src, "wb") as f:
                f.write(b"right")
            with open(dst, "wb") as f:
                f.write(b"wrong")
            future = time.time() + 3600
            os.utime(dst, (future, future))
            self.assertTrue(build._copy_required_file(src, dst, "test DLL"))
            with open(dst, "rb") as f:
                self.assertEqual(f.read(), b"right")

    def test_exact_tree_sync_removes_stale_files(self):
        with tempfile.TemporaryDirectory() as root:
            source, destination = os.path.join(root, "res-src"), os.path.join(root, "res-dst")
            os.makedirs(source)
            os.makedirs(destination)
            with open(os.path.join(source, "current.txt"), "w", encoding="utf-8") as f:
                f.write("current")
            with open(os.path.join(destination, "stale.txt"), "w", encoding="utf-8") as f:
                f.write("stale")
            build._sync_tree_exact(source, destination)
            self.assertEqual(set(os.listdir(destination)), {"current.txt"})

    def test_android_build_does_not_shadow_global_shutil(self):
        self.assertNotIn("shutil", build.build_android.__code__.co_varnames)


class SetupContractTests(unittest.TestCase):
    def test_zero_build_targets_are_rejected(self):
        with self.assertRaises(common.SetupError):
            step_build.required_artifacts(NATIVE_ROOT, headless=False, windowed=False)

    def test_dirty_decomp_requires_explicit_override(self):
        with tempfile.TemporaryDirectory() as root:
            subprocess.run(["git", "init", "-q", root], check=True)
            subprocess.run(["git", "-C", root, "config", "user.email", "test@example.invalid"], check=True)
            subprocess.run(["git", "-C", root, "config", "user.name", "Test"], check=True)
            os.makedirs(os.path.join(root, "src"))
            tracked = os.path.join(root, "src", "unit.c")
            with open(tracked, "w", encoding="utf-8") as f:
                f.write("one\n")
            subprocess.run(["git", "-C", root, "add", "src/unit.c"], check=True)
            subprocess.run(["git", "-C", root, "commit", "-qm", "base"], check=True)
            pin = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
            self.assertIsNone(step_decomp.decomp_checkout_problem(root, pin))
            os.makedirs(os.path.join(root, "scratch"))
            with open(os.path.join(root, "scratch", "note.txt"), "w", encoding="utf-8") as f:
                f.write("ignored scratch")
            self.assertIsNone(step_decomp.decomp_checkout_problem(root, pin))
            with open(tracked, "w", encoding="utf-8") as f:
                f.write("extra edit\n")
            self.assertIn("modified", step_decomp.decomp_checkout_problem(root, pin))
            with self.assertRaises(common.SetupError):
                step_decomp.validate_decomp_checkout(root, pin)
            self.assertTrue(step_decomp.validate_decomp_checkout(root, pin, allow_dirty=True))


class FstLimitTests(unittest.TestCase):
    def test_boot_bin_requires_exact_0x440_bytes(self):
        for size in (0x43F, 0x441):
            with self.assertRaises(common.SetupError):
                nod_ffi.validate_boot_bytes(b"GP6E01" + bytes(size - 6))
        self.assertTrue(nod_ffi.validate_boot_bytes(b"GP6E01" + bytes(0x440 - 6)))

    def test_fst_size_and_entry_caps(self):
        with self.assertRaises(common.SetupError):
            nod_ffi.validate_fst_bytes(b"\0" * (nod_ffi.MAX_FST_BYTES + 1))
        data = bytearray((nod_ffi.MAX_FST_ENTRIES + 1) * 12)
        struct.pack_into(">III", data, 0, 0x01000000, 0, nod_ffi.MAX_FST_ENTRIES + 1)
        with self.assertRaises(common.SetupError):
            nod_ffi.validate_fst_bytes(data)

    def test_zero_byte_category_files_do_not_satisfy_completeness(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "sys"))
            os.makedirs(os.path.join(root, "files", "data"))
            os.makedirs(os.path.join(root, "files", "mess"))
            os.makedirs(os.path.join(root, "files", "mic"))
            os.makedirs(os.path.join(root, "files", "sound"))
            with open(os.path.join(root, "sys", "boot.bin"), "wb") as f:
                f.write(b"GP6E01" + bytes(nod_ffi.BOOT_BIN_BYTES - 6))
            paths = [
                ("data", None), ("data/x", 0), ("mess", None), ("mess/x", 0),
                ("mic", None), ("mic/x", 0), ("sound", None),
                ("sound/MP6_SND.msm", 1), ("sound/MP6_Str.pdt", 1), ("opening.bnr", 1),
            ]
            names = bytearray(b"\0")
            offsets = {}
            for path, _size in paths:
                name = path.rsplit("/", 1)[-1]
                if name not in offsets:
                    offsets[name] = len(names)
                    names.extend(name.encode("ascii") + b"\0")
            entries = [(0x01000000, 0, 11)]
            entries += [
                (0x01000000 | offsets["data"], 0, 3), (offsets["x"], 0, 0),
                (0x01000000 | offsets["mess"], 0, 5), (offsets["x"], 0, 0),
                (0x01000000 | offsets["mic"], 0, 7), (offsets["x"], 0, 0),
                (0x01000000 | offsets["sound"], 0, 10),
                (offsets["MP6_SND.msm"], 0, 1), (offsets["MP6_Str.pdt"], 0, 1),
                (offsets["opening.bnr"], 0, 1),
            ]
            fst = b"".join(struct.pack(">III", *entry) for entry in entries) + bytes(names)
            with open(os.path.join(root, "sys", "fst.bin"), "wb") as f:
                f.write(fst)
            for path, size in paths:
                if size is None:
                    continue
                target = os.path.join(root, "files", *path.split("/"))
                with open(target, "wb") as f:
                    f.write(b"x" * size)
            with self.assertRaisesRegex(common.SetupError, "data, mess, mic"):
                nod_ffi.validate_extracted_root(root)
            for index in (2, 4, 6):
                entries[index] = (entries[index][0], entries[index][1], 1)
            fst = b"".join(struct.pack(">III", *entry) for entry in entries) + bytes(names)
            with open(os.path.join(root, "sys", "fst.bin"), "wb") as f:
                f.write(fst)
            for category in ("data", "mess", "mic"):
                with open(os.path.join(root, "files", category, "x"), "wb") as f:
                    f.write(b"x")
            # An extra plausible file outside fst.bin must not be copied by
            # the already-extracted-folder workflow.
            extra = os.path.join(root, "files", "data", "not-in-fst.bin")
            with open(extra, "wb") as f:
                f.write(b"do not copy")
            with tempfile.TemporaryDirectory() as dest_parent:
                dest = os.path.join(dest_parent, "GP6E01")
                nod_ffi.extract_disc_folder(root, dest)
                self.assertTrue(os.path.isfile(os.path.join(dest, "files", "data", "x")))
                self.assertFalse(os.path.exists(os.path.join(dest, "files", "data", "not-in-fst.bin")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
