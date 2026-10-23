"""Source/loader coverage is distinct from gameplay and visual audit status."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

from tools import apply_patches, build, native_boards, native_modes, native_support


ROOT = Path(__file__).resolve().parents[1]
FAMILIES = {
    "minigames": {"m" + number for number in (
        "602 608 609 612 613 616 617 618 619 620 621 625 626 627 629 630 631 632 633 "
        "635 636 637 638 640 642 643 644 646 647 649 650 651 656 657 658 659 660 662 "
        "664 665 666 667 668 670 674 675 676 680"
    ).split()},
    "modes": set("mdmic mdmini mdparty mdpresult mdsel mdsing mgmbattle mgmbingo "
                 "mgmdecathlon mgmfree mgmrensho mgmtournament ttwars".split()),
    "boards": set("w01 w02 w03 w04 w05 w06 w10 w11 s01 s02 s03".split()),
    "support": set("actman boot ending filesel inst meschk micquiz micquizsel miraclebook "
                   "motchk opening option result saf selmenu sequence staff".split()),
}


class NativeInventory(unittest.TestCase):
    def test_new_owners_have_declared_native_abis(self):
        if not (Path(build.DECOMP) / 'src/REL').is_dir() or not Path(build.ZIG).is_file():
            self.skipTest('native compiler and pinned checkout are required')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            directory = Path(directory)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            new_modes = {'mdmicdll', 'mdsingdll', 'mgmbattledll',
                         'mgmbingodll', 'mgmdecathlondll'}
            for inventory in (native_boards, native_support, native_modes):
                for relative, flags in inventory.sources(build.DECOMP):
                    if inventory is native_modes and Path(relative).parent.name not in new_modes:
                        continue
                    with self.subTest(unit=relative):
                        source = patched / relative
                        if not source.is_file():
                            source = Path(build.DECOMP) / relative
                        output = directory / (Path(relative).parent.name + '_' +
                                              Path(relative).stem + '.o')
                        command = build.transport_long_defines(
                            [build.ZIG, 'cc', '-I', str(patched / 'include'),
                             *[f for f in build.COMMON_FLAGS if f != '-w'], *flags,
                             '-Werror=implicit-function-declaration',
                             '-Werror=incompatible-function-pointer-types',
                             '-Werror=incompatible-pointer-types',
                             '-Werror=int-conversion', '-Werror=pointer-to-int-cast',
                             '-Werror=int-to-pointer-cast',
                             '-c', str(source), '-o', str(output)], str(output))
                        result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                                text=True, timeout=120)
                        self.assertEqual(result.returncode, 0, result.stderr)

    def test_support_selector_import_does_not_read_the_checkout(self):
        result = subprocess.run(
            [sys.executable, "-c", "from pathlib import Path; "
             "reject=lambda *a,**k: (_ for _ in ()).throw(AssertionError('eager read')); "
             "Path.read_text=reject; Path.read_bytes=reject; Path.glob=reject; "
             "Path.rglob=reject; from tools import native_support"],
            cwd=ROOT, env={**os.environ, "PYTHONPATH": str(ROOT)},
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_all_89_complete_modules_have_sources_and_loader_routes(self):
        if not (Path(build.DECOMP) / "src/REL").is_dir():
            self.skipTest("pinned decomp checkout is not installed")
        selected = build.native_rel_sources()
        actual = {
            Path(path).parts[2].lower().removesuffix("dll")
            for path, _ in selected
            if path.startswith("src/REL/") and len(Path(path).parts) > 3
        }
        expected = set().union(*FAMILIES.values())
        self.assertEqual(len(expected), 89)
        self.assertEqual(actual, expected)
        bridge = (ROOT / "src/os/dll_bridge.c").read_text().lower()
        for module in sorted(expected):
            with self.subTest(module=module):
                self.assertIn(f'"dll/{module}dll.rel"', bridge)
        for path, _ in selected:
            self.assertTrue((Path(build.DECOMP) / path).is_file(), path)
            self.assertNotEqual(Path(path).name, "runtime.c")
        names = build.rel_object_names(selected)
        self.assertEqual(len(names), len(set(name.lower() for name in names)))


if __name__ == "__main__":
    unittest.main()
