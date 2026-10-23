"""CPU-only safety tests for the isolated desktop AO candidate builder."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "integration" / "build_ao_candidate.py"
SPEC = importlib.util.spec_from_file_location("build_ao_candidate_test_module", SOURCE)
assert SPEC and SPEC.loader
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class FakeBuild:
    def __init__(self, output: Path) -> None:
        self.BUILD_DIR = "canonical-build"
        self.OBJ_DIR = "canonical-objects"
        self.output = output
        self.calls: list[tuple[str, str]] = []
        self.main_argv: list[str] | None = None

    def configure_windows(self, configuration: str) -> None:
        self.calls.append(("configure", self.BUILD_DIR))
        if configuration != "release":
            raise AssertionError(configuration)
        self.OBJ_DIR = "canonical-build/obj-release"

    def collect_units(self, *args: object, **kwargs: object):
        self.calls.append(("collect", self.BUILD_DIR))
        return [("unchanged.c", [], "game_unchanged.o", "common")]

    def main(self) -> int:
        self.main_argv = list(sys.argv)
        self.configure_windows("release")
        self.collect_units(False)
        executable = Path(self.BUILD_DIR) / "release" / "mp6native.exe"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b"private candidate executable")
        return 0


class FailingBuild(FakeBuild):
    def main(self) -> int:
        self.main_argv = list(sys.argv)
        self.configure_windows("release")
        self.collect_units(False)
        return 17


class AoCandidateBuilderTests(unittest.TestCase):
    def test_import_is_side_effect_free_and_does_not_import_build(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            candidate = Path(temporary) / "candidate"
            with mock.patch.object(builder, "OUTPUT", candidate):
                self.assertFalse(candidate.exists())
                self.assertEqual(builder.source_selector_policy()["default"], "xegtao")
            self.assertFalse(candidate.exists())

        # Re-import through a fresh module object with a hostile ``build``
        # sentinel. Importing the builder must not load or touch that module;
        # only main() is allowed to invoke the build driver.
        module_name = "build_ao_candidate_import_only"
        fresh_spec = importlib.util.spec_from_file_location(module_name, SOURCE)
        assert fresh_spec and fresh_spec.loader
        fresh = importlib.util.module_from_spec(fresh_spec)
        with mock.patch.dict(sys.modules, {"build": object()}):
            fresh_spec.loader.exec_module(fresh)

    def test_configure_resolves_canonical_release_before_redirecting(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "candidate"
            fake = FakeBuild(output)
            original_argv = list(sys.argv)
            before_env = {
                key: os.environ.get(key)
                for key in ("MP6_AO_XEGTAO", "MP6_AO_XEGTAO_QUALITY",
                            "MP6_AO_XEGTAO_DENOISE", "MP6_AO_XEGTAO_RESOLUTION")
            }
            result = builder.main(build_module=fake, output=output)
            self.assertEqual(result, 0)
            self.assertEqual(fake.calls[0], ("configure", "canonical-build"))
            self.assertEqual(fake.calls[1], ("collect", str(output.resolve())))
            self.assertEqual(fake.main_argv[1:], ["--configuration", "release"])
            self.assertEqual(sys.argv, original_argv)
            self.assertEqual(fake.BUILD_DIR, "canonical-build")
            self.assertEqual(fake.OBJ_DIR, "canonical-objects")
            self.assertEqual(
                {key: os.environ.get(key) for key in before_env}, before_env
            )

            executable = output / "release" / "mp6native.exe"
            provenance = json.loads((output / "provenance.json").read_text())
            self.assertEqual(provenance["release_asset"], False)
            self.assertEqual(provenance["exe_sha256"], hashlib.sha256(executable.read_bytes()).hexdigest())
            policy = provenance["source_selector_policy"]
            self.assertEqual(policy["default"], "xegtao")
            self.assertEqual(policy["android_default"], "legacy")
            self.assertEqual(policy["default_policy"]["quality"], "high")
            self.assertEqual(policy["opt_in"]["quality"], "ultra")
            self.assertEqual(policy["opt_in"]["denoise"], 3)
            self.assertEqual(policy["opt_in"]["resolution"], ["full", "half"])
            self.assertEqual(policy["opt_in"]["environment"]["MP6_AO_XEGTAO"], "1")
            self.assertEqual(policy["opt_in"]["environment"]["MP6_AO_XEGTAO_QUALITY"], "ultra")
            self.assertEqual(policy["opt_in"]["environment"]["MP6_AO_XEGTAO_DENOISE"], "3")
            self.assertEqual(policy["opt_in"]["environment"]["MP6_AO_XEGTAO_RESOLUTION"], ["full", "half"])
            self.assertFalse(provenance["source_transforms"])
            self.assertFalse(provenance["qa_probes"])
            self.assertNotIn("MP6_AO_XEGTAO", fake.__dict__)

    def test_production_release_output_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            builder.main(output=ROOT / "build" / "release")

    def test_failed_build_invalidates_private_provenance_only(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "candidate"
            output.mkdir(parents=True)
            (output / "provenance.json").write_text(
                json.dumps({"release_asset": False, "exe_sha256": "stale"}),
                encoding="utf-8",
            )
            canonical_exe = root / "canonical" / "release" / "mp6native.exe"
            canonical_exe.parent.mkdir(parents=True)
            canonical_exe.write_bytes(b"canonical release sentinel")
            canonical_before = canonical_exe.read_bytes()
            fake = FailingBuild(output)
            with mock.patch.object(builder, "PROTECTED_EXE", canonical_exe):
                result = builder.main(build_module=fake, output=output)
            self.assertEqual(result, 17)
            self.assertFalse((output / "provenance.json").exists())
            self.assertEqual(canonical_exe.read_bytes(), canonical_before)

    def test_cache_seed_is_private_and_retargets_freshness_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / "obj-release"
            candidate = root / "candidate" / "obj"
            original.mkdir()
            source = root / "patched" / "source.c"
            source.parent.mkdir()
            source.write_text("int unchanged;\n")
            object_path = original / "game_source.o"
            object_path.write_bytes(b"object")
            (original / "game_source.o.d").write_text(
                f"{object_path}: {source}\n", encoding="utf-8"
            )
            record = {
                "contract": 5,
                "command": ["cc", "-MD", "-MF", str(original / "game_source.o.d"),
                            "-MT", str(object_path), "-c", str(source), "-o", str(object_path)],
                "dependencies": [{"path": str(source), "sha256": "fixture"}],
            }
            (original / "game_source.o.cmd.json").write_text(json.dumps(record), encoding="utf-8")
            copied = builder._seed_cached_object(
                (str(source), [], "game_source.o", "common"), original, candidate
            )
            self.assertTrue(copied)
            self.assertTrue((candidate / "game_source.o").is_file())
            copied_record = json.loads((candidate / "game_source.o.cmd.json").read_text())
            self.assertEqual(copied_record["command"][-1], str(candidate / "game_source.o"))
            self.assertEqual(copied_record["command"][7], str(source))
            self.assertNotIn(str(original), json.dumps(copied_record["command"]))

    def test_seeded_record_is_accepted_by_build_freshness_oracle(self) -> None:
        source_object = ROOT / "build" / "obj-release" / "board_dice.o"
        source_record = Path(str(source_object) + ".cmd.json")
        source_depfile = Path(str(source_object) + ".d")
        if not (source_object.is_file() and source_record.is_file() and source_depfile.is_file()):
            self.skipTest("the local Release object cache is unavailable")

        tools_root = str(ROOT / "tools")
        sys.path.insert(0, tools_root)
        try:
            import build as build_driver
        finally:
            sys.path.remove(tools_root)

        with tempfile.TemporaryDirectory() as temporary:
            candidate = Path(temporary) / "obj"
            record = json.loads(source_record.read_text(encoding="utf-8"))
            command = record["command"]
            source = Path(command[command.index("-c") + 1])
            self.assertTrue(builder._seed_cached_object(
                (str(source), [], source_object.name, "common"),
                source_object.parent,
                candidate,
            ))
            candidate_object = candidate / source_object.name
            candidate_record = json.loads(Path(str(candidate_object) + ".cmd.json").read_text())
            # Seeding must keep the cache entry's freshness exactly: fresh stays
            # fresh, stale stays stale. Whether the live Release cache is fresh
            # depends on when this checkout last built, so it is not asserted.
            self.assertEqual(
                build_driver.needs_rebuild(str(source), str(candidate_object), candidate_record["command"]),
                build_driver.needs_rebuild(str(source), str(source_object), record["command"]),
            )


if __name__ == "__main__":
    unittest.main()
