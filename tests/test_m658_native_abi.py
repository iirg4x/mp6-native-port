"""Execute T Minus Five's real pool and AI boundaries with native-width records."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_inst_integration import function_definition_body

ROOT = Path(__file__).resolve().parents[1]


class M658NativeAbi(unittest.TestCase):
    def test_registry_capacity_pointer_payloads_and_ai_fields(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            patched = directory / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            game = (patched / 'src/REL/m658dll/gameplay.c').read_text()
            math = (patched / 'src/REL/m658dll/math_camera.c').read_text()
            descriptors = re.search(r'M658RegistryDescriptor lbl_1_data_494\[7\] = \{[\s\S]*?\n\};', game)[0]
            source = '''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "REL/m658dll/recovered.h"
#include "game/memory.h"
static unsigned char *allocations[6];
static size_t lengths[6];
static int allocated, created, tickInstalled, aiState;
static char attachment[] = "native pointer payload";
void *HuMemDirectMallocNum(HEAPID heap, s32 bytes, u32 number) {
    assert(allocated < 6);
    unsigned char *p = malloc(bytes + 32);
    assert(p); memset(p, 0xAB, bytes + 32);
    lengths[allocated] = bytes; allocations[allocated++] = p;
    return p + 16;
}
void fn_1_55FC(OMOBJ *object) { __builtin_trap(); }
void fn_1_140(OMOBJ *object, OMOBJ_FUNC callback) {
    assert(callback == fn_1_55FC); ++tickInstalled;
}
typedef struct M658RegistryTemplate { u32 initialWords[16]; } M658RegistryTemplate;
static void create_item(void *p, s32 event) {
    assert(event == 0);
    M658HookActor *item = p;
    item->hook = attachment;
    ++created;
}
'''
            for callback in ('2000', '2188', '2310', '24F0', '260C', '2728'):
                source += f'void fn_1_{callback}(void *p, s32 event) {{ create_item(p, event); }}\n'
            source += descriptors + '\n'
            for name in ('fn_1_65E8', 'fn_1_6680', 'fn_1_6764', 'fn_1_67A0'):
                source += function_definition_body(math, name) + '\n'
            source += function_definition_body(game, 'fn_1_5430') + '\n'
            source += '''
void fn_1_506C(OMOBJ *object) { aiState = 0; }
void fn_1_533C(void *object) { aiState = 1; }
void fn_1_5370(OMOBJ *object) { aiState = 2; }
'''
            source += function_definition_body(game, 'fn_1_4FCC') + '\n'
            source += function_definition_body(math, 'fn_1_69C8') + '\n'
            source += '''
int main(void) {
    M658RegistryWork registry; memset(&registry, 0xAB, sizeof(registry));
    OMOBJ object = {0}; object.data = &registry;
    fn_1_5430(&object);
    assert(allocated == 6 && created == 9 && tickInstalled == 1);
    assert(registry.entries[6].update == NULL);
    for (int i=0; i<6; ++i) {
        const M658RegistryDescriptor *d = &lbl_1_data_494[i];
        assert(d->itemBytes >= sizeof(M658HookActor));
        assert(lengths[i] == sizeof(M658Pool) + d->count * (sizeof(M658PoolNode) + d->itemBytes));
        assert(fn_1_6680(registry.entries[i].pool) == NULL);
        int count = 0;
        for (M658HookActor *item=fn_1_6764(registry.entries[i].pool); item;
             item=fn_1_67A0(registry.entries[i].pool, item)) {
            assert((uintptr_t)item % _Alignof(M658HookActor) == 0);
            assert(item->hook == attachment); ++count;
        }
        assert(count == d->count);
        for (int n=0; n<16; ++n) {
            assert(allocations[i][n] == 0xAB);
            assert(allocations[i][16 + lengths[i] + n] == 0xAB);
        }
        free(allocations[i]);
    }
    for (int state=0; state<4; ++state) {
        M658PlayerWork work, expected;
        memset(&work, 0, sizeof(work));
        work.player = (void *)&object; work.materialColors = attachment;
        work.inputState = state; work.inputDelayFrames = 9; work.elapsedInputFrames = 100;
        expected = work; expected.inputDelayFrames = 8; expected.elapsedInputFrames = 101;
        object.data = &work; aiState = -1;
        fn_1_4FCC(&object);
        assert(aiState == (state < 3 ? state : -1));
        assert(memcmp(&work, &expected, sizeof(work)) == 0);
    }
    Vec start={1,2,3}, end={5,10,15}, out={0};
    fn_1_69C8(&start, &end, &out, 0.25f);
    assert(out.x == 2 && out.y == 4 && out.z == 6);
    return 0;
}
'''
            subject, exe = directory / 'abi.c', directory / 'abi.exe'
            subject.write_text(source)
            result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                *build.COMMON_FLAGS, '-O1', '-UNDEBUG', '-fsanitize=undefined',
                '-fno-sanitize-recover=all', '-Werror=implicit-function-declaration',
                '-Werror=incompatible-pointer-types', '-Werror=int-conversion',
                str(subject), '-o', str(exe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
