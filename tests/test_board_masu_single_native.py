"""Native integration contracts for the recovered Masu/Single update."""
from pathlib import Path
import unittest

import tests.test_board_native_abi as native
from tests.test_frame_resource_lifetimes import function
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class BoardMasuSingleNative(unittest.TestCase):
    compile_run = native.BoardNativeABI.compile_run

    def test_masu_packed_fields_keep_byte_order_and_cursor_advancement(self):
        source = native.patched('src/board/masu.c')
        macros = source[source.index('#define DATA_READ16'):source.index('void mbMasuInit(')]
        self.compile_run(macros, 'board_masu_decode_selftest.c')
        self.compile_run(macros.replace('be16(ptr) + 1', 'be16(ptr)'),
                         'board_masu_decode_selftest.c', expect_success=False)
        self.compile_run(macros.replace('be32(ptr)', '*(u32 *)(ptr)'),
                         'board_masu_decode_selftest.c', expect_success=False)

    def test_single_flag_set_returns_recovered_index_and_checks_bounds(self):
        source = native.patched('src/game/gamework.c')
        constants = '\n'.join(line for line in source.splitlines()
                              if line.startswith(('#define GW_FLAG_WORD_BITS ',
                                                  '#define GW_FLAG_BIT_MASK ')))
        subject = constants + '\n' + '\n'.join(function(source, name) for name in
            ('GWSingleMgFlagSet', 'GWSingleMgFlagGet', 'GWMgCustomGet'))
        self.compile_run(subject, 'board_single_flags_selftest.c')
        self.compile_run(subject.replace('return mgNo;', 'return 0;'),
                         'board_single_flags_selftest.c', expect_success=False)

    def test_single_uses_recovered_particle_owner_and_real_minigame_call(self):
        source = native.patched('src/board/single.c')
        # The decomp source itself carries this; the port's patch (if any)
        # only replaces inline PowerPC asm and never the particle owner.
        single_patch = ROOT/'compat/decomp/src/board/single.c.patch'
        if single_patch.exists():
            self.assertNotIn('hookData', single_patch.read_text(encoding='utf-8'))
        particle = function(source, 'SingleParticleDataGet')
        self.assertIn('(MBPARTICLE *)Hu3DData[modelId].hookData', particle)
        init = function(source, 'SingleEffInit')
        self.assertEqual(init.count('particle->hookData = work;'), 2)
        self.assertNotRegex(init, r'Hu3DData\[[^\n]+\.hookData = work;')
        # Single must see the new integer-returning API through the real header.
        header = (Path(build.DECOMP)/'include/game/gamework.h').read_text()
        self.assertIn('int GWSingleMgFlagSet(int mgNo);', header)
        sources = [path for path, _ in build.board_sources()]
        self.assertIn('src/board/single.c', sources)
        self.assertIn('src/board/masu.c', sources)
        self.assertIn('src/board/mgcall.c', sources)
        mgcall = native.patched('src/board/mgcall.c')
        self.assertRegex(mgcall, r'\bvoid mbev_MgCallSingle\(int \w+\)')


if __name__ == '__main__':
    unittest.main()
