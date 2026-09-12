/* The generated include contains the real capevent structs and grid setter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "game/hu3d.h"
#include "game/memory.h"

static HU3D_MODEL models[HU3D_MODEL_MAX];
HU3D_MODEL *Hu3DData = models;
static HuVec2f gridBuffer[128];
static int allocationBytes;

void *HuMemDirectMallocNum(HEAPID heap, s32 size, u32 num)
{
    if (heap != HEAP_MODEL || size > sizeof(gridBuffer) || num != 123) abort();
    allocationBytes = size;
    return gridBuffer;
}

#include "capsule_effect_subject.inc"

#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void)
{
    static const int cases[][3] = {{4, 1, 0}, {1, 4, 0}, {2, 3, 1}, {0, 0, 0}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CAPEFFPARTICLESYSTEMWORK work, expected;
        memset(&work, 0x5a, sizeof(work));
        work._unk4C = NULL;
        expected = work;
        Hu3DData[0].hookData = &work;
        Hu3DData[0].mallocNo = 123;
        ev_CapEffGridSet(0, cases[i][0], cases[i][1], cases[i][2]);
        printf("grid %d x %d: draw callback = 0x%llx\n", cases[i][0], cases[i][1],
            (unsigned long long)(uintptr_t)work._unk4C);
        CHECK(work._unk4C == NULL);
        expected.gridNum = (cases[i][0] > 0 ? cases[i][0] : 1)
                         * (cases[i][1] > 0 ? cases[i][1] : 1);
        expected.grid = gridBuffer;
        CHECK(memcmp(&work, &expected, sizeof(work)) == 0);
        CHECK(allocationBytes == expected.gridNum * 4 * sizeof(HuVec2f));
        CHECK(gridBuffer[0].x == 0.0f && gridBuffer[0].y == 0.0f);
        if (!cases[i][2]) {
            CHECK(gridBuffer[expected.gridNum * 4 - 2].x == 1.0f);
            CHECK(gridBuffer[expected.gridNum * 4 - 2].y == 1.0f);
        } else {
            CHECK(gridBuffer[expected.gridNum * 4 - 2].x == 1.5f);
            CHECK(gridBuffer[expected.gridNum * 4 - 2].y == 2.0f / 3.0f);
        }
    }
    puts("capsule animation grid: PASS");
    return 0;
}
