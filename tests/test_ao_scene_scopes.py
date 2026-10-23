"""Executable source-order checks for AO scene/feedback boundaries.

These tests apply each REL patch to its pinned pristine source before slicing
the affected hook.  Checking the patched source (rather than patch text)
guards against a hunk drifting onto a declaration or an adjacent helper while
keeping the test independent of a full REL build.
"""
import unittest
from pathlib import Path

from tools.apply_patches import apply_unified_diff, require_pinned_decomp


ROOT = Path(__file__).resolve().parents[1]
DECOMP = Path(require_pinned_decomp())


def apply_rel(rel_path: str, patch_path: str) -> str:
    pristine = (DECOMP / rel_path).read_text(encoding="utf-8")
    patch = (ROOT / patch_path).read_text(encoding="utf-8")
    return apply_unified_diff(pristine, patch)


def function_slice(source: str, signature: str, next_signature: str) -> str:
    """Return a concrete function body, avoiding forward declarations."""
    start = source.rfind(signature + "\n{")
    if start < 0:
        raise AssertionError(f"definition not found: {signature}")
    end = source.find(next_signature + "\n{", start + len(signature) + 2)
    if end < 0:
        raise AssertionError(f"following definition not found: {next_signature}")
    return source[start:end]


class AmbientOcclusionSceneScopeTests(unittest.TestCase):
    def test_opening_excludes_page_capture_and_postdraw_reopens_depth(self):
        source = apply_rel(
            "src/REL/bootDll/opening.c",
            "compat/decomp/src/REL/bootDll/opening.c.patch",
        )
        pre = function_slice(
            source,
            "static void PreDrawHook(s16 layerNo)",
            "static void PostDrawHook(s16 layerNo)",
        )
        post = function_slice(
            source,
            "static void PostDrawHook(s16 layerNo)",
            "void OpeningKill(void)",
        )

        # The illustration capture is layer 0; exclude only that segment.
        self.assertIn(
            "Hu3DCameraLayerHookSet(OPENING_CAMERA_BIT, 0, PreDrawHook);",
            source,
        )
        self.assertIn(
            "Hu3DCameraLayerHookSet(OPENING_CAMERA_BIT, 8, PostDrawHook);",
            source,
        )
        exclude = pre.index("mp6_ao_camera_exclude(Hu3DCameraNo);")
        native_capture = pre.index(
            "mp6_widescreen_camera_native_target(OPENING_CAMERA_BIT, 1);"
        )
        self.assertLess(exclude, native_capture)
        self.assertNotIn("mp6_ao_camera_exclude", post)

        # PostDrawHook's depth clear is after the page/frame copy.  The
        # shared Hu3DZClear hook retires the illustrated depth and reopens the
        # next eligible 3D segment before the subsequent layer.
        clear = post.index("Hu3DZClear();")
        self.assertLess(post.rfind("Hu3DFbCopyExec(", 0, clear), clear)
        self.assertNotIn("mp6_enh_set_values", source)
        self.assertNotRegex(source, r"ambientOcclusion\s*=")

    def test_mdsel_feedback_ends_then_excludes_before_ortho(self):
        source = apply_rel(
            "src/REL/mdseldll/mdsel.c",
            "compat/decomp/src/REL/mdseldll/mdsel.c.patch",
        )
        body = function_slice(
            source,
            "void fn_1_2A78(s16 layerNo)",
            "void fn_1_30FC(void)",
        )
        end = body.index("mp6_ao_camera_end(Hu3DCameraNo);")
        exclude = body.index("mp6_ao_camera_exclude(Hu3DCameraNo);")
        ortho = body.index("MTXOrtho(")
        self.assertLess(end, exclude)
        self.assertLess(exclude, ortho)
        self.assertEqual(body.count("mp6_ao_camera_exclude(Hu3DCameraNo);"), 1)
        self.assertNotIn("mp6_enh_set_values", source)
        self.assertNotRegex(source, r"ambientOcclusion\s*=")

    def test_party_feedback_ends_then_excludes_and_clears_after_copy(self):
        source = apply_rel(
            "src/REL/mdpartydll/stage.c",
            "compat/decomp/src/REL/mdpartydll/stage.c.patch",
        )
        body = function_slice(
            source,
            "void fn_1_400E0(s16 layerNo)",
            "void fn_1_40A9C(void)",
        )
        end = body.index("mp6_ao_camera_end(Hu3DCameraNo);")
        exclude = body.index("mp6_ao_camera_exclude(Hu3DCameraNo);")
        ortho = body.index("MTXOrtho(")
        copy = body.index("GXCopyTex(")
        clear = body.index("Hu3DZClear();")
        self.assertLess(end, exclude)
        self.assertLess(exclude, ortho)
        self.assertLess(copy, clear)
        self.assertEqual(body.count("mp6_ao_camera_exclude(Hu3DCameraNo);"), 1)
        self.assertNotIn("mp6_enh_set_values", source)
        self.assertNotRegex(source, r"ambientOcclusion\s*=")


if __name__ == "__main__":
    unittest.main()
