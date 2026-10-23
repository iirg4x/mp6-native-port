"""Bounded checks for worker-local RmlUi pass binding reuse and transitions."""

from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


TARGETS = {
    "lib/gfx/common.cpp",
    "lib/rmlui/pipeline.cpp",
    "lib/rmlui/pipeline.hpp",
}


@dataclass
class Slot:
    ref: int = 0
    dynamic: bool = False
    offset: int = 0
    valid: bool = False


class PassBindingKernel:
    """Executable model of one RmlPassBindingState lifetime."""

    def __init__(self):
        self.pipeline = 0
        self.pipeline_valid = False
        self.slots = [Slot() for _ in range(3)]
        self.common_resolves = 0
        self.lookups = 0
        self.set_calls = []
        self.rml_bindings_active = False
        self.restore_calls = 0

    def invalidate(self):
        self.pipeline = 0
        self.pipeline_valid = False
        self.slots = [Slot() for _ in range(3)]

    def begin_pass(self):
        self.invalidate()

    def _bind(self, index, ref, dynamic, offset):
        if ref == 0:
            return
        slot = self.slots[index]
        if (slot.valid and slot.ref == ref and slot.dynamic == dynamic and
                (not dynamic or slot.offset == offset)):
            return
        if not slot.valid or slot.ref != ref:
            self.lookups += 1
        self.set_calls.append((index, ref, dynamic, offset))
        slot.ref = ref
        slot.dynamic = dynamic
        slot.offset = offset
        slot.valid = True

    def render_rml(self, pipeline, common_offset, group1=0, group1_dynamic=False,
                   group1_offset=0, group2=0, group2_dynamic=False, group2_offset=0):
        if not self.pipeline_valid or self.pipeline != pipeline:
            self.invalidate()
            self.pipeline = pipeline
            self.pipeline_valid = True

        common = self.slots[0]
        if not common.valid or not common.dynamic or common.offset != common_offset:
            if common.valid:
                common_ref = common.ref
            else:
                self.common_resolves += 1
                common_ref = 100
            self._bind(0, common_ref, True, common_offset)

        self._bind(1, group1, group1_dynamic, group1_offset)
        self._bind(2, group2, group2_dynamic, group2_offset)
        self.rml_bindings_active = True

    def clear(self):
        # Clear does not touch bind groups; a later GX still needs to restore
        # the static group 0/empty group 2 pair left by a successful Rml draw.
        self.invalidate()

    def _restore_non_rml_bindings(self):
        if self.rml_bindings_active:
            self.restore_calls += 1
            self.rml_bindings_active = False

    def gx(self, succeeded=True):
        # Restore once on the Rml -> GX transition, including when GX fails.
        self._restore_non_rml_bindings()
        self.invalidate()
        return succeeded

    def custom(self):
        self.invalidate()
        # Custom draw replay restores the static groups after the callback.
        self.rml_bindings_active = False


