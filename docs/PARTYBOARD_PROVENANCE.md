# PARTYBOARD PROVENANCE NOTICE (L4 ripped-UI lane)

*This notice is a hard requirement of the L4 lane and must travel with the
code it describes.*

## What happened

At **explicit, repeated user direction** (the L4 lane brief, quoting the
user verbatim: "I WANT THE IMPLEMENTATION RIPPED FROM THE REPO … RIP IT,
STITCH IT, ADAPT IT TO MP6"), the launcher/settings UI implementation of
the **partyboard** project (the Mario Party 4 PC port) was **copied
directly into this repository** and adapted to MP6. The project's prior
clean-room rule (L1–L3: partyboard read as a spec only, all code
re-expressed independently) was **revoked by the user for this lane**.
The L1–L3 ImGui reimplementation (`platform/gx/launcher_menu.cpp`) was
deleted in the same change (git history retains it).

## Source

| Fact | Value |
|---|---|
| Repository | `mariopartyrd/partyboard` (local checkout `external_refs/repos/partyboard`, read-only) |
| Commit copied from | `9f607425e37703adc2650c799faf5175c62a1907` ("Update aurora", 2026-06-13) |
| License | **NONE.** Verified at copy time: no `LICENSE`/`COPYING`/license text anywhere in the repo; the GitHub repository's license field is empty. Default copyright applies; no redistribution rights have been granted by the authors. |
| Destination | This **private local repository** only. Files were copied at explicit user direction; this notice documents that decision and its scope. **Do not publish or redistribute this repository (or these files) without resolving the upstream licensing situation first.** |
| Authorship credit | The UI framework files carry `// Credits: TwilitRealm` upstream; README credits Mario Party R&D contributors, the aurora developers, "[ImWhoreHay] for the font", and "justcamtro for designing the assets". All such credit lines are preserved in the copies. |

The rendering substrate underneath the copied UI is **not** part of this
notice's concern: aurora (including its RmlUi integration module) is MIT
licensed, RmlUi is MIT licensed, and our aurora pin (`1dde08f`) already
contained the module — see `docs/L4_RIPPED_UI.md` for the substrate
decision. Only the partyboard-authored files below are unlicensed
material.

## File map (every ripped file)

### Code: `src/port/ui/<name>` → `platform/gx/ui/<name>`

Copied and adapted (namespace `partyboard::ui` → `mp6::ui`, includes
retargeted, `[MP6]`-marked content adaptations; each file carries a
RIPPED-provenance header + the original Credits line):

| # | File | Adaptation level |
|---|---|---|
| 1 | `ui.hpp` / `ui.cpp` | light (font paths → `res/fonts/`, res base resolution, controller-toast gate always-on, debugger include dropped) |
| 2 | `nav_types.hpp` | mechanical only |
| 3 | `event.hpp` / `event.cpp` | mechanical only |
| 4 | `component.hpp` / `component.cpp` | mechanical only |
| 5 | `document.hpp` / `document.cpp` | mechanical only |
| 6 | `button.hpp` / `button.cpp` | mechanical only |
| 7 | `select_button.hpp` / `select_button.cpp` | mechanical only |
| 8 | `bool_button.hpp` / `bool_button.cpp` | mechanical only |
| 9 | `number_button.hpp` / `number_button.cpp` | mechanical only |
| 10 | `string_button.hpp` / `string_button.cpp` | mechanical only |
| 11 | `pane.hpp` / `pane.cpp` | mechanical only |
| 12 | `tab_bar.hpp` / `tab_bar.cpp` | mechanical only |
| 13 | `window.hpp` / `window.cpp` | light (document-source hrefs routed through our res-base resolver; magic_enum include dropped) |
| 14 | `modal.hpp` / `modal.cpp` | mechanical only |
| 15 | `input.hpp` / `input.cpp` | mechanical only (their aurora PAD-extension API exists in our aurora pin) |
| 16 | `overlay.hpp` / `overlay.cpp` | light (our flat config for FPS chip keys; magic_enum replaced by a local name table; achievements include dropped) |
| 17 | `menu_bar.hpp` / `menu_bar.cpp` | medium (Achievements tab + first-run preset gate dropped; Quit wired to our loop flag; compiled but not routed at runtime — no in-game menu surface yet) |
| 18 | `graphics_tuner.hpp` / `graphics_tuner.cpp` | medium (their two MP4 graphics settings stubbed out — component compiled per the full-framework directive, unrouted at runtime) |
| 19 | `prelaunch.hpp` / `prelaunch.cpp` | heavy (disc-image concept → our content-root concept; their globals → our loop flags; hero → runtime-decoded MP6 wordmark; single-OK restart-modal branch; see file headers) |
| 20 | `settings.hpp` / `settings.cpp` | heavy (their window/row-builder skeleton kept; tab CONTENT replaced with our settings model: VIDEO/AUDIO/GAME/ABOUT over `mp6_config.json`) |

NOT copied: `compat.cpp` (their C bridge; ours is `launcher_core.cpp`),
`achievements.*`, `controller_config.*`, `preset.*` (systems our port
does not have), `portmain.cpp` (read as wiring reference only, per the
lane brief).

OURS (not ripped, listed for completeness): `platform/gx/ui/
launcher_core.cpp` + `launcher_state.hpp` — the kept L1–L3 MP6 glue
(config model, automation-skip mode decision, settings application,
HSF wordmark decode) carried forward from the deleted launcher_menu.cpp.

### Stylesheets: `res/rml/<name>` → `res/rml/<name>` (copied VERBATIM)

`window.rcss`, `tabbing.rcss`, `prelaunch.rcss`, `overlay.rcss`,
`popup.rcss`, `tuner.rcss` — each with a provenance header comment
prepended; `prelaunch.rcss` additionally carries a clearly-marked
`[MP6] ADDITIVE BLOCK` at the end (originally two rules for our
watermark/hero fallback elements, joined by a third — a mobile-breakpoint
override for `.watermark` — in the A5 lane; see this file's closing
section). No other rule was altered.

### Fonts: `res/<name>` → `res/fonts/<name>` (copied VERBATIM, per the user's blanket order)

Exactly the five faces partyboard's `ui.cpp` loads:

| File | Family | License status |
|---|---|---|
| `AlegreyaSC-Regular.ttf` | Alegreya SC | SIL OFL 1.1 (Google Fonts family) — redistributable |
| `AlegreyaSC-Bold.ttf` | Alegreya SC | SIL OFL 1.1 — redistributable |
| `MaterialSymbolsRounded-Regular.ttf` | Material Symbols Rounded | Apache-2.0 (Google) — redistributable |
| `N64Party-Monochromatic.otf` | "N64 Party" | **fan font, no license text**; partyboard's README credits ImWhoreHay "for the font" (gifted to that project). Copied at explicit user direction; private local repo only. |
| ~~`FOT-NewRodin Pro DB.otf`~~ (REMOVED) | FOT-NewRodin Pro | **Commercial (Fontworks), no redistribution rights — REMOVED before publication.** Replaced by Inter (SIL OFL) as the UI body font; all `.rcss` body rules re-pointed from `"FOT-NewRodin Pro"` to `"Inter"`. |
| `Inter-Regular.ttf` / `Inter-Bold.ttf` | Inter | SIL OFL 1.1 (The Inter Project Authors) — redistributable. The open replacement for FOT-NewRodin; see res/fonts/LICENSES.txt. |

Their remaining `res/` fonts (`Inter-*`, `NotoMono-Regular`,
`N64Party.otf` color variant) and images (`logo.png`, `prelaunch-bg.png`,
`icon.png`) were **not** copied — nothing in the ripped UI loads them, and
our branding stays the runtime-decoded, never-committed disc art
(wordmark/watermark), exactly as in L2/L3.

## Adaptation ground rules used

1. Every ripped file keeps its upstream `// Credits: TwilitRealm` line and
   gains a header naming the source repo+commit, the user directive, and
   the no-license status.
2. Mechanical changes (namespace, include paths) are not marked inline;
   all content-level changes are marked `[MP6]`.
3. Their widget/framework semantics, animations, and stylesheet values
   are preserved as coded; only the SETTINGS CONTENT (our config model)
   and the disc-picker→content-root mapping differ, plus the branding
   swap to our runtime-decoded disc art.
4. `docs/L4_RIPPED_UI.md` carries the stitch map, substrate decision, and
   gate evidence for the change this notice covers.

## A4 addendum (2026-07-17): android CMake read as reference, nothing new ripped

The A4 lane (`docs/A4_ANDROID_UI.md` — Android launcher UI, first-run content
onboarding, APK size) enabled the already-ripped RmlUi launcher on the Android
build and added one new, unripped file: `platform/gx/ui/content_setup.cpp` /
`.hpp` (the first-run "Game Content Setup" dialog). It is **our own code**,
built on classes and stylesheet rules already covered by this notice
(`WindowSmall`/`Button` from the ripped `window.hpp`/`button.hpp`; the
`.modal-*`/`.verification-*` CSS classes already present in the ripped,
verbatim-copied `window.rcss`, originally partyboard's own disc-hash-
verification modal styling) — no new file map entry is needed.

`external_refs/repos/partyboard`'s top-level `CMakeLists.txt` was read **as a
build-recipe reference only, not copied**, the same treatment this notice
already gives `portmain.cpp` ("read as wiring reference only, per the lane
brief"): it confirmed `AURORA_ENABLE_RMLUI` is set unconditionally (not
Android-gated) in a project that ships the same ripped UI framework on
Android in production, and that their `ANDROID` branch builds a single
`SHARED` library named `main` for `SDLActivity` to `dlsym` — a shape this
port's own `libmain.so` bootstrap already used independently since U-A2, not
something copied from this read. No CMake text, build logic, or file was
copied from it; `tools/build.py` remains the sole build brain for this port,
unchanged in kind by this observation.

## A5 addendum (2026-07-17): mobile aspect fix, one new rule in our own additive block

The A5 lane (`docs/A5_LAUNCHER_ASPECT.md` — launcher layout on phone/tablet
aspect ratios) added exactly one new CSS rule, entirely inside the
already-declared `[MP6] ADDITIVE BLOCK` at the end of `res/rml/prelaunch.rcss`
— a `@media (max-height: 640dp)` override that hides `.watermark` (our own
floating character-art element, not partyboard's) on the same breakpoint
partyboard's own verbatim rules already use for their mobile layout. No
partyboard-authored rule was read, copied, or modified for this — the fix
is scoped entirely to content this notice already attributes to MP6. The
actual root cause fixed by this lane was in our own C glue
(`platform/gx/aurora_bridge.c`, `platform/gx/ui/launcher_core.cpp`,
`platform/main_native.c`): the RmlUi launcher's presentation dimensions were
being computed from the same `AURORA_VIEWPORT_FIT`-fitted framebuffer size
the GAME's 4:3 GX viewport uses, so the launcher was quietly composed for a
letterboxed 4:3 sub-rectangle instead of the real window/display surface —
see that doc for the full analysis, including how partyboard's own
`portmain.cpp` calls the same `AuroraSetViewportPolicy` API at the same
point in boot but defaults its equivalent config key
(`video.lockAspectRatio`, their `src/port/settings.cpp`) to `false`, which
is why their launcher doesn't hit this by default.
