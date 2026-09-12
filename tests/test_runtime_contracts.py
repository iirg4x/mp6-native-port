"""Runtime and source-layout regressions; dependency checkouts are read-only."""
from pathlib import Path
import hashlib
import os
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


class RuntimeContracts(unittest.TestCase):
    def test_title_replay_preserves_live_overlay_resources(self):
        relative = 'src/REL/bootDll/boot.c'
        original = (Path(build.DECOMP) / relative).read_text(encoding='utf-8')
        current = apply_patches.apply_unified_diff(
            original, (ROOT / 'compat/decomp' / (relative + '.patch')).read_text())
        # Replay stays within BootExec. Font/card/cursor graphs and title
        # assets carry HU_MEMNUM_OVL but remain live until omOvlKill. The
        # former replay-time bulk free invalidated them; pressing Start then
        # failed in HuWinAllKill. Keep the original loop lifecycle exactly.
        pattern = r'title_exec:\n.*?goto opening_exec;\n    }'
        self.assertEqual(re.search(pattern, current, re.S).group(),
                         re.search(pattern, original, re.S).group())

    def test_missing_overlay_returns_to_supported_history(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'overlay-selftest.exe'
            command = [build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                       'tests/native/overlay_fallback_selftest.c',
                       'src/os/overlay_fallback.c', '-o', str(exe)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], check=True, capture_output=True,
                                    text=True, timeout=5)
            self.assertIn('missing overlay return: PASS', result.stdout)

    def test_live_shadow_allocation_and_failure(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith('MP6_')}
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'shadow-selftest.exe'
            command = [build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                       'tests/native/shadow_settings_selftest.c',
                       'src/hsf/mp6_shadow_quality.c', 'src/enh/mp6_enhancements.c',
                       '-o', str(exe)]
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], env=env, check=True, capture_output=True,
                                    text=True, timeout=5)
            self.assertIn('live shadow allocation: PASS', result.stdout)

    def test_native_settings_and_minigame_continuation(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith('MP6_')}
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'runtime-selftest.exe'
            subprocess.run([build.ZIG, 'cc', '-O2', '-I', str(ROOT / 'include'),
                            'tests/native/runtime_settings_selftest.c',
                            'src/enh/mp6_enhancements.c', 'src/os/heap_scale.c',
                            'src/os/minigame_stub.c', '-o', str(exe)],
                           cwd=ROOT, env=env, check=True, capture_output=True)
            result = subprocess.run([str(exe)], env=env, check=True, capture_output=True,
                                    text=True, timeout=5)
            self.assertIn('continuation: PASS', result.stdout)

    def test_board_never_compiles_the_unavailable_minigame_transition(self):
        self.assertNotIn('src/board/mgcall.c', [source for source, _ in build.board_sources()])
        source = (ROOT / 'tools/build.py').read_text()
        self.assertEqual(source.count('"minigame_stub.c"'), 1)

    def test_board_skip_return_still_means_continue(self):
        source = (Path(build.DECOMP) / 'src/board/board.c').read_text(encoding='utf-8')
        transition = source[source.index('mgCallF = mbev_MgCall();'):]
        self.assertRegex(transition, r'if\s*\(!mgCallF\)\s*\{\s*mbNextTimeSet\(\);\s*HuPrcSleep\(-1\);\s*\}\s*else\s*\{\s*GwSystem.turnPlayerNo = 0;\s*mbNextTime\(\);')

    def test_consolidated_filesel_patch_preserves_host_state_fixes(self):
        relative = 'src/REL/fileseldll/filesel.c'
        original = (Path(build.DECOMP) / relative).read_text(encoding='utf-8', errors='surrogateescape')
        current = apply_patches.apply_unified_diff(original, (ROOT / 'compat/decomp' / (relative + '.patch')).read_text(encoding='utf-8', errors='surrogateescape'))
        # Verified against the old patch + fragment before consolidation.
        self.assertEqual(hashlib.sha256(current.encode('utf-8', errors='surrogateescape')).hexdigest(),
                         '01927378e68101aadb71cbe61f5ace5e61d16c79c6bbd152d65be61bfc9d457a')

    def test_compatibility_and_layout_contract(self):
        self.assertEqual(Path(apply_patches.DEFAULT_PATCHES_DIR), ROOT / 'compat/decomp')
        self.assertFalse((ROOT / 'CMakeLists.txt').exists())
        self.assertFalse((ROOT / 'patches').exists())
        self.assertTrue((ROOT / 'packaging/android/app/build.gradle').exists())
        self.assertTrue((ROOT / 'packaging/web/index.html').exists())


if __name__ == '__main__':
    unittest.main()
