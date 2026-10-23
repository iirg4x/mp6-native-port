"""The name index returns the same first match as the linear search it replaces."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp6_name_index.h"

typedef struct { int pad; const char *name; double more; } Item;

static int linear(const Item *items, int count, const char *name)
{
    int i;
    for (i = 0; i < count; i++)
        if (items[i].name && strcmp(items[i].name, name) == 0) return i;
    return -1;
}

int main(void)
{
    static char pool[4096][8];
    static Item items[3000];
    unsigned rng = 7;
    int round, checked = 0;
    for (round = 0; round < 200; round++) {
        int count = (int)(rng % 3000), i, q;
        Mp6NameIndex ix;
        for (i = 0; i < count; i++) {
            rng = rng * 1664525u + 1013904223u;
            /* few distinct names, so duplicates (first match) are common */
            snprintf(pool[i], sizeof(pool[i]), "n%u", (rng >> 8) % (unsigned)(count / 3 + 1));
            items[i].name = (rng & 31) == 0 ? NULL : pool[i];
        }
        mp6_name_index_build(&ix, items, sizeof(Item), offsetof(Item, name), count);
        assert(count < 8 ? ix.slot == NULL : ix.slot != NULL);
        for (q = 0; q < 400 && ix.slot; q++) {
            char name[8];
            rng = rng * 1664525u + 1013904223u;
            snprintf(name, sizeof(name), "n%u", (rng >> 8) % (unsigned)(count / 3 + 2));
            if (mp6_name_index_find(&ix, name) != linear(items, count, name)) {
                printf("mismatch round %d name %s\n", round, name);
                return 1;
            }
            checked++;
        }
        mp6_name_index_free(&ix);
        rng = rng * 1664525u + 1013904223u;
    }
    printf("checked %d\n", checked);
    return checked > 1000 ? 0 : 2;
}
'''


class NameIndex(unittest.TestCase):
    def test_first_match_equals_linear_search(self):
        with tempfile.TemporaryDirectory(prefix='name-index-', dir=ROOT / 'build') as temporary:
            folder = Path(temporary)
            (folder / 'test.c').write_text(HARNESS, encoding='utf-8')
            exe = folder / 'test.exe'
            result = subprocess.run([build.ZIG, 'cc', '-O2', '-UNDEBUG', '-I', str(ROOT / 'include'),
                                     str(folder / 'test.c'), str(ROOT / 'src/hsf/name_index.c'),
                                     '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
