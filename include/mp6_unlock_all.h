/* MP6 native port -- the "Unlock All" mod (Settings > Extras).
 *
 * When on, the game behaves as if every unlock were owned: all minigames
 * (GWMgUnlockGet), every Star Bank purchase -- the bank characters,
 * Clockwork Castle, mic activities, Miracle Book pages, sound test banks,
 * minigame-mode extras (GWBankFlagGet) -- and every Solo-mode prize record
 * (GWSinglePrizeSaveFlagGet). This mirrors the game's own GWSaveDebugSet,
 * minus its 1000-star grant. Default off.
 *
 * SAVE SAFETY. The override lives only at the query layer of
 * game/gamework.c (compat/decomp/src/game/gamework.c.patch): GwCommon keeps
 * the save's real bits, so the memory-card image never gains an unlock the
 * player did not earn, and turning the mod off restores the true locked state
 * immediately. Two game paths would otherwise write a queried "owned" answer
 * back into GwCommon -- the Star Bank's exit copies every item it displayed as
 * owned into GWBankFlagSet, and that copy calls GWMgUnlockSet(677) for item
 * 6 -- so while the mod is on those unlock setters leave the bits unchanged
 * (progress earned while it is on is not recorded). The one bank flag that is
 * a player OPTION rather than an unlock (60, the minigame-mode model toggle
 * the bank switches on and off) keeps its real value and stays settable.
 *
 * SOURCES. The launcher publishes the persisted game.unlock_all setting here
 * after loading the config and after every settings change; automation runs
 * never read the config. MP6_UNLOCK_ALL=1/0 overrides both (and disables the
 * Settings row), for scripted runs and headless builds.
 *
 * Host-owned state: the store is carved out of savestates (a restored state
 * must not flip the running process's setting).
 */
#ifndef MP6_UNLOCK_ALL_H
#define MP6_UNLOCK_ALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bank flag 60 is the bank's on/off minigame-mode option, not an unlock. */
#define MP6_UNLOCK_ALL_BANK_OPTION_FLAG 60

/* 1 when the override is active (MP6_UNLOCK_ALL when set, else the
 * published setting). Safe to call every frame. */
int mp6_unlock_all_enabled(void);

/* The launcher's published setting (0/1). */
void mp6_unlock_all_set_enabled(int enabled);

/* 1 when MP6_UNLOCK_ALL is set, so the Settings row cannot change it. */
int mp6_unlock_all_env_override(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_UNLOCK_ALL_H */
