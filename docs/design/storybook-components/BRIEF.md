# Storybook component proof — approval gate

Scope: visual reconstruction only. Do not connect settings, input, game rendering,
or builds until the user approves the reconstructed screen. Existing runtime work
is paused, not reverted.

Reference: `../settings-storybook-approved.png` (the approved minimal-button version).
No redesign. Preserve the original proportions, typography, colors and placement.
Landscape only; scale the complete reference layout uniformly.

Latest explicit amendments: the backdrop is only yellow and blue color layers;
the zigzag is a separate alpha mask; stars/confetti are particle instances;
the sun and moon are independently animated sprites. These supersede the baked
background decoration in the original concept. Do not bake them into a backdrop.

## Design discipline

Intent: a Mario Party 6 player adjusts the party through a playful picture book.
Domain: storybook chapters, paper cutouts, Brighton, Twila, day/night, stars.
Palette: sunshine yellow, cobalt midnight, ivory paper, red cover/ribbon, gold trim,
navy ink and pale blue clouds, sampled from the approved reference.
Depth: the reference's light paper shading, not new texture or dramatic shadows.
Surfaces: separate backdrop, cover, page leaves, photo, character cutouts and footer.
Typography: preserve reference letterforms in this visual proof; no substitute font.
Spacing: reference-pixel measurements, uniform landscape scaling, no adaptive reflow.
Signature: neutral book pages in front of the zigzag day/night world, with Brighton
and Twila independent of the book. No launcher cards or floating footer controls.

## Component boundary

Separate backdrop halves/decorations; book cover and page leaves; chapter tabs;
heading emblem and typography; selection ribbon and star; row labels, values and
chevrons; preview image and photo corners; help typography and divider; Brighton,
Twila, cloud cutouts; detached footer holder and individual button prompts.

All exported textures use power-of-two width AND height. Preserve artwork size and
aspect ratio; transparent padding must never be stretched into the content area.
Store a content rectangle and layout anchor for every asset. Preview screenshots
are not runtime textures and retain the reference viewport ratio.

## Acceptance

Compare the reconstruction to the approved reference at the reference size.
Show both the assembled proof and the separated component view. Verify dimensions
automatically. Do not claim that visual approval also approves runtime wiring.
