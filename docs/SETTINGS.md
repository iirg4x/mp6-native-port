# Player settings

Settings keep the existing party-style interface, with short descriptions of
what changes during play. Existing saved configuration keys remain compatible.

| Tab | Contents |
| --- | --- |
| Video | Original / Classic+ / Modern presets, picture quality, smoothness, FPS counter; desktop window and monitor controls |
| Audio | Volume and More Sound Effects |
| Game | Guided game selection; desktop save-folder shortcut |
| Mods | Fast Forward, Quick Party, Free Camera and Unlock All |
| Advanced | Extra Memory and Graphics Driver (only when multiple real drivers exist) |
| Touch Controls (Android) | Full GameCube touch controller, size/opacity, floating sticks, layout editor and individual control visibility |
| Save States | Numbered slots, Quick Save / Load, compatibility and trusted-file guidance; available during play |
| About | Version, project status and credits |

Android does not expose Window Mode, Window Size, monitor selection, desktop
save-folder actions, keyboard quick-save labels, or desktop-only supersampling.
Game files can only be changed before starting play. Raw Content Root and Clear
Path controls were removed; existing paths still work and guided selection is
the supported way to replace them.

Android always uses immersive fullscreen, independently of saved desktop window
preferences and the selected picture aspect ratio. Swiping from an edge can
temporarily reveal Android's system bars. Fullscreen is restored when returning
to the game; the console's on-screen keyboard remains available.

## When changes apply

Volume, VSync, FPS counter, Unlocked FPS, Ambient Occlusion, Fast Forward, sound-effect limits,
desktop window controls and monitor selection apply live. Shadow quality
replaces active shadow maps when memory permits. Widescreen adjusts the picture
live; scene-specific camera/background changes finish on the next scene change.

All anti-aliasing modes apply live in the optimized Windows build. The older
Debug/Android renderer still requires a restart for some anti-aliasing changes.
Extra Memory needs a restart only after game memory has been allocated.
Graphics Driver changes need a restart. Presets also set the audio and memory
options; the preset description explains this.

Fast Forward speeds up gameplay. Unlocked FPS smooths motion at normal speed;
these are intentionally separate controls. Explicit launch overrides still
take priority without changing the saved configuration.

## Quick Party

Mods > **Quick Party** (Off / 2x / 3x / 4x) shortens a party without touching
minigames. It is off by default, applies live and is saved as
`game.quick_play` (1 = off, 2..4). `MP6_QUICK_PLAY=0..4` overrides it for a
run; `MP6_QUICK_PLAY_TRACE=1` logs every change between full speed and the
wait that holds normal speed.

- Boards and results screens run the chosen multiple faster. Where the
  device keeps up, the board simply runs that many more ticks a second and
  shows one per display refresh (also on the web, where the game otherwise
  follows the display). Whatever the device cannot reach is made up inside
  the game at the normal 60 ticks a second: board event waits, walking
  between spaces, turning on the spot and camera moves take fewer ticks.
  Minigames, their instructions, file select and every mode menu stay at
  normal speed.
- The board drops to normal speed for exactly the ticks it waits on a human:
  the dice block, path choices, the orb menu and targeting, the shop, item
  roulettes, the minigame pick, the map view, choice windows and the pause
  menu. Cursors, roulettes and the dice keep their usual timing.
- Message text prints at once and "press A to continue" pauses move on by
  themselves straight away, so lines that only explain or comment no longer
  stop the game; questions never answer themselves, and help panels stay up. The turn-start
  banner and the solo board guide pass without a press, as they already do
  for CPU players.
- The game boots straight to save-file selection: no warning screen, logos,
  opening or title. Backing out of file select still reaches the title.
- Mode select skips the hosts' greeting, and Party Mode skips the welcome
  lines, the "learn how to play Party Mode?" question and the "choose the
  game settings" line, opening straight on board selection. The saved-party
  resume question and every setting screen (board, mode, players,
  characters, skill levels, turns) still come up as usual; backing out of
  board selection asks whether to leave Party Mode.

VSync is never changed. Fast Forward is separate: it runs every scene as
fast as the device allows. Nothing reaches the save file.

