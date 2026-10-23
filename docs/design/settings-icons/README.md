# Settings icon art direction

The seven `*-v3.png` files are the current settings artwork. Runtime copies
in `res/menu/` are produced by `tools/prepare_menu_icons.py` at 256 pixels.
The complete built-in imagegen prompt set, generated filenames, input references,
and sound-icon edge cleanup prompt are recorded in `prompts-v3.json`.

The six `option-*-v1.png` illustrations give Preset, Shadow Quality,
Anti-Aliasing, Ambient Occlusion, More Sound Effects and Free Camera their own
identities. Their exact built-in generation prompts are in
`prompts-options-v1.json`. Category art is never repeated on option cards.
Other options use native functional previews: live aspect/volume/opacity,
actual button labels, distinct save/load slot numbers, and transport symbols.
New settings default to text-first, never to a repeated generic picture.

The direction follows contemporary Mario Party menus: simple illustrated category
pictograms, restrained dimensional shading, thin white separation edges and
softly shaded 3D Brighton/Twila heads with their recognizable split colors.
Game is a Mushroom Orb, Extras a star-stamped book, and the cursor a mouthless
collectible star. The actual menu is a horizontal option-card deck, not the old
two-pane settings inspector. Category chips sit above it; Brighton and Twila's
message bubble sits below. Full choices/help open as a separate sheet. Left/right
browse cards, A/Enter toggles or opens a card, A enters slider adjustment, and
B/Esc leaves adjustment or returns from a sheet. Pointer/touch arrows also browse.
The menu uses sky-tinted light panels, dark readable Inter type, pink selection
and yellow focus edges. Retained transforms provide
animation without another sprite sheet or a full-screen filter.

The unversioned PNGs and `prompts.json` preserve the rejected glossy study.
The `*-v2.png`, `prompts-v2.json` and `day-night-v2.json` preserve the superseded
retro outlined study. Neither study is shipped or used by the current importer.

The Brighton/Twila JPGs in `references/` are character artwork from Super Mario
Wiki. The modern menu references are official Nintendo screenshots:

- [Jamboree rules menu](https://www.nintendo.com/jp/topics/article/107fc72c-c536-454d-89c7-1f5500510832)
- [Superstars settings](https://www.nintendo.com/au/games/nintendo-switch/mario-party-superstars/)

These are design references, not generated or original port artwork, and are
not included in runtime resources. The port UI is inspired by these references;
it is not an extraction or exact reproduction of Nintendo's menu.
