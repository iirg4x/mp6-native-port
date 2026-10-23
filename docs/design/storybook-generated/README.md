# Generated storybook — approved visual prototype

This replaces the rejected concept-cropping approach. The concept is a visual
reference only. None of its pixels are used by this preview.

**Approved and integrated into the native settings renderer on 2026-10-02.**
This directory remains an independent visual-only preview. The game implementation
is `src/gx/ui/storybook_view.hpp`, driven by the real controls in `settings.cpp`.
Historical revision notes below describe the prototype at that point in time.

The generation recipes are local provenance, not a portable asset bundle:
`prompts.json` retains the original machine-specific image paths, and
`prepare.cjs` also expects a local reference capture. The source images and
generated textures are intentionally ignored by Git. To regenerate elsewhere,
supply your own copies and update those paths first; the normal native build
does not run this historical preparation script.

## Native integration

- `capture-native.cjs` calls the preview's `proof.nativeExport()` through Playwright
  and saves `output/playwright/storybook-native.json`. Run
  `node tools/import_storybook.cjs` to import its blank paper profiles, independent
  sprites and font into `res/menu/storybook/` and `res/fonts/`. It does not flatten
  menu text or controls into a screenshot. Re-export after changing the paper art.
- The native mesh shares one warped surface for paper, ink, selection and photo.
  The border is below the content, and the opaque turning leaf paints above the
  stationary pages. A single sheet turns in 280 ms; rows, values, descriptions,
  chapter tabs and the background animate independently.
- Native UI fills the physical window independently of the game's widescreen
  mod or presentation aspect (`0056-full-window-menu.patch`). The day/night sky
  fills the viewport, while the entire 1672×941 book composition is centered with
  one uniform contain scale. Paper, tabs, text, controls, hosts and footer cannot
  stretch independently. Extra space reveals the populated sky, not black bars.
  Pointer coordinates invert the same centered scale before unwarping the page.
  Vector silhouettes have screen-pixel coverage antialiasing, independent of game
  AA. Arrows are single joined chevrons; the photo and white frame share the warp.
- Exported `book-left`, `book-right`, `paper-left` and `paper-right` keep each
  face's ornament orientation. Resting and moving sheets use the same face;
  an ivory crease and red lower saddle seal the center binding.
- At the user's request, intro/outro delays and durations are now doubled, with
  page turns and navigation feedback unchanged. Entrance closes the two zigzag
  sky panels into place (0–320 ms), slides in the book (160–600 ms), then pops
  Brighton/clouds (580–1100 ms) and Twila/clouds (800–1320 ms). The book is
  interactive by 600 ms; later motion is decorative.
  The 800 ms exit reverses the layers in staggered order. Per-layer fades are
  avoided so scenery cannot show through paper. Closing releases input immediately,
  rapid reopening reverses from the current pose, and Menu Animation Off skips all transitions.
  One continuous playhead drives all layers in both directions; repeated F10
  presses never reset the sky, book or hosts to an endpoint.
- Brighton and Twila each share a restrained damped spring with their clouds.
  Both position and scale use the same spring (about 3.8% overshoot), followed by
  a smaller counter-settle and zero-velocity join to the idle drift. Departure
  reverses the spring with a slight anticipatory lift. This replaces the old
  scale-only pop plus linear slide that visually stopped in place. The stagger,
  book/sky easing, total intro/outro times and motion opt-out are unchanged.
  Native checks sweep the curve for bounded scale and smooth endpoints; GPU
  captures include both host rebounds and the early outro lift.
- Sunbeam geometry is clipped to the day panel before assigning mask UVs or
  applying the entrance/exit translation. This prevents the shared repeating UI
  sampler from wrapping off-panel rays into the opening between the sky halves.
  Ray angles are built in screen space, without viewport or book scaling; their
  reach extends beyond the window and is clipped, rather than stretched to fit.
  Mask sampling stays within edge texel centers to prevent bilinear wrap seams.
  The native geometry regression sweeps every ray through a full rotation and
  101 reveal positions at four window sizes; the GPU harness captures the first
  entrance frame as well as the later sky/book/character stages.
- The zigzag is one opaque black/white 64×126 luminance mask, generated from
  precise vector geometry by `tools/generate_storybook_mask.cjs`. Its top and
  bottom rows match; the tile repeats vertically at one uniform scale. Only the
  constant side margins extend to fill the window. Aurora's `luminance-mask`
  shader samples white coverage for night and `1-mask` for day/rays from that
  same texture; no inverse-mask asset is needed. Geometry tests check coverage
  and identical physical x/y source-pixel size at six aspects, with GPU checks
  for intro seams, opaque grayscale pixels and a seamless source tile.
- Actual Rml controls still own settings callbacks, persistence, disabled states,
  inline choices and save/load actions. Left/Right step values; A/Enter advances
  adjustable choices without opening the legacy option picker. Arrows grey out
  independently at value limits; enum choices no longer wrap. Help stays on the
  right page. Picture Preset is first; desktop has four
  chapters, with touch Controls retained only for Android. No new setting options
  were introduced by this integration.