## Unlock All

Mods > **Unlock All** makes every board, character, minigame, Star Bank
extra and Solo-mode prize record available without unlocking it first, like
the game's own debug unlock but without its bank-star grant. It is off by
default, applies live (menus already open update when next entered) and is
saved as `game.unlock_all`. `MP6_UNLOCK_ALL=1`/`0` overrides it for a run.

The memory card is never changed by it. The mod answers the game's unlock
queries without touching the loaded save data, so turning it off shows exactly
what the save has unlocked. While it is on, the Star Bank cannot write its
"owned" items back and new unlocks are not recorded; the bank's on/off
minigame-mode option still saves normally. `tests/test_unlock_all.py` checks
this against the real save format.

## Ambient Occlusion

Video > Picture > **Ambient Occlusion** adds soft contact shading between
characters and scenery. Choose **Subtle** for lighter shading, **Strong** for
more contrast, or **Off** for the original lighting. It applies during play
without restarting, including with Unlocked FPS. It costs GPU time and battery;
it is off in all presets and remains off when upgrading an older config.

The setting is saved as `enhancements.ambient_occlusion` (0/1/2). Automation
can opt in with `MP6_ENH_AMBIENT_OCCLUSION`. Like other enhancement switches,
an explicit launch override wins without changing the saved preference.

This is screen-space shading, not ray tracing or new light sources: it uses
visible scene depth and cannot shade from geometry outside the view. Front HUD
sprites and the settings/touch overlays are drawn afterwards. Translucent HUD
backgrounds still show the shaded scenery beneath them.

Renderer ownership, resource lifetime and verification are documented in
[AMBIENT_OCCLUSION.md](AMBIENT_OCCLUSION.md).

## Android touch controller

The overlay has independent main and C sticks, an eight-way D-pad, A/B/X/Y,
Z, Start, and L/R. Both sticks and multiple buttons can be held together.
The main stick controls analog-only menus; the D-pad sends digital directions.

Open the gear > **Touch Controls** to adjust size (75–150%), opacity (20–100%),
floating stick centers, or individual button visibility. These changes apply
immediately. The gear stays available with the controller hidden.

L/R are full-press buttons across their entire touch area. There is no slider.
The console's Close button or its top-left X returns control to the game;
opening the gear menu also closes the console input.

During play, choose **Edit Layout**, drag visible controls, then **Save**.
**Cancel** restores the previous layout; the editor's **Reset** restores default
positions. **Reset Touch Controls > Restore Defaults** also restores visibility,
size, opacity and stick behavior. Editing blocks game-controller input (it does
not pause game time, just like the existing settings menu).

Layouts respect screen cutouts and have separate default arrangements for
landscape and portrait. Saved custom centers are proportional to the usable
screen. Size increases may need a layout adjustment to keep controls apart.
Preferences live in the existing config's `touch.*` keys, not in game save states.

This is an independent port implementation inspired by
[Dolphin's configurable Android overlay](https://github.com/dolphin-emu/dolphin/blob/master/Source/Android/app/src/main/java/org/dolphinemu/dolphinemu/overlay/InputOverlay.kt),
not a copy of Dolphin's source or artwork.

## Unavailable minigames

Boards roll and launch minigames with the game's own flow on every platform.
When the rolled minigame's module is not recovered yet, it ends at once with
nobody scoring (Bowser minigames: everyone escapes), the results screen and
turn save still run, and the board continues. The UI shows a notice.

Outside boards the missing-module fallback returns through the real overlay
manager to the nearest supported history entry instead of idling on a black
screen. One-shot boot initialization is not a return target. If no usable
history exists, it resets the history root to mode select.

Tests cover native return decisions, board continuation, one-shot notifications,
and preprocessed desktop/Android menu visibility. Android visibility tests are
not a substitute for a device run.

After building the release headless executable with local disc assets, run
`python tests/integration/check_overlay_routes.py` to exercise a missing
minigame, instruction screen and free-play menu through the real overlay
manager. Each must return to mode select and keep ticking before exiting cleanly.
