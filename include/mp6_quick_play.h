/* MP6 native port -- the "Quick Party" mod (Settings > Mods > Game Speed).
 *
 * One selector: Off / 2x / 3x / 4x. When on, it
 *   - speeds the boards and results screens up, while minigames, their
 *     instructions and every menu keep normal speed (see the mechanism
 *     below: extra ticks where the device keeps up, in-game shortening of
 *     waits, walking, turning and camera moves for the rest);
 *   - drops back to normal speed for any tick in which the board is waiting on
 *     a human player's decision or timing (dice hit, path choice, item and
 *     shop menus, roulettes, map view, choice windows, the pause menu), so
 *     cursors, roulettes and the dice block behave exactly as usual;
 *   - plays a board event's timed challenge (anything with a countdown, or
 *     a help panel saying what to press: a pick, a mash, a board mini-game)
 *     at normal speed, and on a human
 *     player's event everything from the challenge (or the part to watch
 *     before it, like a shell game's shuffle) to the event's end;
 *   - prints message text instantly and moves past "press A to continue"
 *     pauses at once, so dialogue that only explains or comments no longer
 *     stops the game (questions and choices still wait for the player, as do
 *     help panels and lines that state what is at stake, like Bowser's
 *     penalty for losing his minigame);
 *   - lets the turn-start banner and the solo-mode board guide pass without a
 *     button press, as they already do for CPU players;
 *   - boots straight to save-file selection, skipping the warning, logos,
 *     opening movie and title screen.
 * Default off. Nothing it does reaches the save file.
 *
 * Game-speed mechanism, two layers that together give the chosen speed:
 *   - extra ticks: the tick throttle (src/gx/aurora_bridge.c) paces a sped-up
 *     board at speed x the base rate (60 Hz when the game otherwise free-runs,
 *     as on the web) and shows one tick per display refresh, hiding the rest,
 *     so the whole board runs faster wherever the device keeps up;
 *   - in-game: the shortfall (chosen speed / tick multiple actually reached)
 *     shortens board process sleeps (src/os/process_native.c), player moves
 *     between spaces (board/player.c) and board camera moves
 *     (board/camera.c), so a device that cannot run extra ticks still gets
 *     faster event waits, walking and camera at the normal 60 ticks/s.
 *
 * SOURCES. The launcher publishes the persisted game.quick_play setting (1 =
 * off, 2..4) after loading the config and after every settings change;
 * automation runs never read the config. MP6_QUICK_PLAY=0..4 overrides both
 * (0 and 1 mean off) and disables the Settings row, for scripted runs.
 *
 * Host-owned state: the store is carved out of savestates (a restored state
 * must not change the running process's setting or speed).
 */
#ifndef MP6_QUICK_PLAY_H
#define MP6_QUICK_PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#define MP6_QUICK_PLAY_OFF 1
#define MP6_QUICK_PLAY_MAX 4

/* The selected speed: 1 (off) .. MP6_QUICK_PLAY_MAX (MP6_QUICK_PLAY when
 * set, else the published setting). */
int mp6_quick_play_speed(void);

/* The launcher's published setting (clamped to 1..MP6_QUICK_PLAY_MAX). */
void mp6_quick_play_set_speed(int speed);

/* 1 when MP6_QUICK_PLAY is set, so the Settings row cannot change it. */
int mp6_quick_play_env_override(void);

/* 1 when the mod is on and the current overlay is a sped-up scene (a board or
 * a results screen). Text-window shortcuts apply only here. */
int mp6_quick_play_scene(void);

/* 1 when the mod is on: boot skips the intro and goes to file select. */
int mp6_quick_play_skip_intro(void);

/* Called by game code while it polls a human player for a decision or a
 * timed press. Keeps this tick and the next at normal speed; harmless (and
 * free) when the mod is off. `what` names the wait (a string literal) for the
 * MP6_QUICK_PLAY_TRACE=1 log, which prints each change of what holds speed. */
void mp6_quick_play_hold(const char *what);

/* Board events: board/masu.c brackets each call of a board's own event hooks
 * (passing a space, reaching one, stopping on a ? space) with these;
 * playerNo is the player the event is for. Outside a bracket the countdown
 * rule below only holds the current tick. */
void mp6_quick_play_event_begin(int playerNo);
void mp6_quick_play_event_end(void);

/* A board event's stretch that a person has to watch to play it (a shell
 * game's shuffle, a pick and its reveal): from this call to the end of the
 * event, the board runs at normal speed when the event's player is a human.
 * A computer's event keeps its speed. `what` names it for the trace. */
void mp6_quick_play_event_watch(const char *what);

/* Called every tick while a countdown timer is on screen (game/gamemes.c).
 * On a board a countdown means a timed challenge -- a pick, a mash, a board
 * mini-game -- so that tick runs at normal speed whenever any player is a
 * human (someone may be playing it even on a computer's turn), and on a
 * human player's event the rest of the event stays at normal speed so the
 * result is shown at the game's pace. */
void mp6_quick_play_countdown(void);

/* Once per tick (VIWaitForRetrace): settles and returns this tick's board time
 * scale (1 = normal speed). Consumes the hold window above. */
int mp6_quick_play_tick_factor(void);

/* This tick's board time scale, as last settled (1 = normal speed). */
int mp6_quick_play_time_scale(void);

/* A board-script duration in ticks, shortened by the time scale (never below
 * one tick; 0 and negative values, which mean "now" and "forever", pass). */
int mp6_quick_play_scale_ticks(int ticks);

/* The same, for process sleeps (src/os/process_native.c). */
int mp6_quick_play_scale_sleep(int ticks);

/* MP6_QUICK_PLAY_PARTS bit mask (diagnostic; default 5 = all): 1 process
 * sleeps, 4 player and camera moves. (Model animation is never scaled in-game:
 * board scripts poll animation frames, and skipped frames stall them.) */
int mp6_quick_play_parts(void);

/* From the tick throttle: the multiple of the base tick rate this device
 * actually reaches on a sped-up board. The in-game scale below makes up the
 * difference to the chosen speed. */
void mp6_quick_play_set_achieved(double multiple);

/* The in-game speed-up applied to board sleeps and moves: chosen speed /
 * achieved tick multiple, between 1 and the chosen speed (1 when held). */
double mp6_quick_play_game_scale(void);

/* 1 when text windows print a whole page at once: sped-up boards only. A
 * results screen times its animations by its pages, so its text prints at
 * the game's own speed (the extra ticks still make it quick). */
int mp6_quick_play_instant_text(void);

/* 1 when board message messNum states what is at stake -- a penalty or a
 * prize the players should know before it happens. On a sped-up board such
 * a page still prints at once, but waits for a person's A press instead of
 * moving on, even on a computer's turn (game/window.c drops the computer's
 * scripted press); a party with no person keeps the scripted press. */
int mp6_quick_play_must_read(unsigned int messNum);

/* 1 when board help panel messNum tells players what to do in a board
 * event (mash A, pick, aim) rather than general controls (dice, paths, the
 * map, "Start: skip"). While one is up the board runs at normal speed if
 * any player is a person: game/window.c calls mp6_quick_play_action_prompt()
 * every tick such a panel is on screen. */
int mp6_quick_play_action_help(unsigned int messNum);
void mp6_quick_play_action_prompt(void);

/* 1 when any player is a person. */
int mp6_quick_play_human_present(void);

/* Ticks a printed "press A to continue" page stays up before the mod moves
 * on: 1 on boards, as long as a prompt player elsewhere. */
int mp6_quick_play_keywait_ticks(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_QUICK_PLAY_H */
