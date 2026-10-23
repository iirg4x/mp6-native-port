"""Execute TT Wars native pointer-table initialization and reload ownership."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_modes
from tests.test_native_modes import execute_reset_owners, function_body


ROOT = Path(__file__).resolve().parents[1]
MODULE = "ttwarsdll"


class NativeTTWars(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())
        rel = f"src/REL/{MODULE}/game.c"
        cls.original = (cls.decomp / rel).read_text(encoding="utf-8")
        cls.source = apply_patches.generated_source(cls.decomp, rel)

    def test_explicit_source_and_storage_ownership(self):
        self.assertEqual(native_modes.MODULES[MODULE], ("game", "prolog"))
        globals_ = re.findall(
            r"(?m)^\w[\w *]+(lbl_1_(?:bss|data)_\w+)(?:\[[^;=]*\])?\s*[;=]",
            self.original,
        )
        self.assertEqual(len(globals_), 45)
        for name in globals_:
            self.assertEqual(self.source.count(f"X({name})"), 1, name)
        execute_reset_owners(self, self.decomp, MODULE, ("game",), "game")

    def test_real_setup_clears_all_native_pointer_slots(self):
        setup = function_body(self.source, "fn_1_344")
        piece = function_body(self.source, "fn_1_25E0")
        record = re.search(r"typedef struct TTWarsObjectData_s.*?} TTWarsObjectData;",
                           self.source, re.S).group()
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            source = temporary / "table.c"
            executable = temporary / "table.exe"
            source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
typedef int s32;
typedef unsigned char u8;
typedef float f32;
typedef struct { float x, y, z; } Vec;
typedef short HU3D_MODELID;
typedef struct OMOBJ_s {
    void (*objFunc)(struct OMOBJ_s *);
    void *data;
    int work[4];
} OMOBJ;
typedef struct OMOBJMAN_s { int unused; } OMOBJMAN;
static OMOBJ sentinel;
static OMOBJ *lbl_1_bss_1464[256];
static int lbl_1_bss_8, lbl_1_bss_4;
static HU3D_MODELID lbl_1_bss_1454[8], lbl_1_bss_144E, lbl_1_bss_1444;
static HU3D_MODELID lbl_1_bss_1446[4];
static void *lbl_1_bss_1870;
enum { DATA_ttwars, HU_MEMNUM_OVL, HEAP_MODEL, HU3D_ATTR_DISPOFF, HU3D_ATTR_NONE };
#define DATANUM(d, n) (n)
static int modelCreates;
static void *HuDataSelHeapReadNum(int n, int owner, int heap) { return &sentinel; }
static HU3D_MODELID Hu3DModelCreate(void *data) { return (HU3D_MODELID)modelCreates++; }
static void Hu3DModelAttrSet(HU3D_MODELID id, unsigned attr) {}
static void Hu3DModelRotSet(HU3D_MODELID id, float x, float y, float z) {}
static void omMakeGroupEx(OMOBJMAN *manager, int group, int count) { assert(count == 256); }
static void *omGetGroupMemberListEx(OMOBJMAN *manager, int group) { return NULL; }
static void WipeCreate(int mode, int type, int frames) { assert(frames == 20); }
static void fn_1_5BC(OMOBJ *obj) {}
static OMOBJ *lbl_1_bss_1864 = &sentinel;
static int lbl_1_bss_142C, lbl_1_bss_141C[4];
static unsigned HuPadDStkRep[4], HuPadBtnDown[4];
enum { PAD_BUTTON_A = 256, PAD_BUTTON_Y = 2048 };
static int deleted, transformed;
static int fn_1_2E80(int column, int row) { return 0; }
static int fn_1_2F70(int column, int row) { return 0; }
static void fn_1_1DCC(void) {}
static void fn_1_23F4(OMOBJ *obj) { free(obj->data); obj->data = NULL; ++deleted; }
static void omSetTra(OMOBJ *obj, float x, float y, float z) {
    assert(obj->data != NULL); ++transformed;
}
''' + record + setup + piece + r'''
int main(void) {
    for (int visit = 0; visit < 2; ++visit) {
        for (int i = 0; i < 256; ++i) lbl_1_bss_1464[i] = &sentinel;
        OMOBJ setup = {0};
        fn_1_344(&setup);
        for (int i = 0; i < 256; ++i) assert(lbl_1_bss_1464[i] == NULL);
        assert(setup.objFunc == fn_1_5BC);
        assert(lbl_1_bss_8 == 8 && lbl_1_bss_4 == 6);
    }
    assert(modelCreates == 14);
    OMOBJ piece = {0};
    TTWarsObjectData *data = calloc(1, sizeof(*data));
    piece.data = data;
    data->state = 1;
    data->position.y = 90;
    fn_1_25E0(&piece);
    assert(data->position.y == 85 && transformed == 1 && deleted == 0);
    data->state = 3;
    fn_1_25E0(&piece);
    assert(piece.data == NULL && transformed == 1 && deleted == 1);
    return 0;
}
''', encoding="utf-8")
            result = subprocess.run([
                build.ZIG, "cc", "-O1", "-UNDEBUG", "-fsanitize=undefined",
                "-fno-sanitize-recover=all", str(source), "-o", str(executable),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
