"""Every REL reset list names exactly the mutable file-scope storage clang sees."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apply_patches  # noqa: E402
import gen_rel_state  # noqa: E402
import host_cpus  # noqa: E402


class RelStateLists(unittest.TestCase):
    def test_lists_match_compiler_view(self):
        apply_patches.require_pinned_decomp()
        apply_patches.apply_all()
        checked, problems = gen_rel_state.differences(max(1, min(6, host_cpus.allowed_cpu_count() // 2)))
        self.assertGreater(checked, 150)
        self.assertEqual(problems, [], 'run tools/gen_rel_state.py --check; '
                         'a variable added to a REL source must join its reset list')

    def test_const_detection_follows_the_object_not_the_pointee(self):
        own_const = gen_rel_state._own_type_is_const
        self.assertTrue(own_const('const s16'))
        self.assertTrue(own_const('const char *const[3]'))
        self.assertTrue(own_const('const Vec[4]'))
        self.assertFalse(own_const('const char *[3]'))
        self.assertFalse(own_const('void (*[4])(M676ParticleRecord *)'))
        self.assertTrue(own_const('void (*const[4])(void)'))
        self.assertFalse(own_const('s32'))


if __name__ == '__main__':
    unittest.main()
