"""Real sprite matrix hook and narrowly opted-in Solo/save backing owners."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_widescreen_effects import patched, function

ROOT = Path(__file__).resolve().parents[1]


class SoloResultsWide(unittest.TestCase):
    def test_qa_uses_real_return_and_save_award_paths(self):
        from tests.integration.build_widescreen_qa import SOLO_RESULTS_SETUP, solo_results_probe
        for seed in ('mbSaveInit(6)', 'mbSaveStoryInit(1, 0, 1)',
                     'GwSystem.turnNo = 3', 'GWMgCoinBonusSet(0, 1)', 'SLSaveFlagSet(TRUE)'):
            self.assertIn(seed, SOLO_RESULTS_SETUP)
        self.assertNotIn('lbl_1_bss_', SOLO_RESULTS_SETUP)
        prefix = '#include <stdlib.h>\n#include "mp6_events.h"\n'
        for rel, bank, anchor, event, value in (
            ('src/REL/mdsingdll/mdsing.c', False, '    mdsingResultPanelOpen();',
             'result-ready', '0'),
            ('src/game/saveload.c', True, 'static HUWINID SLBankAwardExec(s16 award)\n{',
             'bank-ready', 'award'),
        ):
            original = patched(rel)
            probe = solo_results_probe(original, bank)
            observation = ('\n    if (getenv("MP6_WS_QA_SOLO_RESULTS"))\n'
                           f'        mp6_event_post("qa.mdsing.{event}", {value}, "ready");')
            self.assertEqual(probe, prefix+original.replace(anchor, anchor+observation, 1))
            self.assertEqual(probe[len(prefix):].replace(observation, ''), original)

    def test_backing_matrix_tracks_aspect_without_mutating_authored_state(self):
        source = patched('src/game/sprput.c')
        subject = function(source, 'mp6SprWideBackdrop')
        self.assertIn('MTXConcat(*sp->groupMtx, modelview, modelview);\n'
                      '        mp6SprWideBackdrop(modelview, sp->attr);\n'
                      '        GXLoadPosMtxImm(modelview, GX_PNMTX0);', source)
        header = (ROOT/'include/mp6_widescreen.h').read_text()
        attribute = re.search(r'^#define MP6_HUSPR_ATTR_WIDE_BACKDROP .+$', header, re.M).group()
        native_header = (Path(apply_patches.require_pinned_decomp())/'include/game/sprite.h').read_text()
        bits = [int(x, 16) for x in re.findall(r'^#define HUSPR_ATTR_\w+ (0x[0-9A-Fa-f]+)',
                                               native_header, re.M)]
        self.assertEqual(sum(bits) & 0x1000, 0)
        fixture = (ROOT/'tests/native/widescreen_sprite_backdrop_selftest.c').read_text()
        fixture = fixture.replace('/* WIDE_BACKDROP_ATTRIBUTE */', attribute)
        with tempfile.TemporaryDirectory(prefix='solo-backing-', dir=ROOT/'build') as tmp:
            directory = Path(tmp)
            (directory/'backdrop_subject.inc').write_text(subject)
            (directory/'test.c').write_text(fixture)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-UNDEBUG', '-I'+tmp,
                                     str(directory/'test.c'), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_only_flat_tints_are_tagged_not_the_panel_or_star_bank_art(self):
        solo = patched('src/REL/mdsingdll/mdsing.c')
        # Both ordinary and board-return initializers own their dimmer.
        marker = ('HuSprGrpScaleSet(lbl_1_bss_474, 40.0f, 40.0f);\n'
                  '    HuSprAttrSet(lbl_1_bss_474, 0, MP6_HUSPR_ATTR_WIDE_BACKDROP);')
        self.assertEqual(solo.count(marker), 2)
        self.assertEqual(solo.count('if (spriteDesc->groupNo == 8)\n'
                                   '            HuSprAttrSet(lbl_1_bss_1292[8], 0, '
                                   'MP6_HUSPR_ATTR_WIDE_BACKDROP);'), 2)
        save = patched('src/game/saveload.c')
        self.assertEqual(save.count('espScaleSet(backdropSpriteId, 10, 10);\n'
                                    '        espAttrSet(backdropSpriteId, '
                                    'MP6_HUSPR_ATTR_WIDE_BACKDROP);'), 4)
        self.assertNotIn('espAttrSet(backgroundSpriteId, MP6_HUSPR_ATTR_WIDE_BACKDROP)', save)


if __name__ == '__main__':
    unittest.main()
