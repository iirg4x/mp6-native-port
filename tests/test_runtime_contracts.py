"""Runtime and source-layout regressions; dependency checkouts are read-only."""
from pathlib import Path
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

    def test_native_settings(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith('MP6_')}
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            exe = Path(temporary) / 'runtime-selftest.exe'
            subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-O2', '-UNDEBUG',
                            'tests/native/runtime_settings_selftest.c',
                            'src/enh/mp6_enhancements.c', 'src/os/heap_scale.c',
                            '-o', str(exe)],
                           cwd=ROOT, env=env, check=True, capture_output=True)
            result = subprocess.run([str(exe)], env=env, check=True, capture_output=True,
                                    text=True, timeout=5)
            self.assertIn('runtime settings: PASS', result.stdout)

    def test_board_compiles_the_real_minigame_flow(self):
        # Boards run the recovered roulette/instruction hand-off; only a
        # missing minigame module falls back (src/os/overlay_fallback.c).
        self.assertIn('src/board/mgcall.c', [source for source, _ in build.board_sources()])
        self.assertFalse((ROOT / 'src/os/minigame_stub.c').exists())
        flags = dict(build.board_sources())['src/board/mgcall.c']
        self.assertIn('-DmbMgCallSingleOnCheck=mp6_mgcall_single_on_check', flags)
        self.assertIn('-DmbMgCallVsEffCreate=mp6_mgcall_vs_eff_create', flags)
        self.assertIn('src/os/board_mgcall_abi.c', build.PLATFORM_SOURCES_COMMON)
        adapters = (ROOT / 'src/os/board_mgcall_abi.c').read_text(encoding='utf-8')
        self.assertIn('BOOL mbMgCallSingleOnCheck(u16 ovl)', adapters)
        self.assertIn('void mbMgCallVsEffCreate(void)', adapters)

    def test_board_skip_return_still_means_continue(self):
        source = (Path(build.DECOMP) / 'src/board/board.c').read_text(encoding='utf-8')
        call = re.search(r'\b(\w+)\s*=\s*mbev_MgCall\(\);', source)
        self.assertIsNotNone(call)
        transition = source[call.end():]
        self.assertRegex(transition, r'if\s*\(!\s*' + re.escape(call[1])
                         + r'\s*\)\s*\{\s*mbNextTimeSet\(\);\s*HuPrcSleep\(-1\);\s*\}\s*else\s*\{\s*GwSystem.turnPlayerNo\s*=\s*0;\s*mbNextTime\(\);')

    def test_consolidated_filesel_patch_preserves_host_state_fixes(self):
        relative = 'src/REL/fileseldll/filesel.c'
        original = (Path(build.DECOMP) / relative).read_text(encoding='utf-8', errors='surrogateescape')
        current = apply_patches.apply_unified_diff(original, (ROOT / 'compat/decomp' / (relative + '.patch')).read_text(encoding='utf-8', errors='surrogateescape'))
        # Check the host fixes, not an entire-source hash tied to an old
        # upstream spelling. Capture/texgen and badge geometry have separate
        # executable coverage in test_widescreen_effects.
        code = re.sub(r'/\*.*?\*/|//[^\n]*', '', current, flags=re.S)
        self.assertRegex(code, r'if\s*\(slotObject\s*!=\s*NULL\)\s*\{\s*strcpy\(slotObject->name,\s*lbl_1_data_190\[i\]\);')
        self.assertRegex(code, r'if\s*\(i\s*>=\s*4\)\s*\{\s*HuPrcEnd\(\);\s*return;\s*\}\s*prevState\s*=\s*p->visualState;')
        self.assertRegex(code, r'mp6_anim_unregister_for_free\(lbl_1_bss_D8\[i\]\.cardTextureAnim\);\s*if\s*\(mp6PackedBacking\s*!=\s*NULL\)\s*\{\s*HuMemDirectFree\(mp6PackedBacking\);\s*\}\s*HuMemDirectFree\(lbl_1_bss_D8\[i\]\.cardTextureAnim\);')
        for assignment in ('proc->property = (void *)(intptr_t)processProperty;',
                           'bank = (s32)(intptr_t)proc->property;',
                           'proc2->property = (void *)(intptr_t)selectedBoxNo;',
                           'idx = (s32)(intptr_t)proc->property;'):
            self.assertIn(assignment, code)

    def test_compatibility_and_layout_contract(self):
        self.assertEqual(Path(apply_patches.DEFAULT_PATCHES_DIR), ROOT / 'compat/decomp')
        self.assertFalse((ROOT / 'CMakeLists.txt').exists())
        self.assertFalse((ROOT / 'patches').exists())
        self.assertTrue((ROOT / 'packaging/android/app/build.gradle').exists())
        self.assertTrue((ROOT / 'packaging/web/index.html').exists())


if __name__ == '__main__':
    unittest.main()
