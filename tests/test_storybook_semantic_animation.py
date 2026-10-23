"""Static contract for native-storybook animation suppression."""

from pathlib import Path
import re
import unittest

from tests.source_tokens import compact_source

ROOT = Path(__file__).resolve().parents[1]


class StorybookSemanticAnimationTests(unittest.TestCase):
    def test_retained_sky_keeps_palette_slots_stable_during_motion(self):
        view = compact_source((ROOT / "src/gx/ui/storybook_view.hpp").read_text(encoding="utf-8"))
        start = view.index(compact_source("for(const auto& dot:dots)"))
        dots = view[start:view.index(compact_source("// A complete night sky"), start)]
        # Only the immutable birthplace may remove an instance from the mesh.
        # Filtering after drift would change the cached palette slot mapping.
        self.assertLess(dots.index(compact_source("float x=dot.x")), dots.index("if(x<860)continue;"))
        self.assertLess(dots.index("if(x<860)continue;"), dots.index("x+=std::sin"))
        for palette, end in (("dotMesh", "// A complete night sky"),
                             ("starMesh", "mLayer=Layer::Day;"),
                             ("balloonMesh", "if(balloonMesh)end_sky_mesh")):
            start = view.index(f"if({palette}){{")
            branch = view[start:view.index(compact_source(end), start)]
            self.assertIn(compact_source("const float visible=hidden?0.f:1.f;"), branch)
            # Dynamic visibility is zero coverage, never omitted palette slots.
            self.assertLess(branch.index("continue;"), branch.index("if(hidden)continue;"))
        self.assertIn(compact_source("SkyMeshGroup* starMesh=dotMesh;"), view)
        self.assertIn(compact_source("SkyMeshGroup* balloonMesh=dotMesh;"), view)
        self.assertEqual(view.count("end_sky_mesh(*"), 1)
        transform = view[
            view.index(compact_source("auto& transform=group.batches")) : view.index(
                compact_source("void end_sky_mesh")
            )
        ]
        self.assertLess(transform.index("if(!ink.alpha){transform={};return;}"), transform.index("std::cos(angle)"))

    def test_cloud_drift_stays_subpixel_and_hosts_keep_adjacent_batching(self):
        view = compact_source((ROOT / "src/gx/ui/storybook_view.hpp").read_text(encoding="utf-8"))
        foreground = view[
            view.index(compact_source("void foreground(float t)")) : view.index(
                compact_source("void footer()")
            )
        ]
        self.assertIn("x-6+drift", foreground)
        self.assertNotIn("mOffset", foreground)
        self.assertNotIn("retained(", foreground)

    def test_skipped_mask_margins_have_exact_zero_coverage(self):
        from PIL import Image

        with Image.open(ROOT / "res/menu/storybook/zigzag-mask.png") as image:
            image = image.convert("RGBA")
            for y in range(image.height):
                self.assertEqual(image.getpixel((0, y)), (0, 0, 0, 255))
                self.assertEqual(image.getpixel((image.width - 1, y)), (255, 255, 255, 255))
        view = compact_source((ROOT / "src/gx/ui/storybook_view.hpp").read_text(encoding="utf-8"))
        self.assertIn("tile.uv.w==0.f", view)
        self.assertIn("mLayer==Layer::Day&&tile.uv.x==1.f", view)
        self.assertIn("mLayer==Layer::Night&&tile.uv.x==0.f", view)
        self.assertIn("if(oppositeSolidMargin)continue;", view)

    def test_invisible_native_footer_does_not_generate_a_blur_texture(self):
        css = (ROOT / "res/rml/settings.rcss").read_text(encoding="utf-8")
        self.assertIn("#window.native-storybook settings-footer { box-shadow: none; }", css)
        self.assertIn("#window.native-storybook settings-footer * { opacity: 0; pointer-events: none; }", css)

    def test_hidden_startup_layout_uses_the_runtime_font_size(self):
        source = (ROOT / "src/gx/ui/settings.cpp").read_text(encoding="utf-8")
        start = source.index("void SettingsWindow::preload_resources()")
        end = source.index("SettingsWindow::SettingsWindow(", start)
        preload = source[start:end]
        self.assertIn("aurora::window::get_window_size()", preload)
        self.assertIn("context->SetDimensions(originalDimensions)", preload)
        self.assertLess(preload.index("native_fb_width"), preload.index("StorybookView::preload_resources"))
        constructor = source[end:source.index("void SettingsWindow::build_shell()", end)]
        self.assertIn("mFontPixels=settings_font_pixels(", constructor)
        self.assertLess(constructor.index("mFontPixels=settings_font_pixels("), constructor.index("build_shell();"))
        self.assertIn("warmup.mDocument->UpdateDocument();", preload)
        self.assertLess(preload.index("warmup.mDocument->UpdateDocument();"),
                        preload.index("context->Update();"))
        self.assertIn("const float fontPx = settings_font_pixels(size, dpi);", source)
        for forbidden in ("->Show(", ".show(", "push_document", "cfg_save", "->enter("):
            self.assertNotIn(forbidden, preload)

    def test_live_preview_has_only_one_layout_owner(self):
        source = (ROOT / "src/gx/ui/settings.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/gx/ui/settings.hpp").read_text(encoding="utf-8")
        view = compact_source((ROOT / "src/gx/ui/storybook_view.hpp").read_text(encoding="utf-8"))
        # The obsolete display:none preview always measures zero. Retrying
        # its width/height writes dirtied the complete document every present.
        for obsolete in ("mPreviewImage", "mPreviewFrameWidth", "mPreviewViewportWidth"):
            self.assertNotIn(obsolete, source)
            self.assertNotIn(obsolete, header)
        self.assertIn("aurora::rmlui::scene_dimensions()", view)
        self.assertIn("if(scene!=mLastScene)", view)
        self.assertIn("++mPhotoRevision", view)
        self.assertIn("mp6scene://live", view)

    def test_native_hidden_branches_disable_only_legacy_motion(self):
        css = (ROOT / "res/rml/settings.rcss").read_text(encoding="utf-8")
        start = css.index("#window.native-storybook story-backdrop,")
        end = css.index(".native-storybook content pane.settings-detail", start)
        suppression = css[start:end]

        for selector in (
            "story-backdrop",
            "story-book",
            "story-decoration",
            "story-cast",
            "chapter-heading",
            ".chapter-emblem",
            "story-preview",
            "controller-guide",
            "settings-guide",
            "tab-bar",
            "settings-footer",
            "content pane.settings-list",
        ):
            self.assertIn(f"#window.native-storybook {selector}", suppression)

        self.assertRegex(
            suppression,
            r"\{\s*animation:\s*none;\s*transition:\s*none;\s*\}",
        )
        # Native rendering and the DOM-rendered detail pane retain their own
        # motion semantics; neither is covered by the suppression selector.
        self.assertNotIn("storybook-view", suppression)
        self.assertNotIn("pane.settings-detail", suppression)


if __name__ == "__main__":
    unittest.main()
