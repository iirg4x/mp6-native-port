# Menu artwork

These thirteen illustrations were generated with the built-in imagegen tool for
the full-screen settings menu. The v3 direction follows contemporary Mario Party:
simple illustrated pictograms, thin white separation edges, and softly shaded
Brighton/Twila character heads. Light menu panels and pink/yellow focus states
are implemented in `res/rml/settings.rcss`, not baked into images.
They are generated artwork, not extracted retail assets.

Unmodified transparent PNGs and the exact prompt set are retained in
`docs/design/settings-icons/`. `tools/prepare_menu_icons.py` only downsizes
the category `*-v3.png` and option `option-*-v1.png` sources into 256-pixel
runtime PNGs, preserving transparency. Run it with Pillow after replacing a
source image. All thirteen runtime images use 3.25 MiB of uncompressed RGBA.

Category icons are navigation-only. Each illustrated option has a distinct
asset; other cards use functional previews (ratio, buttons, volume, save slot).
No fallback category icon is stamped across unrelated options. Option-art
prompts are in `docs/design/settings-icons/prompts-options-v1.json`.

The settings menu uses native controls and retained transforms; the PNGs are
icons, not screenshots of controls. The controller preview uses the same layout
model as the playable Android touch controller.
