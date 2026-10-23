"""Title backdrop widens proportionally instead of mirroring its art."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TitleBackdropWide(unittest.TestCase):
    def test_title_backdrops_scale_proportionally_without_mirror_folds(self):
        patch = (ROOT / 'compat/decomp/src/REL/bootDll/boot.c.patch').read_text(encoding='utf-8')
        added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+'))
        self.assertNotIn('mp6_widescreen_extend_mirror(TitleMdlId[2]', added)
        self.assertIn('mp6_widescreen_scale_backdrop(TitleMdlId[2], "grid5", 0);', added)
        self.assertIn('mp6_widescreen_scale_backdrop(TitleMdlId[2], "grid14", -1);', added)

    def test_uniform_entries_scale_both_axes_about_their_own_pivot(self):
        source = (ROOT / 'src/hsf/mp6_widescreen_extrude.c').read_text(encoding='utf-8')
        body = source[source.index('void mp6_widescreen_scale_backdrop('):]
        body = body[:body.index('\n}\n')]
        # The pivot is measured from the native snapshot, never widened data.
        self.assertIn('PSMTXMultVec(e->fwdMtx, &((HuVecF *)e->native)[j], &world);', body)
        self.assertIn('e->center.x = (left + right) * 0.5f;', body)
        self.assertIn('e->pivotY = anchor < 0 ? low : anchor > 0 ? high : (low + high) * 0.5f;', body)
        apply = source[source.index('static void mp6_ws_selective_apply('):]
        apply = apply[:apply.index('\n}\n')]
        # 4:3 restores the exact native snapshot before any scaling.
        self.assertLess(apply.index('if (k == 1.0f) {'), apply.index('float cover = k * 1.01f;'))
        self.assertRegex(apply, r'grown\.y = e->pivotY \+ cover \* \(world\.y - e->pivotY\);')
        self.assertIn('if (!e->uniform) grown.y = world.y;', apply)


if __name__ == '__main__':
    unittest.main()
