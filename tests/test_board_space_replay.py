"""Execute the patched board-space admission policy for moving cameras."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


def patched_masu():
    original = (Path(build.DECOMP) / 'src/board/masu.c').read_text(encoding='utf-8')
    patch = (ROOT / 'compat/decomp/src/board/masu.c.patch').read_text(encoding='utf-8')
    return apply_patches.apply_unified_diff(original, patch)


class BoardSpaceReplayTests(unittest.TestCase):
    def test_real_policy_keeps_paced_replay_membership_stable(self):
        source = patched_masu()
        policy = re.search(r'#ifndef MP6_HEADLESS_BUILD\n    mp6StableSpaceDraw =.*?#endif',
                           source, re.S).group()
        guards = re.findall(r'        if \(!mp6StableSpaceDraw && !mbCameraCullCheck.*?\n        }',
                            source, re.S)
        self.assertEqual(len(guards), 2, 'both spaces and capsule overlays need stable membership')
        for headless in (False, True):
            with self.subTest(headless=headless), tempfile.TemporaryDirectory(
                    prefix='space-replay-', dir=ROOT / 'build') as folder:
                folder = Path(folder)
                subject = r'''
#include <assert.h>
static int unlocked, paced, camera, cullCalls, policyCalls;
#ifndef MP6_HEADLESS_BUILD
static int mp6_unlocked_fps_enabled(void) { ++policyCalls; return unlocked; }
static int mp6_tick_interpolation_possible(void) { ++policyCalls; return paced; }
#endif
static int mbCameraCullCheck(const int *pos, float radius) {
    assert(radius == 200.0f); ++cullCalls;
    return (*pos + camera) % 3 != 0;
}
static unsigned emit(int capsule) {
    int mp6StableSpaceDraw = 0;
POLICY
    unsigned ids = 0;
    for (int i = 0; i < 12; ++i) {
        int pos = i;
        if (capsule) {
GUARD1
        } else {
GUARD0
        }
        ids |= 1u << i;
    }
    return ids;
}
int main(void) {
    for (unlocked=0; unlocked<2; ++unlocked)
    for (paced=0; paced<2; ++paced)
    for (int capsule=0; capsule<2; ++capsule)
    for (camera=0; camera<6; ++camera) {
        unsigned expected = 0;
        int stable = unlocked && paced;
#ifdef MP6_HEADLESS_BUILD
        stable = 0;
#endif
        for (int i=0; i<12; ++i)
            if (stable || (i+camera)%3 != 0) expected |= 1u << i;
        cullCalls = policyCalls = 0;
        assert(emit(capsule) == expected);
        assert(cullCalls == (stable ? 0 : 12));
#ifdef MP6_HEADLESS_BUILD
        assert(policyCalls == 0);
#else
        assert(policyCalls == (unlocked ? 2 : 1));
#endif
    }
}
'''.replace('POLICY', policy).replace('GUARD0', guards[0]).replace('GUARD1', guards[1])
                path, exe = folder / 'test.c', folder / 'test.exe'
                path.write_text(subject, encoding='utf-8')
                result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG',
                    *(['-DMP6_HEADLESS_BUILD'] if headless else []), str(path), '-o', str(exe)],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preserves_authored_depth_and_ao_brackets(self):
        source = patched_masu()
        self.assertIn('masuP->pos.y + 3.0f', source)
        self.assertIn('GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);', source)
        self.assertIn('GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);', source)
        self.assertEqual(source.count('mp6_ao_decals(Hu3DCameraNo, FALSE);'), 1)
        self.assertEqual(source.count('mp6_ao_decals(Hu3DCameraNo, TRUE);'), 1)
        self.assertEqual(source.count('mp6_unlocked_fps_enabled()'), 1)
        loops = source.split('mbMasuPosGet(i + 1, &pos);')[1:]
        self.assertEqual(len(loops), 2)
        for loop in loops:
            self.assertLess(loop.index('if (!mp6StableSpaceDraw'), loop.index('GXLoadPosMtxImm'))


if __name__ == '__main__':
    unittest.main()
