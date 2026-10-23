/* MP6 native port -- MP6_TEST_CAPSULE_SWEEP, a test lever for tools that must
 * see every capsule (orb) effect, such as the shader seed recorder
 * (tools/generate_shader_seed.py): many capsules load their own models and
 * effects, and some change how the players are drawn (the Metal Mushroom),
 * which a computer party only reaches by chance.
 *
 * MP6_TEST_CAPSULE_SWEEP=<offset>: before each computer player rolls, it is
 * handed one capsule and uses it. Player p on turn t gets the
 * (offset + 4 (t - 1) + p)-th capsule of the shop list, wrapping, so a
 * 20-turn party uses every listed capsule at least once and successive
 * offsets put each capsule on a different player. Unset: no effect. */
#include "dolphin.h"
#include "mp6_test_capsule_sweep.h"

#include <stdio.h>
#include <stdlib.h>

#define MP6_CAPSULE_COUNT 53 /* board/capsule.c's CAPSULE_MAX */

BOOL mbCapListExcludeCheck(s16 capsuleNo);

static int sweep_offset(void)
{
    static int offset = -2; /* -2: not read yet, -1: off */
    if (offset == -2) {
        const char *env = getenv("MP6_TEST_CAPSULE_SWEEP");
        offset = env && *env ? abs(atoi(env)) : -1;
    }
    return offset;
}

int mp6_test_capsule_sweep_pick(int playerNo, int turnNo)
{
    int listed[MP6_CAPSULE_COUNT];
    int count = 0;
    const int offset = sweep_offset();
    if (offset < 0) {
        return -1;
    }
    for (int capsule = 0; capsule < MP6_CAPSULE_COUNT; ++capsule) {
        /* listFlag: the capsule appears in the shop and capsule machines,
         * i.e. a player can hold it. */
        if (mbCapListExcludeCheck((s16)capsule)) {
            listed[count++] = capsule;
        }
    }
    if (count == 0) {
        return -1;
    }
    const int index = offset + 4 * (turnNo > 0 ? turnNo - 1 : 0) + playerNo;
    const int capsule = listed[index % count];
    printf("[CAPSULE-SWEEP] turn %d player %d holds capsule %d (%d listed)\n", turnNo, playerNo, capsule, count);
    fflush(stdout);
    return capsule;
}

int mp6_test_capsule_sweep_force(int playerNo, const int *capsuleTbl, int capsuleNum)
{
    if (sweep_offset() < 0) {
        return -2;
    }
    (void)playerNo;
    /* The swept capsule is the only one held (mp6_test_capsule_sweep_pick
     * replaced the inventory): use it. */
    return capsuleNum > 0 && capsuleTbl[0] >= 0 ? 0 : -2;
}
