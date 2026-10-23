/* MP6 native port -- gameplay state trace (MP6_GAMEPLAY_TRACE=1).
 *
 * While a minigame overlay runs, prints one "[GPLAY]" line per game tick
 * with hashes of the state that decides how the game plays out: the shared
 * random stream, the collision world (src/game/colman.c), every live object
 * and model transform, and the players' records. Two runs that must play
 * the same -- the same seeded, scripted minigame at 4:3 and at 21:9, say --
 * print identical lines; tools/gameplay_view_check.py compares them.
 *
 *   MP6_GAMEPLAY_TRACE=1             the per-tick lines
 *   MP6_GAMEPLAY_TRACE=2             the same, and exit when the minigame ends
 *   MP6_GAMEPLAY_TRACE_MODELS=<t>    also print every model's transform at
 *                                    overlay tick t (to name what diverged)
 *
 * Off by default; one cached flag test per tick when unset.
 */
#ifndef MP6_GAMEPLAY_TRACE_H
#define MP6_GAMEPLAY_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per game tick, after the tick's game work. */
void mp6_gameplay_trace_tick(void);

/* game/frand.c (frand.c.patch): read and pin the shared random stream. */
uint32_t mp6_frand_seed_get(void);
void mp6_frand_seed_set(uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