- The background is a complete scene behind the book, not just edge decoration:
  twelve rising balloons cover the day half, and twenty-three bright twinkling
  stars gently drift and rock throughout the night half. The distant star field
  drifts more subtly, with rotating glints on its brighter points. Motion Off
  freezes the scenery.
  The opaque book covers them normally; intro/outro reveal the populated sky.
  The browser preview and native renderer use matching positions and sizes.
- During play, the photo borrows the current pre-UI GPU scene. It fits the complete
  frame using the current scene texture dimensions (not the menu's aspect), and ignores GX destination alpha
  when compositing the scene into the opaque photo. No CPU readback is used by the
  production menu. Launcher Settings uses the Picture icon instead; the browser
  design prototype retains its reference gameplay still.
- The photo fits a padded 580×274 print-space slot (926,152), keeping its thin
  paper edge and folded corner pockets inside the right-page ornament. Decorative
  titles under all chapter images, including Live game preview, are removed.
  The former caption band remains clear paper before the blue setting help.
- Verified in isolated, muted, hidden native runs: desktop and host-compiled touch
  branches; keyboard, mouse, synthetic controller shoulders, paging, focus after
  clicks, volume bounds, inline-only choices, close/reopen, motion opt-out, and save-slot labels.
  Save/load actions were not executed. GPU captures cover 720p, 2x DPI, a narrow
  landscape window, 4:3, ultrawide, and both resting and turning pages. Pixel
  checks cover full-window sky, uniform paper contours, a continuous spine, proportional balloons and
  antialiased coverage at four resolutions.
  The live-game harness keeps the same menu open across 16:10, 4:3, ultrawide,
  720p and 2560×1440 with 2× UI scale. It checks the actual layout bounds, a clear
  ivory-pixel gap above the caption, and nonblank live scene pixels at all five
  stages. A second five-stage run keeps the game in original 4:3 while its menu
  still fills the ultrawide window. This is not an Android
  device certification.
- Evidence: `build/settings-qa/captures/`, `build/settings-qa-touch/captures/`, and
  `build/settings-qa-preview/captures/live-preview.png`. The latter was captured
  while the actual game was running and is checked for nonblank scene pixels.
- Automated checks: 20 player-settings/runtime-contract tests, 13 preview/source
  fidelity checks, and the updated preview's 30-check browser audit passed.

### Launcher opening spread and first-run linking

Launcher Settings turns from a snapshot of the actual opening spread in 280 ms,
without replaying the sky/book/host entrance. The chapter tabs rise from behind
the paper with position-only springs staggered 100 ms from left to right. Each
spring lasts 480 ms; desktop tabs settle in 780 ms (880 ms with five touch tabs),
independently of the quick page turn.
Welcome uses the same spring on initial entrance and when returning to the launcher.
Back can cancel immediately and returns without an outro. Motion Off skips both
the turn and springs. Standalone and in-game
Settings retain the animated entrance and exit. The launcher no longer prints
the decorative "A day-and-night adventure" subtitle below the game logo.

Picture in launcher Settings uses a generated icon, never a static game preview.
The native live preview remains available during gameplay. Every settings icon
and the live preview use the same scrapbook photo mounting: a thin off-white
paper edge, soft shadow and small yellow folded corner pockets, with no rigid
gold bevels or inset mat. The linked Mario Party 6 logo uses that same mounting.
Its 540×224 paper mount is bounded separately from the logo art, with a 24-unit
inner inset and room outside for the corner pockets, shadow and page ornament.
The logo fits proportionally inside that inset; its intrinsic width never sets
the mount size. Geometry checks sweep six viewport sizes, and launcher captures
check the printed ornament remains visible alongside the whole mount.
The mount follows the page mesh. The
Picture icon was created with built-in imagegen using the existing Picture
composition and approved Sound/Game sprites as references; its full prompt is
in `chapter-picture-generation.json`, original in `sources/chapter-picture.png`,
and deployable sprite in `assets/chapter-picture.png`. Repackage without changing
its generated alpha using `node tools/package_storybook_picture.cjs`.

`Prelaunch` uses the same native `StorybookView` as Settings: full-window day/night
sky, proportional hosts/clouds, layered paper, Titan One type, animated red ribbon
and selection star. Play/Select Game, Settings and Quit remain real semantic Rml
buttons. Pointer hit testing follows the page warp, and keyboard/controller focus
drives a single selection. Play and Quit wait for the shared book outro before
handoff; reduced motion remains immediate. The disc-decoded logo is optional.

With no valid linked content, the first launcher update opens `ContentSetup`
automatically over the opening spread. The scoped cream/red/gold linking dialog
offers a disc-image picker, an extracted-folder picker and Not now. Dismissal does
not reopen it automatically in the same session; Select Game can reopen it.
Successful linking refreshes Play and runtime branding without requiring a restart.
Existing validation, progress, error reporting and cancellable import are retained.
No disc art is needed for the unlinked launcher, and no game files are bundled.

Menu sounds use the user's `sound/MP6_SND.msm`: the four original common cursor,
confirm, accept and cancel effects (MSM_SE_CMN_01–04). A bounded read-only parser
validates their simple one-shot macros and predecodes DSP-ADPCM before input.
Separate private SDL streams keep UI sounds audible while menu music overrides
game audio; each cue has one voice, preventing rapid-input queues. Master volume
and mute apply immediately. Invalid/missing banks stay silent. No game MSM state,
disc files, mixer voices or savestates are modified.

The hidden native harness checks automatic linking, dismissal without a reopen
loop, manual reopening, the post-link switch to Play, keyboard/pointer navigation,
Settings round-trip and Play's outro handoff. It captures linked/unlinked spreads,
desktop/landscape/portrait linking dialogs, and 4:3, ultrawide, high-DPI and small
landscape launchers. Picker/import actions are not invoked by these UI checks.
Both launcher and settings use the same fixed-aspect book fit; only their sky
expands to fill different window shapes. The linking dialog lays out separately
within 90% of the viewport, with its complete panel and actions checked in small
landscape and portrait windows.

### Linked-disc soundtrack

The native launcher and in-game settings now shuffle idle music from the validated
linked disc's `files/sound/MP6_Str.pdt`. The visual-only browser prototype remains
silent. No soundtrack files are bundled or downloaded, and no new setting was
added. Missing or invalid banks leave the existing audio alone.

- `src/audio/pdt_music.hpp` validates the bank and incrementally decodes bounded
  DSP-ADPCM chunks. The parser recognizes 97 authored looping tracks in the local
  USA bank, but `menu_music_catalog` admits only six verified idle/menu streams:
  file selection (2), mode selection (3), party setup (4), minigame
  selection (6), options (7), and Star Bank (92). IDs are verified against their
  original game call sites, not inferred from the legacy stream-header names.
  Minigame gameplay, board/event, cinematic, result and jingle tracks are excluded,
  as is the non-looping title cue;
  an empty idle pool stays silent rather than falling back to other loops.
  Each shuffled bag visits every eligible track once, with no immediate repeat
  across bags. Tracks advance after one authored pass, with short boundary fades.
- `src/audio/menu_music.cpp` owns a separate SDL stream on the UI thread, respects
  Master Volume, and closes its file/device on unlink, menu close, or shutdown.
- As explicitly requested, opening in-game settings replaces game audio while
  the book is open. Only the game stream's output gain is suppressed: the game's
  mixer, track positions and saved volume are untouched. Closing the book restores
  the latest Master Volume. If menu music cannot start, gameplay is not silenced.
- `tests/test_menu_music.py` covers malformed/truncated metadata, chunk history,
  mono/stereo output, restart/read failure, the exact idle allowlist, no-fallback
  behavior, six-track shuffle bags, and gain restoration.
  The linked-bank test decoded all 97 tracks (124,857,656 PCM frames) end to end.
  Hidden native runs use SDL's dummy audio device and check launcher link/unlink,
  in-game playback, muting, close/reopen, and restoration after a volume change.

### Cold-open preparation

The first native settings render used to decode the book PNGs, build font atlases,
and upload textures after the entrance clock had started. The hidden native
harness measured 333.713 ms for that first render, versus about 1 ms afterward.
`ui::initialize()` now prepares the art and Titan One atlases after loading fonts,
primes the shared stylesheet cache with an unshown settings document, and draws
each prepared texture into a private 1-pixel target. The last step is necessary:
Aurora defers uploads until drawing, so decoding alone still left a 46 ms hitch.
The warmup does not acquire/present a window surface or run menu/control actions.
Resources remain owned by Rml's renderer and are released at normal shutdown.

The same hidden test now measures 3.858 ms for the first render (then 1.561 and
0.873 ms); the measured startup initialization including warmup was 445.291 ms.
These are local CPU timings, not a cross-device FPS guarantee. The native harness
checks that startup preserves configuration, document count and focus, and guards
the first settings render with a generous 50 ms regression ceiling. Animation
durations, close/reopen behavior and visual layout are unchanged.

## Components

- One newly generated 1024×1024 blank book-half texture, drawn twice with mirrored
  UVs. Typography, tabs, selection, ornaments, help and photo are separate layers.
- Generated Brighton and Twila cutouts. Three code-native cloud silhouettes
  share an identical contour/rim width across seven unscaled instances. The
  earlier raster clouds remain as provenance only, and are not loaded or drawn.
- Newly generated sun and moon sprites (256×256), animated independently.
- Newly generated red selection ribbon (1024×128) and cursor star (128×128).
- Newly generated selected-tab shell (512×256), one grayscale round button
  shared by A/B via renderer tint, one mirrored shoulder shell, and a rounded
  gold bubble star (128×128 each).
- Day-gold and night-indigo bottom-to-top lightening gradients, composed through
  a standalone zigzag alpha mask. Rising native balloon cutouts and distant
  glowing stars sit behind the book; galaxies were removed on request.
- Stars/confetti are individually animated procedural particle instances.
- Broad sunbeams rotate independently behind the sun and book, clipped by the
  same day/night mask. They are not baked into either background texture.
- Crisp font-rendered labels; simple shaded control surfaces and gold ornaments
  are code-native. No labels are baked into textures.
- A real user-provided gameplay capture is used for the static photo preview,
  not an imagegen imitation of gameplay. It is not yet a live game texture.

All texture dimensions are powers of two. `manifest.json` records dimensions and
content rectangles so transparent packing padding is not stretched into artwork.
The book is reused by mirroring, not by storing a second book texture.

## Source and verification

`prompts.json` records every selected asset's complete built-in imagegen prompt
and original generated file. `prepare.cjs` packs those outputs, cleans isolated
alpha debris and clips generation halos from the blank book/ribbon. It never
reads the concept. Only newly generated image margins are trimmed during packing.

`index.html` and `preview.js` are the independent visual-only reconstruction.
Its inspection controls switch between assembly and the asset gallery, pause/play,
and replay a leaf turn. Clicking a row / Up / Down changes only the visual selection;
Left/Right adjust preview values, Q/E change chapters, and the replay button
replays the leaf turn. None call game settings, controllers, audio or runtime.

Code layout: classic scripts with no build step, loaded in this order by `index.html`.

- `page-mesh.js`: the WebGL mesh that bends a flat page texture onto the curved paper.
- `confetti.js`, `backdrop.js`, `clouds.js`: scenery. Deterministic confetti, the
  day/night sky, and the native vector clouds.
- `chapters.js`: menu data, one object per row (`label`, `value`, `help`, `options`,
  ...), limited to entries that exist in `src/gx/ui/settings.cpp`.
- `menu.js`: menu rules. Presets and "Custom", availability, value stepping.
- `paper.js`: measures the cream paper in the generated book texture, once per
  texture size.
- `preview.js`: layout, page printing and compositing, page turns, input, and the
  `window.proof` hooks used by the verify, inspect and export scripts.

The earlier claim of a zero-difference source decomposition is **not** an acceptance
test for this new generated-art reconstruction. The new art needs visual approval.
The backdrop deliberately follows the user's newer particle/mask instructions.

All menu lettering uses Titan One by Rodrigo Fuenzalida, a rounded, bubbly display face;
the unmodified font and OFL license are stored together in `assets/`.
Source: https://github.com/google/fonts/tree/main/ofl/titanone.
Footer and controller glyphs use the same face. Bree Serif is retained only as
earlier-revision provenance, not loaded by the preview.

Run `node docs/design/storybook-generated/serve.cjs` from the port checkout and
open the localhost URL it prints. No installation is required for the preview.
The preview renders typography at the actual display pixel density while keeping
the landscape composition's proportions fixed.
To repack just one edited generated component without touching other assets,
run `node docs/design/storybook-generated/prepare.cjs --only <component-name>`.

## Feedback revision — 2026-10-01

- Default preview animates; `?still` is explicitly a screenshot/test switch.
  Pause freezes the current frame and resume continues from it without a jump.
  OS reduced-motion starts paused, with an explicit Play override.
- Photo, its four mounts and the printed content share the book's curved surface.
  A page-turn demonstration bends that same surface; the reverse face is blank
  paper, not mirrored lettering or gameplay.
- Removed the large printed sun and blue crescent from the pages. Sky sprites,
  the selected Picture tab's small emblem, Brighton and Twila remain separate.
- Regenerated the selection star with built-in imagegen: compact gold silhouette,
  thin dark contour, no oversized ivory/navy rim. Final prompt in `prompts.json`.
- Consolidated concept anchors in `preview.js:layout`. Corrected the generated
  book's exaggerated outer slant, photo mounts, horizontal cloud banks, cursor
  size/placement, corner ornaments and the detached footer.
- One mirrored 1024×1024 book asset is still shared. Paper render surfaces are
  power-of-two and text is rasterized for actual DPI. No reference pixels used.
- Full landscape composition fits both viewport dimensions without stretching.

Verification: `verify.cjs` run through Playwright CLI, 24 checks passed. Includes
visible pixel animation, exact paused pixels, resume, single-click and keyboard
selection, complete page-turn cycle, shared photo curvature, 4 landscape viewport
sizes, actual 2× DPI, reduced-motion and POT sizes; no browser errors.

Evidence: `output/playwright/storybook-revision-2-reference.png`,
`storybook-revision-2-720p.png`, `storybook-revision-2-hidpi.png` and the recorded
`storybook-revision-2.webm`. These are browser renders, not generated mockups.
This remains a visual approval candidate, not a claim of pixel-identical art or
live gameplay integration. Do not wire it into the game before approval.

## Browser annotations — revision 3, 2026-10-01

The seven user-marked regions are addressed in the independent browser proof:

1. Replaced the wedge/star at the upper-left page corner with a tapered curved
   gold bracket. The concept has no star in that corner. Other corner ornaments
   now use the rounded star asset.
2. Generated a new red selected-tab shell with an ivory edge and gold bevel;
   replaced the spiral emblem with the concept's ring-and-dot sun and adjusted
   tab lettering. The first overly ornate generated shell was rejected.
3. Inset the photo to 640×344 at (896,212). Photo and all four corner mounts are
   composited together, then wrapped using a smooth fit to the generated book's
   ivory-paper contour. Fitting broad curvature avoids following tiny generation
   defects. Page-turn mapping is applied after this common surface projection.
4. Added DPI-scaled soft cast shadows to all chapter tabs; inactive tabs have
   subtle ivory shading and a narrow gold edge.
5. Added two freshly generated cloud silhouettes alongside the existing cloud.
   Seven independently positioned cutouts use the same fixed 0.5 source-pixel
   scale and preserve their proportions. No anisotropically stretched clouds.
6. Replaced the divider's procedural sharp star with a freshly generated rounded
   gold bubble star. Its initially malformed top point was repaired with imagegen.
7. Generated minimal shaded green/red round button shells and asymmetric gray
   shoulder shells. Removed the concentric white rings and keyboard-key styling;
   A/B/L/R are still real text. The holder remains detached from the book.

Seven new selected textures bring the total to 17, all power-of-two. Full prompts
and selected source paths are in `prompts.json`. New assets use built-in imagegen;
none are cropped from the concept. No game/runtime files were changed this pass.

Revision 3 verification: 29 Playwright checks passed, including motion, paused
pixel stability, single-click selection, page turns, smooth photo curvature,
all seven assets, four landscape sizes and native 2× DPI; no browser errors.
Screenshots were visually checked at reference size, 720p and 2× DPI. These
checks do not certify an exact artistic match: visual approval remains pending.

Current evidence: `output/playwright/storybook-revision-3-reference.png`,
`storybook-revision-3-720p.png`, `storybook-revision-3-hidpi.png`,
`storybook-revision-3-page-turn.png`, and `storybook-revision-3.webm`.
The video records actual browser motion. The preview photograph is still a real
captured game frame, not a live feed. Nothing is connected to game settings.

## Browser annotations — revision 4, 2026-10-01

- Removed the marked upper-left and lower-left page ornaments, plus the star
  beside Picture in the selected chapter tab. The unmarked right ornament stays.
- All printed page UI now shares `wrapContent`: headings, row ribbon, labels,
  values, arrows, rules, photograph and mounts, help text, and divider star. The
  independently animated selection star uses the same `surfacePoint` mapping.
  The curved surface is inverted for input, so row hit areas follow the art.
- Clouds, celestial sprites and cutout hosts remain independent. Chapter tabs
  stay behind the book and the footer remains detached, outside the annotated
  page area as previously requested.
- Replaced the synthetic turning-page gradient/path with the static leaf's own
  texture, contour and printed content. Static and moving states use the same
  compositing stack. A mask from the unprinted sheet leaves the red hardcover
  stationary. Opaque paper backing prevents tabs showing through the leaf.
- Regenerated the selected tab with concave inset red corners, the background
  sun as a plain golden disc (no spiral), and the shoulder button as a rounded
  controller shoulder (no trapezoid). L/R mirror a single blank gray shell.
- A/B share `assets/button-round.png`, a 128×128 neutral grayscale texture.
  Green/red are runtime multiply tints, cached after compositing; letters are
  text. Removed the obsolete separate `button-green.png` / `button-red.png` from
  the prototype assets. Their source images remain in the original imagegen
  output directory. There are now 16 active POT textures, not 17.

Fresh artwork was made with built-in imagegen; full prompts and selected source
paths remain in `prompts.json`. The packer enforces neutral luminance in the
shared button texture. The concept is still a reference, never an asset crop.

Verification: 38 Playwright checks, including a pixel continuity check at page
lift-off, all six warped rows clicked individually, inverse surface mapping,
animation controls, four landscape sizes and 2× DPI. Browser screenshot evidence
uses the `storybook-revision-4-` prefix in `output/playwright/`, including a frame
during the page turn. No game files, controller handling or audio were changed.
The design is still unapproved for integration.

## Browser annotations — revision 5, 2026-10-01

- Replaced all cloud bitmap sampling with three native curve silhouettes. Every
  instance uses the same 2.4-unit navy contour and 9-unit ivory rim; no per-cloud
  resize, stretch or scaling. Extra coverage comes from additional shapes.
- Added broad, continuously rotating day-side sunbeams. Stars/confetti and the
  sun/moon remain independent animated components. Pause freezes all of them.
- Removed the last page-corner ornament, row divider lines, the duplicated
  Picture heading and duplicated setting name in the help area. The left-page
  subtitle and right-page explanation are centered within their content areas.
- The photo shows the entire captured game frame with its original aspect
  ratio. Its frame, mounts and new soft cast shadow share the page projection.
  This is still a captured image, not a live feed or a running game capture.
- Selected chapter shell now uses nine-slice layout, preserving its concave
  corners and equal edge insets. Sun emblem and text are centered as one group.
- All chapter-tab labels use the locally bundled Titan One bubbly face. Page
  typography is unchanged; only the tab labels were requested to change.
- Added a separate red bound spine at the lower page join, with clearance above
  the detached button holder. It stays fixed throughout the page turn.
- Replaced the flattened back-and-forth page squeeze with a full right-to-left
  curved-sheet mesh. Positive lift moves toward the camera and enlarges the
  paper; the binding is fixed. Front/back faces are selected per mesh face, not
  switched all at once. Sheet lighting and cast shadow are separate effects.
- The moving sheet excludes the lower paper stack on BOTH its bottom and outer
  edges. Rest and movement use the same mesh sampler to avoid a rendering pop.
  The demonstration lands on the same proof content; it does not pretend to
  change chapters or game settings. All type/photo components stay on the sheet.
- Edited Brighton with built-in imagegen: left rays red, right rays gold,
  opposite the face halves. The ray ring is offset clockwise around the upright
  face so no individual ray is divided into two colors. Mirrored the short upper
  ray onto the gold side, then lengthened the upper-left red rays while keeping
  the orderly ring, upright face and smooth shading. The random short-ray
  variant was rejected and is recorded only as provenance.
  The selected result is packed into `assets/brighton.png` at 512×512. The exact
  edit prompt and original/new source paths are preserved in `prompts.json`.

Verification: 47 Playwright checks across four landscape sizes and native 2×
DPI, including actual pixel continuity at lift-off/landing, full-frame sampling,
cloud stroke transforms, fixed hinge, preserved sheet width, correct near-camera
projection, and visible independent ray motion. No browser errors. Current
evidence uses `output/playwright/storybook-revision-5-*`. Visual approval remains
the gate; these checks do not certify an exact artistic match.

### Further revision 5 annotations

- Increased the selected chapter shell's shared corner scale from 20 to 36,
  giving top/side insets of about 9 logical pixels (within one pixel of each
  other). Concave corners remain intact. Removed its sun emblem and centered
  the Picture label; the sky sun is unchanged.
- Only setting descriptions are blue. Other labels, captions and notes are
  black, with white lettering retained on active red selections/buttons.
- All menu text, including footer and controller letters, is bubbly Titan One.
  Text width limits preserve the existing row/value/control layout.
- Removed both page headings and moved printed content up 120 logical units.
  Hit regions move with the rows; the bottom selection star clears Brighton.
- All six setting labels now share the same left-aligned anchor, including
  Widescreen and Picture Preset, which previously had special horizontal offsets.
- Confetti uses a separate time-based emitter: randomized initial scatter,
  new births above the screen, positive downward speeds and retirement below
  the screen. No vertical wrapping or looping around original positions.
  Star twinkling remains independent. Pause and seek remain deterministic.

Verification: 47 general checks plus 12 focused checks passed, with
no browser errors. `verify-tab.cjs` covers frame padding, type/color rules,
removed headings, bottom-row clearance, and confetti initialization/lifetime/
motion. The bottom-row selection screenshot confirms clearance visually.

### Lower binding and page-turn attachment

The broad contour fit excluded the final part of the generated book half, so
the moving sheet ended above the visible fold. Its inner contour now blends
to the measured top-sheet endpoint (source y=995 within the content rectangle).
The attachment remains fixed throughout the turn. Curl reverses with the page
side after halfway, preventing the sheet from folding back across its hinge.
The cast shadow is clipped to the top sheet, excluding the lower paper stack.

Replaced the overlaid red chevron with a shallow rounded cover binding UNDER
the paper/cover halves. Its ends no longer sit on top of the paper. The center
stays stable while the single sheet turns. `verify-binding.cjs` checks the
measured endpoint, attachment across six angles, and exact binding pixel
stability; it saves rest/front/back/landed browser frames for visual inspection.

### Historical soundtrack request (superseded by native integration above)

The original request was random Mario Party 6 music when a valid ISO is linked,
without bundling tracks or using substitute audio. After approving integration,
the user also requested this replacement soundtrack inside in-game settings.
That later choice supersedes the earlier proposal to leave gameplay music alone.
The browser prototype remains silent, and local game tests remain muted.

## Styled pages and chapter navigation — 2026-10-02

Historical first proposal below; its invented options and Controls chapter were
rejected. The source-backed correction in the following section supersedes it.

- Picture Preset is first. Its Original / Balanced / Crisp choices update the
  other values in the preview; individual adjustments switch it back to Custom.
- Added separate Sound, Controls, Game and Extras chapter models, options, help
  descriptions and printed illustrations. Each remembers its row and values for
  this browser session. These are proposed UI controls, not claims of supported
  runtime settings. Reloading restores the mock defaults.
- Click chapter handles or the footer shoulder buttons, or use Q/E (PageUp/
  PageDown), to turn in either direction. Up/Down selects; Left/Right or the row
  arrows adjust values. Inputs are ignored during the turn. Reduced-motion
  navigation changes pages immediately. The replay button is still independent.
- Turning sheets now combine the outgoing front and incoming reverse with the
  correct stationary pages beneath. Completion swaps cached matching content,
  not a blank sheet. A resize safely completes an active chapter transition.
- Added fine gold double rules and faint lower-margin star trails as printed
  content, so they follow the same paper deformation as labels and illustrations.
  Removed headings and row separators remain absent; only descriptions are blue.
- Edited the existing generated book half with built-in imagegen to replace the
  few broad page layers with a dense stack of fine leaves. Still one mirrored
  `assets/book-half.png` (1024×1024). `prompts.json` preserves both original and
  edit prompts/source paths. The edited source's alpha is retained during packing.
- Recalibrated the moving top-sheet endpoint to source y=975 in the new packed
  texture. The paper stack and binding remain stationary; only one sheet turns.

Verification: 47 general checks, 12 annotation checks, 3 binding checks and
20 chapter checks passed (82 total). Includes both-direction navigation,
per-chapter state, presets, actual tab/keyboard input, outgoing/incoming pixel
continuity, four landscape sizes and native 2× DPI. No browser errors. Reviewed
all five chapters plus front/back turns visually. Evidence is in
`output/playwright/storybook-chapter-*.png` and the verification JSON beside it.
Artistic approval is still pending. No game code, audio, saves or settings were
connected or changed by this prototype work.

## Source-backed chapters and animated feedback — 2026-10-02

- Removed Controls as requested. Picture, Sound, Game and Extras now contain
  only entries present in the desktop/prelaunch menu in
  `src/gx/ui/settings.cpp`. Removed invented music/output/mute, autosave and
  similar options. Runtime-dependent drivers/monitors and in-game save-state
  entries are not fabricated in this isolated prelaunch proof.
- Picture Preset is first, with the real Original / Classic+ / Modern tiers.
  Custom is derived from individual changes. Picture's 11 rows use two groups
  of six visible slots; Sound has Master Volume and More Sound Effects.
  Game file/folder entries remain inert actions, not fake toggles.
- The turning sheet's top and outer clipping edges follow the top paper,
  excluding the stationary lower stack. The selection star shares the same
  tile, compressed surface mapping and GPU sampler at rest and on the reverse
  of the turning page, avoiding the size/position pop at landing.
- Shortened chapter/replay turns from 1.8 seconds to 640ms. Chapter tabs lift,
  the red ribbon and star travel between rows in 190ms, values pop/nudge with
  their arrows, and descriptions exit then enter over 200ms. Rapid row input
  retargets from the currently visible position. Pause freezes these effects;
  reduced motion uses immediate row/chapter updates.
- Cached paper geometry and fixed ornaments keep selection changes responsive.
  Static print and animated UI use the same connected surface projection.
- Replaced the poorly fitting rectangular/mitered border with a softer rounded
  double rule fitted to smooth paper contours. Top/side/bottom insets use the
  same 27-unit margin, a 7-unit rule gap and extra breathing room at the fold.
  The faint star trail remains in the lower margin.
- Added dark-bottom/light-top day and night gradients, four rising and swaying
  hot-air balloons and 240 distant stars with independent brightness/glow
  cycles. The initially added galaxies were removed on request.
  Existing sun rays, falling confetti, stars, clouds and characters remain
  independently animated.
- Corrected the fore-edge detector: isolated white highlights on the cover and
  lower leaves were being mistaken for the top sheet, producing the wrong side
  slope. It now requires a continuous stretch of pale paper. The same measured
  edge drives the border inset and the moving-sheet clip. A pixel-material test
  checks the side gap down seven heights (about 27 logical units).
- Replaced the Sound and Game placeholders using built-in imagegen. The glossy
  first versions and flat second versions were rejected; the current pair use
  the actual Brighton and Twila sprites as explicit style references. Both are
  packed locally into `assets/chapter-sound.png` and `assets/chapter-game.png`
  at 512×512. Full current and rejected prompts/source paths are preserved in
  `prompts.json`; the actual icons are transparent illustrated cutouts.

Focused verification is in `verify-feedback.cjs`, alongside general, chapter,
annotation, source-fidelity and binding checks. Latest render evidence uses
`output/playwright/storybook-latest-*.png` and
`storybook-motion-selection.png`. These tests check behavior and geometry,
not artistic approval. This remains a silent, visual-only candidate; no game
code, runtime settings, saves, controller handling or audio have been wired.

## Animation compositing correction — 2026-10-02

The user approved the design for integration, then reported an animation overlap
beside the FPS-counter value arrows. Native integration is still pending.

The resting cache had retained row text and arrows underneath their animated
copies. It now contains only paper and fixed ornaments. All row lettering,
values and arrows have one interaction layer, composed with the ribbon before
the common page warp. Text is recolored through the ribbon's exact alpha mask
instead of painting a second glyph over the first. Arrow kicks keep a single
silhouette throughout their movement.

`verify-compositing.cjs` checks the actual pixels at 960-, 1280- and 1920-pixel
viewport widths. Suppressing live arrows leaves zero cached arrow pixels,
and a frame sweep verifies that the kicked arrow returns to the same position.
All 124 checks passed across the general, annotations, chapters, binding,
source-fidelity, feedback and compositing suites after this correction.
Evidence: `output/playwright/storybook-compositing-selection.png` and
`storybook-compositing-value.png`.

## Audit fixes and code structure — 2026-10-02

Behavior fixes from a review of the running preview:

- Pausing during a page turn froze the sheet mid-air and blocked all input until
  Play. Pausing now lands the turn first. A turn started while paused (chapter
  change or replay) still animates, then returns to paused; it used to switch
  animation back on. `proof.seek()` without a progress lands an in-flight chapter
  turn instead of orphaning it.
- Master Volume stops at 0% and 100% (the `clamp` row flag), matching
  `std::clamp(value, 0, 100)` in `settings.cpp`. Other rows still cycle.
- Window Size and Display show the native labels "Auto (keep current)" and
  "Auto (highest refresh)": "Auto" at the normal size, the parenthetical as a small
  second line.
- Left/Right no longer reprints the whole book: 68 ms per press at 1920×1080 @2x
  became about 7 ms including the redraw. The fully printed turn copies are marked
  stale and rebuilt just before the next turn. A paused page no longer redraws
  every frame.
- Shortcuts ignore Ctrl/Alt/Cmd combinations, and the pointer cursor shows only
  over real click targets.
- A live region announces chapter, row, value and help text. The canvas has
  `role="application"`; the Pause/Play button no longer pairs a changing label
  with `aria-pressed`.
- No page scroll at 1280×800. The gallery drops to two columns under 900 px and one
  on phones; short landscape phones get a full-height stage; portrait phones get a
  rotate hint.
- `?seed=N` pins the confetti; `?still` and `?capture` use a fixed seed so captures
  from separate loads agree.
- `serve.cjs` answers 400 to a malformed URL instead of exiting, and serves `.js`
  as `text/javascript`.

Structure, with no rendering change: readable formatting; an explicit drawing
context passed to every helper instead of one swapped global; rows as objects and
menu rules found by label instead of by index; paper measurement and clouds in
their own files; the turn buffers grouped as one `book`; named click zones shared
by clicks and the pointer cursor. `export-native.cjs` and `inspect-occlusion.cjs`
call `proof.nativeExport()` and `proof.debugBuffers()` instead of patching
`preview.js` source text.

The restructure was compared pixel for pixel with the previous build across 72
states (every chapter, selection/value/turn animation frames, hover, disabled rows,
1× and 2×, gallery tiles): all identical. `export-native.cjs` output, all 23 files
including `storybook_art_data.hpp`, is byte-identical.

Two visible changes followed:

- The selected tab showed a faint 3×3 grid. Its nine-slice boundaries fell between
  tile pixels (36 units × 512/245 = 75.2 px), so neighbouring slices each covered
  part of the seam pixel and left it about 20% transparent. Boundaries now snap to
  whole pixels, with corners rounded down so they never exceed their nominal size.
- Values had room for only 138 units between the arrows, so 14 of them printed
  smaller. Chosen by the user: the left arrow moved 20 units left (575 → 555) and
  values are centered between the arrows (671 → 655.5) with 169 units of room.
  "Original", "Classic+", "Ultra 1.5x", "Top Left", "Top Right", "640 × 480",
  "960 × 720" and "Primary" now print at full size. "1280 × 960", "Windowed",
  "Fullscreen" and "1920 × 1440" are 4–10% smaller; "Bottom Left" and
  "Bottom Right" 18% and 26% (previously up to 40%). The arrow stays 13.5 units
  clear of the longest label ("More Sound Effects") at rest, and 6.5 at the peak
  of its kick; a 25-unit move made them touch.

Against the original snapshots, these change only the selected tab and the
value-and-arrow column; everything else is pixel-identical, and the export is
unchanged. All suites pass: the 125 existing checks, plus 30 for this section in
`verify-audit.cjs`.
