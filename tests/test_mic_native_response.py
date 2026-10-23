"""Exercise the shared controller/microphone queue at native pointer width."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class NativeMicResponse(unittest.TestCase):
    def test_queue_stride_and_last_slot(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        rel = 'src/game/mic.c'
        source = apply_patches.apply_unified_diff((decomp / rel).read_text(encoding='utf-8'),
            (ROOT / 'compat/decomp' / (rel + '.patch')).read_text(encoding='utf-8'), rel)
        consumer_rel = 'src/REL/m667dll/map_players_effects.c'
        consumer = apply_patches.apply_unified_diff((decomp / consumer_rel).read_text(encoding='utf-8'),
            (ROOT / 'compat/decomp' / (consumer_rel + '.patch')).read_text(encoding='utf-8'), consumer_rel)
        subject = '#include <assert.h>\n#include <stdlib.h>\n#include <string.h>\n#include "dolphin.h"\n'
        subject += '#include "REL/m667dll/recovered.h"\nstatic M667SceneView scene;\nM667SceneView *lbl_1_bss_2C = &scene;\n'
        subject += re.search(r'typedef struct M667MicResponse \{[^}]+\} M667MicResponse;', consumer).group()
        subject += '\n' + function_definition_body(consumer, 'fn_1_1FB4') + '\n'
        for name in ('MIC_ALLOC_TAG', 'MIC_RESPONSE_CAPACITY', 'MIC_SCORE_UNAVAILABLE'):
            subject += re.search(r'^#define ' + name + r' .+$', source, re.M).group() + '\n'
        for name in ('MCResponseEntry_s', 'MCResponse_s', 'MicResponseEntry_s'):
            subject += re.search(r'typedef struct ' + name + r' \{[^}]+\} ' + name + ';', source).group() + '\n'
        for name in ('MCResponseBuf', 'MCCurResponse', 'MCUnkResponseData'):
            subject += re.search(r'^static [^;\n]*\b' + name + r'(?:\[[^]]+\])?;', source, re.M).group() + '\n'
        subject += '''
static s32 MCResponseNo, MCResponseLastNo, MCStat, MicOpenF;
static OSMessageQueue MCMessageQueue;
static OSThreadQueue MCThreadQueue;
static int HuMCSelWinCheck(void) { return 1; }
static void HuMCSelModeSet(int mode) { assert(mode == 2); }
static int HuMCMicSaveGet(void) { return 2; }
BOOL OSSendMessage(OSMessageQueue *q, OSMessage m, s32 f) { __builtin_trap(); }
void OSSleepThread(OSThreadQueue *q) { __builtin_trap(); }
static size_t requested;
static unsigned char *guarded;
void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 tag) {
    assert(heap == 0 && tag == MIC_ALLOC_TAG);
    requested = size; guarded = malloc(size + 32); assert(guarded);
    memset(guarded, 0xA5, size + 32); return guarded + 16;
}
'''
        for name in ('MicWriteResponse', 'MicWriteResponseBuf', 'HuMCCurResponseGet',
                     'HuMCResponseGet2', 'HuMCUnkResponseCheck'):
            subject += '\n' + function_definition_body(source, name)
        allocation = re.search(r'MCResponseBuf = HuMemDirectMallocNum\([^;]+;', source).group()
        subject += '\nint main(void) {\n' + allocation + '''
    assert(requested == MIC_RESPONSE_CAPACITY * sizeof(*MCResponseBuf));
    short values[256];
    /* Multiple reads must consume precisely the same typed slots the writers produced. */
    for (int i = 0; i < 100; ++i) {
        values[i] = i;
        MicWriteResponse(0, 1, &values[i], 4000 + i, i);
    }
    for (int i = 0; i < 100; ++i) {
        assert(HuMCResponseGet2() == i);
        MCResponse_s *r = (MCResponse_s *)HuMCCurResponseGet();
        assert(r->status == 0 && r->score == 4000 + i && r->count == 1);
        assert(r->result == &values[i] && r->word == i);
        assert(r->entry[0].result == &values[i]);
    }
    MCResponse_s *empty = (MCResponse_s *)HuMCCurResponseGet();
    assert(empty->status == -1 && empty->count == 0 && empty->result == NULL && empty->word == -1);
    /* Test the final legal slot and repeated capped writes without overrunning its owner. */
    MCResponseNo = MIC_RESPONSE_CAPACITY - 1;
    for (int i = 0; i < 3; ++i) MicWriteResponse(0, 1, &values[0], 5000, 17);
    assert(MCResponseNo == 255 && MCResponseBuf[255].word == 17);
    for (int i = 0; i < 16; ++i) assert(guarded[i] == 0xA5 && guarded[requested + 16 + i] == 0xA5);
    /* Full ten-candidate copies and the ambiguity scratch owner use native records too. */
    MCResponseEntry_s entries[10];
    for (int i = 0; i < 10; ++i) entries[i] = (MCResponseEntry_s){5000-i*100, 1, &values[i]};
    MCResponseNo = MCResponseLastNo = 0;
    MicWriteResponseBuf(0, (u16 *)entries);
    assert(HuMCUnkResponseCheck() == 1);
    assert(memcmp(MCUnkResponseData.entry, entries, sizeof(entries)) == 0);
    MCResponse_s *r = (MCResponse_s *)HuMCCurResponseGet();
    assert(memcmp(r->entry, entries, sizeof(entries)) == 0);
    /* The actual minigame consumer receives full-width result pointers. */
    MCResponseNo = MCResponseLastNo = 0;
    for (int direction = 0; direction < 4; ++direction) {
        MicWriteResponse(0, 1, &values[direction], 10000, direction + 1);
        fn_1_1FB4(HuMCCurResponseGet());
        assert((scene.controlBits == (int[]){8,4,2,1}[direction]));
    }
    free(guarded);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            src, exe = directory / 'mic.c', directory / 'mic.exe'
            src.write_text(subject, encoding='utf-8')
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'), *build.COMMON_FLAGS,
                '-O1', '-UNDEBUG', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(src), '-o', str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
