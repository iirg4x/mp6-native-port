/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "menu_bar.hpp"

#include <RmlUi/Core.h>

/* [MP6] dropped: #include "achievements.hpp" */
#include "aurora/rmlui.hpp"
#include "launcher_state.hpp" /* [MP6] replaces <port/settings.h> */
#include "imgui.h"
#include "modal.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <chrono>
#include <cmath>

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {

    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/tabbing.rcss" />
    <link type="text/rcss" href="res/rml/popup.rcss" />
</head>
<body>
    <popup id="popup" />
</body>
</rml>
)RML";

    /* [MP6] rewrites the `"res/` hrefs to our resolved res/ base. */
    Rml::String formatted_source(const Rml::String &raw)
    {
        char buf[4096];
        mp6::ui::format_document_source(raw.c_str(), buf, sizeof(buf));
        return Rml::String(buf);
    }

}

MenuBar::MenuBar()
    : Document(formatted_source(kDocumentSource))
    , mRoot(mDocument->GetElementById("popup"))
{
    mTabBar = std::make_unique<TabBar>(mRoot,
        TabBar::Props {
            .onClose =
                [this] {
                    // mDoAud_seStartMenu(kSoundMenuClose); // TODO
                    hide(false);
                },
            .autoSelect = false,
        });
    mTabBar->add_tab("Settings", [this] { push(std::make_unique<SettingsWindow>()); });
    /* [MP6] their Achievements tab dropped -- no achievements system in this
     * port. (Their commented Warp/Reset tabs left out with it.) */
    // TODO PC
    // mTabBar->add_tab("Reset", [this] {
    //     mTabBar->set_active_tab(-1);
    //     const auto dismiss = [](Modal &modal) { modal.pop(); };
    //     push(std::make_unique<Modal>(Modal::Props{
    //         .title = "Reset Game",
    //         .bodyRml = "Unsaved progress will be lost.<br/>"
    //                    "<span class=\"tip\">Tip: You can also reset by holding Start+X+B</span>",
    //         .actions =
    //             {
    //                 ModalAction{
    //                     .label = "Cancel",
    //                     .onPressed =
    //                         [this, dismiss](Modal& modal) {
    //                             // mDoAud_seStartMenu(kSoundWindowClose); // TODO PC
    //                             dismiss(modal);
    //                         },
    //                 },
    //                 // ModalAction{
    //                 //     .label = "Reset",
    //                 //     .onPressed =
    //                 //         [this, dismiss](Modal& modal) {
    //                 //             // mDoAud_seStartMenu(kSoundClick); // TODO PC
    //                 //             // if (fpcM_SearchByName(fpcNm_LOGO_SCENE_e)) {
    //                 //             //     dismiss(modal);
    //                 //             //     return;
    //                 //             // }
    //                 //             dismiss(modal);
    //                 //             hide(false);
    //                 //         },
    //                 // },
    //             },
    //         .onDismiss = dismiss,
    //         .icon = "question-mark",
    //     }));
    // });
    mTabBar->add_tab("Quit", [this] {
        mTabBar->set_active_tab(-1);
        const auto dismiss = [](Modal &modal) { modal.pop(); };
        push(std::make_unique<Modal>(Modal::Props{
            .title = "Quit Mario Party 6", /* [MP6] our app name */
            .bodyRml = "Unsaved progress will be lost.",
            .actions =
                {
                    ModalAction{
                        .label = "Cancel",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                // mDoAud_seStartMenu(kSoundWindowClose); // TODO PC
                                dismiss(modal);
                            },
                    },
                    ModalAction{
                        .label = "Quit",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                // mDoAud_seStartMenu(kSoundClick); // TODO PC
                                dismiss(modal);
                                mp6::ui::request_quit(); /* [MP6] their TODO'd IsRunning=false, wired to our loop flag */
                            },
                    },
                },
            .onDismiss = dismiss,
            .icon = "question-mark",
        }));
    });

    // Hide document after transition completion
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") && Document::visible()) {
            Document::hide(mPendingClose);
        }
    });
}

void MenuBar::show()
{
    Document::show();
    mRoot->SetAttribute("open", "");
    mTabBar->set_active_tab(-1);
    if (!mTabBar->focus_tab(mFocusedTabIndex)) {
        mTabBar->focus();
    }
}

void MenuBar::hide(bool close)
{
    mFocusedTabIndex = mTabBar->focused_tab_index();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
    }
}

void MenuBar::update()
{
    update_safe_area();
    Document::update();
}

void MenuBar::update_safe_area() noexcept
{
    if (mDocument == nullptr || mTabBar == nullptr) {
        return;
    }

    // Avoid ImGui menu bar if shown
    if (const auto *viewport = ImGui::GetMainViewport(); viewport != nullptr && mTopMargin != viewport->WorkPos.y) {
        mTopMargin = viewport->WorkPos.y;
        mRoot->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(mTopMargin, Rml::Unit::DP));
    }

    Rml::Context *context = mDocument->GetContext();
    Insets safeInsets = safe_area_insets(context);
    safeInsets = {
        0.0f,
        std::round(safeInsets.right),
        0.0f,
        std::round(safeInsets.left),
    };
    if (safeInsets == mTabBarPadding) {
        return;
    }

    mTabBarPadding = safeInsets;
    auto *tabBar = mTabBar->root();
    tabBar->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(safeInsets.right, Rml::Unit::PX));
    tabBar->SetProperty(Rml::PropertyId::PaddingLeft, Rml::Property(safeInsets.left, Rml::Unit::PX));
    if (auto *close = tabBar->QuerySelector("close")) {
        close->SetProperty(
            Rml::PropertyId::Right, Rml::Property(safeInsets.right + 8.0f * context->GetDensityIndependentPixelRatio(), Rml::Unit::PX));
    }
}

bool MenuBar::visible() const
{
    return mRoot->HasAttribute("open");
}

bool MenuBar::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    /* [MP6] their first-run wasPresetChosen gate dropped -- no preset flow. */
    if (cmd == NavCommand::Cancel && visible()) {
        // mDoAud_seStartMenu(kSoundMenuClose); // TODO PC
        hide(false);
        return true;
    }
    return Document::handle_nav_command(event, cmd);
}

bool MenuBar::focus()
{
    return mTabBar->focus();
}

} // namespace mp6::ui
