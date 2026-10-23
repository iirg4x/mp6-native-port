"""Run the real Win32 low-address allocator with fragmented address space."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from tools import build

ROOT = Path(__file__).resolve().parents[1]


class LowMemoryStartupOrder(unittest.TestCase):
    def test_claims_address_space_before_renderer_or_launcher(self):
        source = (ROOT / 'src/main_native.c').read_text()
        start = source.index('int main(int argc, char **argv) /* expands to aurora_main')
        body = source[start:]
        self.assertLess(body.index('mp6_prepare_low_memory();'), body.index('mp6_launcher_decide_mode('))
        self.assertLess(body.index('mp6_prepare_low_memory();'), body.index('aurora_initialize('))


@unittest.skipUnless(os.name == 'nt', 'Windows virtual-memory contract')
class WindowsLowMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='low-memory-', dir=ROOT / 'build')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = (ROOT / 'src/host/host_win32.c').read_text()
        subject = source[source.index('static void *g_preparedGameArena;'):
                         source.index('int mp6_host_image_below_4gb(void)')]
        fixture = directory / 'low-memory.c'
        fixture.write_text('''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "host.h"
#include "mp6_heap_scale.h"
#include "mp6_coro_layout.h"
''' + subject + '''
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed line %d: %s\\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    MEMORY_BASIC_INFORMATION region;
    if (argc > 1 && argv[1][0] == 'f') {
        const uintptr_t bases[] = {0x80000000u,0x90000000u,0xA0000000u,0xB0000000u,
            0x40000000u,0x50000000u,0x60000000u,0x70000000u,0x20000000u,0x30000000u,MP6_CORO_POOL_BASE};
        void *blocks[sizeof(bases)/sizeof(bases[0])] = {0};
        for (size_t i=0; i<sizeof(blocks)/sizeof(blocks[0]); ++i)
            blocks[i] = VirtualAlloc((void *)bases[i], 65536, MEM_RESERVE, PAGE_NOACCESS);
        void *pool = mp6_host_arena_reserve(MP6_CORO_POOL_BASE, MP6_CORO_POOL_SIZE);
        CHECK(pool && mp6_host_range_below_4gb(pool, MP6_CORO_POOL_SIZE));
        for (size_t i=0; i<sizeof(blocks)/sizeof(blocks[0]); ++i) {
            CHECK(pool != (void *)bases[i]);
            if (blocks[i]) {
                CHECK(VirtualQuery(blocks[i], &region, sizeof(region)));
                CHECK(region.AllocationBase == blocks[i] && region.State == MEM_RESERVE);
                VirtualFree(blocks[i], 0, MEM_RELEASE);
            }
        }
        VirtualFree(pool, 0, MEM_RELEASE);
        return 0;
    }
    CHECK(mp6_host_memory_prepare());
    void *game = g_preparedGameArena;
    void *pool = g_preparedCoroPool;
    CHECK(mp6_host_memory_prepare());
    CHECK(game == g_preparedGameArena && pool == g_preparedCoroPool);
    CHECK(VirtualQuery(game, &region, sizeof(region)));
    CHECK(region.State == MEM_RESERVE && region.RegionSize >= MP6_ARENA_EXPANDED_BYTES);
    CHECK(VirtualQuery(pool, &region, sizeof(region)) && region.State == MEM_RESERVE);
    CHECK(!mp6_host_arena_reserve(0, (size_t)MP6_ARENA_EXPANDED_BYTES + 1));
    CHECK(!mp6_host_arena_reserve(MP6_CORO_POOL_BASE, MP6_CORO_POOL_SIZE + 1));
    CHECK(!mp6_host_arena_reserve(0, 0));
    const size_t bytes = argc > 1 && argv[1][0] == 'e' ? MP6_ARENA_EXPANDED_BYTES : MP6_ARENA_RETAIL_BYTES;
    CHECK(mp6_host_arena_reserve(0, bytes) == game);
    CHECK(VirtualQuery(game, &region, sizeof(region)) && region.State == MEM_COMMIT);
    if (bytes == MP6_ARENA_RETAIL_BYTES) {
        CHECK(VirtualQuery((char *)game + bytes, &region, sizeof(region)));
        CHECK(region.State == MEM_RESERVE);
    }
    CHECK(mp6_host_arena_reserve(MP6_CORO_POOL_BASE, MP6_CORO_POOL_SIZE) == pool);
    CHECK(mp6_host_range_below_4gb(game, bytes));
    CHECK(mp6_host_range_below_4gb(pool, MP6_CORO_POOL_SIZE));
    CHECK((uintptr_t)game + bytes <= (uintptr_t)pool || (uintptr_t)pool + MP6_CORO_POOL_SIZE <= (uintptr_t)game);
    ((char *)game)[bytes-1] = 1;
    ((char *)pool)[MP6_CORO_POOL_SIZE-1] = 2;
    VirtualFree(game, 0, MEM_RELEASE);
    VirtualFree(pool, 0, MEM_RELEASE);
    return 0;
}
''', encoding='utf-8')
        cls.exe = directory / 'low-memory.exe'
        result = subprocess.run([build.ZIG, 'cc', '-O2', '-I', str(ROOT / 'include'),
                                 '-I', str(ROOT / 'src/host'), str(fixture), '-o', str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_reserve_then_commit_only_selected_heap_size(self):
        for mode in ('retail', 'expanded'):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_free_holes_are_found_when_all_fixed_candidates_are_occupied(self):
        result = subprocess.run([str(self.exe), 'fragmented'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
