"""Focused release-driver integrity checks.

The broader setup regression module covers the normal release-driver path.
These cases exercise the two fail-closed edges that are easy to miss when
allowlisting generated output: tracked files must never be exempt, and both
build stages must be followed by a checkout-integrity recheck.
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
NATIVE_ROOT = HERE.parent.parent
TOOLS = NATIVE_ROOT / "tools"

for path in (NATIVE_ROOT, TOOLS):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import release_android  # noqa: E402


class ReleaseAndroidIntegrityTests(unittest.TestCase):
    def _repo_with_tracked_build_file(self):
        temp = tempfile.TemporaryDirectory()
        root = pathlib.Path(temp.name)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        (root / ".gitignore").write_text(
            "build/\n*.log\n__pycache__/\n*.pyc\n", encoding="utf-8"
        )
        build = root / "build"
        build.mkdir()
        tracked = build / "checked-in.txt"
        tracked.write_text("clean\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(root), "add", ".gitignore"], check=True)
        subprocess.run(
            ["git", "-C", str(root), "add", "-f", "build/checked-in.txt"],
            check=True,
        )
        subprocess.run(
            [
                "git", "-C", str(root), "-c", "user.name=MP6 Test", "-c",
                "user.email=mp6-test@example.invalid", "commit", "-qm", "fixture",
            ],
            check=True,
        )
        return temp, root, tracked

    def test_tracked_file_under_generated_prefix_is_not_allowlisted(self):
        temp, root, tracked = self._repo_with_tracked_build_file()
        with temp:
            tracked.write_text("changed\n", encoding="utf-8")
            problem = release_android.release_checkout_problem(root)
        self.assertIsNotNone(problem)
        self.assertIn("differs from HEAD", problem)
        self.assertIn("checked-in.txt", problem)

    def test_release_driver_caches_are_allowed_but_source_cache_is_not(self):
        temp, root, _tracked = self._repo_with_tracked_build_file()
        with temp:
            for directory in (root / "setup/lib/__pycache__", root / "tools/__pycache__"):
                directory.mkdir(parents=True)
                (directory / "driver.cpython-313.pyc").write_bytes(b"generated")
            self.assertIsNone(release_android.release_checkout_problem(root))

            source_cache = root / "res/surprise.pyc"
            source_cache.parent.mkdir()
            source_cache.write_bytes(b"unexpected release input")
            problem = release_android.release_checkout_problem(root)
        self.assertIsNotNone(problem)
        self.assertIn("surprise.pyc", problem)

    def test_root_log_allowlist_rejects_a_directory(self):
        temp, root, _tracked = self._repo_with_tracked_build_file()
        with temp:
            (root / "release.log").write_text("generated log\n", encoding="utf-8")
            self.assertIsNone(release_android.release_checkout_problem(root))

            disguised = root / "payload.log"
            disguised.mkdir()
            (disguised / "source.c").write_text("unexpected\n", encoding="utf-8")
            problem = release_android.release_checkout_problem(root)
        self.assertIsNotNone(problem)
        self.assertIn("payload.log", problem)

    def test_malformed_porcelain_output_fails_closed(self):
        temp, root, _tracked = self._repo_with_tracked_build_file()
        with temp, mock.patch.object(
            release_android.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                [], 0, stdout="??", stderr=""
            ),
        ):
            problem = release_android.release_checkout_problem(root)
        self.assertIsNotNone(problem)
        self.assertIn("??", problem)

    def test_native_build_is_followed_by_integrity_recheck(self):
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.object(
            release_android, "_require_release_checkout", side_effect=[True, False]
        ) as gate, mock.patch.object(
            release_android.subprocess, "run", return_value=completed
        ), mock.patch.object(release_android.step_android, "build_apk") as apk:
            self.assertEqual(release_android.main([]), 1)
        self.assertEqual(gate.call_count, 2)
        apk.assert_not_called()

    def test_apk_assembly_is_followed_by_integrity_recheck(self):
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.object(
            release_android, "_require_release_checkout", side_effect=[True, True, False]
        ) as gate, mock.patch.object(
            release_android.subprocess, "run", return_value=completed
        ), mock.patch.object(
            release_android.step_android, "build_apk", return_value="release.apk"
        ) as apk:
            self.assertEqual(release_android.main([]), 1)
        self.assertEqual(gate.call_count, 3)
        apk.assert_called_once_with(release_android.NATIVE_ROOT, variant="release")


if __name__ == "__main__":
    unittest.main(verbosity=2)