class RmlPassBindingTests(unittest.TestCase):
    def test_alternating_dynamic_static_and_offset_only(self):
        kernel = PassBindingKernel()
        kernel.begin_pass()

        kernel.render_rml(7, 64, 11, True, 4, 12, False)
        self.assertEqual(kernel.common_resolves, 1)
        self.assertEqual(kernel.lookups, 3)
        self.assertEqual(len(kernel.set_calls), 3)

        # Offset-only changes reuse all three handles but still update the two
        # dynamic bindings whose offsets changed.
        kernel.render_rml(7, 128, 11, True, 8, 12, False, 99)
        self.assertEqual(kernel.common_resolves, 1)
        self.assertEqual(kernel.lookups, 3)
        self.assertEqual(len(kernel.set_calls), 5)

        # Static/dynamic transitions require a SetBindGroup, while an offset
        # change in static mode is intentionally ignored.
        kernel.render_rml(7, 128, 11, False, 99, 12, False)
        self.assertEqual(len(kernel.set_calls), 6)
        kernel.render_rml(7, 128, 11, False, 1, 12, False, 1)
        self.assertEqual(len(kernel.set_calls), 6)
        kernel.render_rml(7, 128, 11, False, 1, 12, True, 3)
        self.assertEqual(len(kernel.set_calls), 7)
        kernel.render_rml(7, 128, 11, False, 1, 12, True, 3)
        self.assertEqual(len(kernel.set_calls), 7)

    def test_pipeline_change_rebinds_all_slots_and_common_is_lazy(self):
        kernel = PassBindingKernel()
        kernel.begin_pass()
        kernel.render_rml(1, 0, 11, True, 2, 12, True, 3)
        before = (kernel.common_resolves, kernel.lookups, len(kernel.set_calls))

        kernel.render_rml(2, 0, 11, True, 2, 12, True, 3)
        self.assertEqual(kernel.common_resolves, before[0] + 1)
        self.assertEqual(kernel.lookups, before[1] + 3)
        self.assertEqual(len(kernel.set_calls), before[2] + 3)

    def test_failed_gx_and_custom_draw_interruptions_invalidate(self):
        kernel = PassBindingKernel()
        kernel.begin_pass()
        draw = dict(pipeline=4, common_offset=16, group1=21, group1_dynamic=True,
                    group1_offset=5, group2=22, group2_dynamic=False)
        kernel.render_rml(**draw)
        baseline = (kernel.common_resolves, kernel.lookups, len(kernel.set_calls))

        self.assertFalse(kernel.gx(succeeded=False))
        kernel.render_rml(**draw)
        self.assertEqual(kernel.common_resolves, baseline[0] + 1)
        self.assertEqual(kernel.lookups, baseline[1] + 3)
        self.assertEqual(len(kernel.set_calls), baseline[2] + 3)
        self.assertEqual(kernel.restore_calls, 1)

        kernel.custom()
        kernel.render_rml(**draw)
        self.assertEqual(kernel.common_resolves, baseline[0] + 2)
        self.assertEqual(kernel.lookups, baseline[1] + 6)
        self.assertEqual(len(kernel.set_calls), baseline[2] + 6)
        self.assertEqual(kernel.restore_calls, 1)

    def test_restore_is_transition_guarded_across_clear_and_gx_runs(self):
        kernel = PassBindingKernel()
        kernel.begin_pass()
        kernel.render_rml(6, 24, 17, True, 3, 18, False)

        kernel.clear()
        self.assertTrue(kernel.rml_bindings_active)
        self.assertFalse(kernel.gx(succeeded=False))
        self.assertEqual(kernel.restore_calls, 1)
        self.assertFalse(kernel.rml_bindings_active)

        # A second GX in the same pass is already on the restored static state.
        self.assertTrue(kernel.gx(succeeded=True))
        self.assertEqual(kernel.restore_calls, 1)

    def test_render_pass_boundary_does_not_leak_handles(self):
        kernel = PassBindingKernel()
        draw = dict(pipeline=9, common_offset=32, group1=31, group1_dynamic=False)
        kernel.begin_pass()
        kernel.render_rml(**draw)
        first = (kernel.common_resolves, kernel.lookups, len(kernel.set_calls))

        kernel.begin_pass()
        kernel.render_rml(**draw)
        self.assertEqual(kernel.common_resolves, first[0] + 1)
        self.assertEqual(kernel.lookups, first[1] + 2)
        self.assertEqual(len(kernel.set_calls), first[2] + 2)

    def test_patch_hunks_and_generated_contract(self):
        generated_root = ROOT / "third_party/aurora"
        hpp = (generated_root / "lib/rmlui/pipeline.hpp").read_text(encoding="utf-8")
        cpp = (generated_root / "lib/rmlui/pipeline.cpp").read_text(encoding="utf-8")
        common = (generated_root / "lib/gfx/common.cpp").read_text(encoding="utf-8")
        self.assertIn("struct RmlPassBindingState", hpp)
        self.assertIn("wgpu::BindGroup bindGroup", hpp)
        self.assertIn("RmlPassBindingState& state", hpp)
        self.assertIn("state.invalidate();", cpp)
        self.assertIn("common.valid ? common.ref : common_bind_group_ref()", cpp)
        self.assertIn("slot.dynamic == dynamic", cpp)
        self.assertIn("rmlui::RmlPassBindingState rmlState{}", common)
        self.assertEqual(common.count("rmlState.invalidate();"), 3)
        self.assertIn("rmlui::render(draw.rml, pass, rmlState)", common)
        self.assertIn("restore_non_rml_bindings", common)
        self.assertIn("rmlBindingsActive", common)
        self.assertIn("rmlBindingsActive = true", common)
        self.assertIn("rmlBindingsActive = false", common)

if __name__ == "__main__":
    unittest.main()
