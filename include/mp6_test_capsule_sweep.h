/* MP6 native port -- MP6_TEST_CAPSULE_SWEEP (src/os/test_capsule_sweep.c):
 * computer players are handed and use every listed capsule in turn, so tools
 * that record what the game draws reach every capsule effect. */
#ifndef MP6_TEST_CAPSULE_SWEEP_H
#define MP6_TEST_CAPSULE_SWEEP_H

/* The capsule player `playerNo` holds this turn, or -1 when the lever is off. */
int mp6_test_capsule_sweep_pick(int playerNo, int turnNo);
/* The inventory index the computer player uses (the swept capsule), or -2
 * when the lever is off and the game's own choice applies. */
int mp6_test_capsule_sweep_force(int playerNo, const int *capsuleTbl, int capsuleNum);

#endif
