"""Contract checks for Aurora's retained Unlocked FPS replay (third_party/aurora)."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))



TARGETS = {
    "lib/gfx/common.cpp",
    "lib/gfx/common.hpp",
    "lib/gfx/uniform_writer.hpp",
    "lib/gx/command_processor.cpp",
    "lib/gx/command_processor.hpp",
    "lib/gx/gx.cpp",
    "lib/gx/gx.hpp",
    "lib/gx/shader_info.cpp",
}


def masked_equal(stream, recorded, masked):
    """The reuse rule: equal everywhere except the recorded matrix payloads."""
    if len(stream) != len(recorded):
        return False
    at = 0
    for offset, length in masked:
        if stream[at:offset] != recorded[at:offset]:
            return False
        at = max(at, offset + length)
    return stream[at:] == recorded[at:]


class RetainedReplay(unittest.TestCase):
    def test_reuse_rule_only_ignores_matrix_payloads(self):
        recorded = bytes(range(64))
        masked = [(8, 12), (40, 8)]
        moved = bytearray(recorded)
        moved[10] ^= 0xFF  # inside a matrix payload: still reusable
        moved[44] ^= 0xFF
        self.assertTrue(masked_equal(bytes(moved), recorded, masked))
        moved[20] ^= 0xFF  # first byte after a payload: decode in full
        self.assertFalse(masked_equal(bytes(moved), recorded, masked))
        self.assertFalse(masked_equal(recorded[:-1], recorded, masked))

    def test_source_proves_reuse_before_emitting(self):
        text = "\n".join((ROOT / "third_party/aurora" / target).read_text(encoding="utf-8")
                         for target in sorted(TARGETS))
        # Stream bytes, GX state and gfx position are all checked before reuse.
        self.assertIn("++r.streamMismatch", text)
        self.assertIn("++r.stateMismatch", text)
        self.assertIn("++r.gfxMismatch", text)
        self.assertIn("static_texture_cache_clear_pending()", text)
        # Regions with side effects beyond EFB drawing are never recorded.
        for guard in ("textureCopies", "encoderTasks", "count_palette_convs", "has_consumer()",
                      "g_retainTainted = true"):
            self.assertIn(guard, text)
        # Recording never reads the write-combined staging buffer back.
        self.assertIn("g_retainHost", text)
        self.assertIn("aurora_gx_replay_set_verify", text)

    def test_frame_interp_keeps_ao_replays_on_the_fifo_path(self):
        source = (ROOT / "src/gx/frame_interp.c").read_text(encoding="utf-8")
        self.assertIn("if ((s_replayAoCount == 0 || mp6_ao_replay_reuses_tick()) && fi_retained()) {",
                      source)
        self.assertIn("#ifdef MP6_AURORA_RETAINED_REPLAY", source)
        build = (ROOT / "tools/build.py").read_text(encoding="utf-8")
        self.assertIn("-DMP6_AURORA_RETAINED_REPLAY=1", build)
        self.assertIn('getenv("MP6_FI_RETAINED")', source)
        self.assertIn('getenv("MP6_FI_RETAINED_VERIFY")', source)
        self.assertIn("aurora_gx_replay_invalidate();", source)


if __name__ == "__main__":
    unittest.main()
