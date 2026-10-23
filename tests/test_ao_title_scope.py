"""Static lifecycle checks for the boot title's authored-art AO scope."""

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import apply_patches, build  # noqa: E402


def patched(relative):
    original = (Path(build.DECOMP) / relative).read_text(encoding="utf-8")
    patch = ROOT / "compat" / "decomp" / (relative + ".patch")
    if patch.exists():
        original = apply_patches.apply_unified_diff(
            original, patch.read_text(encoding="utf-8"), label=relative
        )
    return original


class AoTitleScopeTests(unittest.TestCase):
    def test_boot_patch_applies_and_excludes_only_title_scope(self):
        source = patched("src/REL/bootDll/boot.c")
        hook_start = source.index("static void BootTitleAOPreDrawHook")
        hook_end = source.index("void BootTitleCreate", hook_start)
        hook = source[hook_start:hook_end]

        self.assertIn("mp6_ao_camera_exclude(Hu3DCameraNo);", hook)
        self.assertNotIn("Hu3DLayerHookSet", hook)
        self.assertNotIn("Hu3DCameraLayerHookSet", hook)

        title_start = source.index("s32 BootTitleExec(s32 firstRun)\n{")
        title_end = source.index("void *NintendoDataDecode", title_start)
        title = source[title_start:title_end]
        self.assertLess(
            title.index("Hu3DCameraLayerHookSet(1, 0, BootTitleAOPreDrawHook)"),
            title.index("mp6_widescreen_camera_widen(1, 20.0f"),
        )

    def test_title_exclusion_is_removed_after_final_boot_wipe(self):
        source = patched("src/REL/bootDll/boot.c")
        boot = source[
            source.index("void BootExec(void)\n{"):source.index("void BootProgExec(void)\n{")
        ]
        wipe = boot.index("WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 60)")
        reset = boot.index("Hu3DCameraLayerHookReset(1, 0)")
        self.assertGreater(reset, wipe)
        self.assertLess(reset, boot.index("HuDataGetAsyncStat", reset))

    def test_opening_replaces_title_hook_and_cleans_all_models(self):
        source = patched("src/REL/bootDll/opening.c")
        opening = source[
            source.index("void OpeningExec(s32 enablePrompt)\n{"):source.index(
                "void OpeningEventExec(void)\n{"
            )
        ]
        self.assertIn(
            "Hu3DCameraLayerHookSet(OPENING_CAMERA_BIT, 0, PreDrawHook);",
            opening,
        )
        self.assertIn("Hu3DCameraLayerHookReset(OPENING_CAMERA_BIT, 0);", opening)
        self.assertIn("Hu3DCameraLayerHookReset(OPENING_CAMERA_BIT, 8);", opening)
        self.assertIn("for (j = 0; j < 8; j++)", opening)
        self.assertIn("Hu3DModelAttrSet(OpeningMdlId[j], HU3D_ATTR_DISPOFF);", opening)


if __name__ == "__main__":
    unittest.main()
