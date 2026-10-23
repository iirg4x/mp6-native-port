"""Command-level checks for the Windows RmlUi release include roots.

These tests stop at command construction.  They deliberately do not invoke
Zig, CMake, a native executable, or a GPU: ``compile_one`` is made to reach
its subprocess boundary and the subprocess is replaced with a recorder.
"""

import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]

from tools import build  # noqa: E402


_INITIAL = {
    name: getattr(build, name)
    for name in (
        "WINDOWS_CONFIGURATION",
        "BUILD_DIR",
        "OBJ_DIR",
        "COMMON_FLAGS",
        "AURORA_ROOT",
        "AURORA_INCLUDE",
        "AURORA_BUILD_RMLUI",
        "AURORA_DEPS_RMLUI",
        "AURORA_FLAGS",
        "AURORA_RUNTIME_DLLS",
        "AURORA_LINK_ITEMS",
        "AURORA_ARTIFACT_STAMP",
    )
}


def _norm(path):
    """Normalize a command path for exact comparisons on Windows and CI."""
    return os.path.normcase(os.path.normpath(str(path)))


def _include_values(command, flag="-I"):
    return [
        _norm(command[index + 1])
        for index, value in enumerate(command[:-1])
        if value == flag
    ]


class RmlReleaseHeaderCommandTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(self._restore_build_state)
        self._restore_build_state()

    @staticmethod
    def _restore_build_state():
        for name, value in _INITIAL.items():
            setattr(build, name, list(value) if isinstance(value, list) else value)

    def _reset_debug_state(self, build_dir=None):
        self._restore_build_state()
        build.WINDOWS_CONFIGURATION = "debug"
        if build_dir is not None:
            build.BUILD_DIR = str(build_dir)
        build.OBJ_DIR = str(Path(build.BUILD_DIR) / "test-objects")

    def _capture_cpp_command(self):
        """Return the argv assembled by compile_one without invoking Zig."""
        source = str(ROOT / "src/gx/ui/ui.cpp")
        captured = []

        def fake_run(command, **_kwargs):
            captured.append(list(command))
            # A failed synthetic process avoids the success path's object and
            # depfile reads while still proving compile_one reached the exact
            # subprocess boundary under test.
            return SimpleNamespace(returncode=1, stdout="", stderr="synthetic")

        with patch.object(build, "needs_rebuild", return_value=True), \
                patch.object(build.subprocess, "run", side_effect=fake_run), \
                patch.object(build, "mp6_port_version", return_value="test"):
            result = build.compile_one((source, [], "storybook-ui.o", "aurora"))

        self.assertFalse(result[2], result)
        self.assertEqual(len(captured), 1)
        return captured[0]

    def test_debug_cpp_command_keeps_canonical_internal_root(self):
        """Debug's internal Aurora headers still resolve from AURORA_ROOT."""
        self._reset_debug_state()
        command = self._capture_cpp_command()
        include_dirs = _include_values(command)

        self.assertIn(_norm(_INITIAL["AURORA_ROOT"]), include_dirs)
        self.assertIn(_norm(_INITIAL["AURORA_INCLUDE"]), include_dirs)
        self.assertEqual(include_dirs.count(_norm(_INITIAL["AURORA_ROOT"])), 1)
        self.assertNotIn(
            _norm(Path(_INITIAL["BUILD_DIR"]) / "aurora-release-source"),
            include_dirs,
        )

    def test_release_cpp_command_uses_the_vendored_source_root(self):
        """Release public and internal headers come from third_party/aurora,
        whatever BUILD_DIR a QA build selects (there is no per-build copy)."""
        with tempfile.TemporaryDirectory(prefix="rml-release-", dir=ROOT / "build") as temp:
            for relative in (Path("qa-one"), Path("nested") / "qa-two"):
                qa_build = Path(temp) / relative
                self._reset_debug_state(qa_build)
                build.configure_windows("release")
                command = self._capture_cpp_command()
                include_dirs = _include_values(command)
                source_root = Path(build.AURORA_SOURCE)

                with self.subTest(build_dir=qa_build):
                    self.assertEqual(include_dirs.count(_norm(source_root)), 1)
                    self.assertEqual(include_dirs.count(_norm(source_root / "include")), 1)
                    self.assertNotIn(_norm(qa_build / "aurora-release-source"), include_dirs)

    def test_command_cache_rebuilds_when_compile_flags_change(self):
        """The existing command-record cache invalidates changed flags."""
        with tempfile.TemporaryDirectory(prefix="rml-command-cache-", dir=ROOT / "build") as temp:
            directory = Path(temp)
            source = directory / "ui.cpp"
            obj = directory / "ui.o"
            depfile = Path(str(obj) + ".d")
            stamp = Path(str(obj) + ".cmd.json")
            source.write_text("// command-cache fixture\n", encoding="utf-8")
            obj.write_bytes(b"object")
            depfile.write_text(f"{obj}: {source}\n", encoding="utf-8")

            old_command = [str(build.ZIG), "cc", "-O0", "-DPROFILE=debug"]
            new_command = [str(build.ZIG), "cc", "-O2", "-DPROFILE=release"]
            stamp.write_text(json.dumps({"command": old_command, "dependencies": []}),
                             encoding="utf-8")

            with patch.object(build, "_command_record",
                              side_effect=lambda command: {"command": list(command)}), \
                    patch.object(build, "_dependency_record", return_value=[]):
                self.assertFalse(build.needs_rebuild(str(source), str(obj), old_command))
                self.assertTrue(build.needs_rebuild(str(source), str(obj), new_command))

    def test_qa_output_override_preserves_selected_backend_headers(self):
        """Real QA builders redirect outputs after selecting the backend."""
        with tempfile.TemporaryDirectory(prefix="rml-qa-output-", dir=ROOT / "build") as temp:
            self._reset_debug_state()
            build.configure_windows("release")
            selected_source = Path(build.AURORA_INCLUDE).parent
            build.BUILD_DIR = str(Path(temp) / "settings-qa")
            build.OBJ_DIR = str(Path(build.BUILD_DIR) / "obj")
            include_dirs = _include_values(self._capture_cpp_command())
            self.assertIn(_norm(selected_source), include_dirs)
            self.assertIn(_norm(selected_source / "include"), include_dirs)
            self.assertNotIn(_norm(Path(build.BUILD_DIR) / "aurora-release-source"), include_dirs)


if __name__ == "__main__":
    unittest.main()
