"""Struct views and byte offsets agree with the GameCube layout natively.

tools/layout_audit.py finds partial struct copies whose opaque pads stood in
for pointer fields, and GameCube byte offsets applied to structs whose
fields moved once pointers became 8 bytes (or BOOL became 1 byte on
Android). Both corrupted memory: the battle-results sparkles overwrote the
particle renderer's display list, and E. Gadd's Garage's capsule station
wrote its shot list over the wrong board-work fields.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import apply_patches  # noqa: E402
import host_cpus  # noqa: E402
import layout_audit  # noqa: E402

DUMP = '''
*** Dumping AST Record Layout
         0 | struct Owner
         0 |   s16 state
         2 |   s16 group
         4 |   s32 count
         8 |   void * anim
        16 |   void * patternCoords
        24 |   Particle * particles
        32 |   union (anonymous at owner.c:3:5)
        32 |     s32 a
           | [sizeof=40, align=8]
'''


def record(name, unit, fields):
    return {'name': name, 'size32': 0, 'size64': 0, 'fields': fields, 'units': {unit}}


class LayoutAudit(unittest.TestCase):
    def test_parses_top_level_fields_and_anonymous_members(self):
        size, fields = layout_audit.parse_layouts(DUMP)['struct Owner']
        self.assertEqual(size, 40)
        self.assertEqual(fields[4], ('patternCoords', 'void *', 16))
        self.assertEqual(fields[6][0], '')  # the anonymous union has no name
        self.assertEqual(len(fields), 7)  # nested members are not top-level fields

    def test_pad_standing_in_for_pointers_is_reported(self):
        # The battle-results shape: same leading fields, then 8 opaque bytes
        # where the owner has two pointers, then the shared `particles`.
        unit = 'src/REL/resultDll/a.c'
        owner = record('struct Owner', unit, [
            ('state', 's16', 0, 0), ('group', 's16', 2, 2), ('count', 's32', 4, 4),
            ('anim', 'void *', 8, 8), ('patternCoords', 'void *', 12, 16), ('particles', 'P *', 16, 24)])
        view = record('struct View', unit, [
            ('state', 's16', 0, 0), ('group', 's16', 2, 2), ('count', 's32', 4, 4),
            ('settings', 'u8[8]', 8, 8), ('particles', 'P *', 16, 16)])
        found = layout_audit.mismatches({'o': owner, 'v': view})
        self.assertEqual([(f[2], f[3], f[4], f[6]) for f in found], [('particles', 16, 24, 16)])

    def test_tail_view_behind_one_long_pad_is_reported(self):
        # E. Gadd's Garage's capsule cannon: `u8 earlierState[4780]` stood
        # in for the board work's head, pointers included, and its three
        # fields are spelled differently from the owner's.
        unit = 'src/REL/w02Dll/a.c'
        owner = record('struct PositionModel', unit, [
            ('model', 's32', 0, 0), ('object', 'OMOBJ *', 4, 8), ('throwPathIndex', 'int', 4780, 4832),
            ('throwPathCount', 'int', 4784, 4836), ('throwPaths', 'ThrowPath *', 4788, 4840)])
        view = record('struct ThrowPathWork', unit, [
            ('earlierState', 'u8[4780]', 0, 0), ('pathIndex', 's32', 4780, 4780),
            ('pathCount', 's32', 4784, 4784), ('paths', 'throwpathsThrowPath *', 4788, 4792)])
        found = layout_audit.mismatches({'o': owner, 'v': view})
        self.assertEqual(sorted({f[3] for f in found}), [4780, 4784, 4788])

    def test_pad_over_plain_fields_is_not_reported(self):
        unit = 'src/REL/m600/a.c'
        owner = record('struct Owner', unit, [
            ('a', 's32', 0, 0), ('b', 's32', 4, 4), ('c', 's32', 8, 8), ('x', 'f32', 12, 12), ('y', 'f32', 16, 16)])
        view = record('struct View', unit, [
            ('a', 's32', 0, 0), ('b', 's32', 4, 4), ('c', 's32', 8, 8), ('skip', 'u8[4]', 12, 12),
            ('y', 'f32', 16, 16)])
        self.assertEqual(layout_audit.mismatches({'o': owner, 'v': view}), [])

    def test_gamecube_byte_offset_past_a_pointer_is_reported(self):
        unit = 'src/REL/w02Dll/a.c'
        board = record('struct Board', unit, [
            ('model', 's32', 0, 0), ('backup', 'void *', 4, 8), ('display', 's32', 8, 16)])
        found = layout_audit.moved_offsets({'b': board}, [(unit, 12, 'struct Board', 8)])
        self.assertEqual([(f[4], f[5]) for f in found], [('display', 16)])
        self.assertEqual(layout_audit.moved_offsets({'b': board}, [(unit, 12, 'struct Board', 0)]), [])

    def test_every_recovered_source_agrees_with_the_gamecube_layout(self):
        apply_patches.require_pinned_decomp()
        apply_patches.apply_all()
        jobs = max(1, min(6, host_cpus.allowed_cpu_count() // 2))
        by_target, failures, offsets = layout_audit.collect(jobs)
        self.assertEqual(failures, [], 'sources the audit could not compile go unchecked')
        self.assertGreater(len(offsets), 20)
        for target, records in by_target.items():
            with self.subTest(target=target):
                self.assertGreater(len(records), 1000)
                self.assertEqual(layout_audit.mismatches(records), [],
                                 'run tools/layout_audit.py: a struct view skips pointer fields '
                                 'with a fixed-size pad')
                self.assertEqual(layout_audit.moved_offsets(records, offsets), [],
                                 'run tools/layout_audit.py: a GameCube byte offset reaches a moved field')


if __name__ == '__main__':
    unittest.main()
