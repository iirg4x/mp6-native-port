/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
/* [MP6] adaptation summary (the second-most-adapted ripped file):
 *   - their DISC IMAGE concept (SDL file picker, background hash
 *     verification thread + progress/warning modals, update checker) maps
 *     to our CONTENT ROOT concept: the first menu button relabels
 *     "Play" <-> "Set Content Root" and, with no content, jumps to the
 *     Settings GAME tab with the content-root row focused;
 *   - their PartyBoard_IsGameLaunched / IsRunning globals become our
 *     request_play()/request_quit() loop flags (launcher_state.hpp);
 *   - their hero (logo.png + team eyebrow) becomes our disc-decoded MP6
 *     wordmark (mp6tex://wordmark runtime texture -- NO Nintendo art in
 *     the repo) with a text fallback, plus the faded mario_ex watermark;
 *   - their HuPadInit() call position is kept as a direct aurora PADInit()
 *     (installs default keyboard bindings; idempotent for the game's own
 *     later HuPadInit);
 *   - SUPPORTS_PROCESS_RESTART is constexpr false here, selecting their
 *     single-OK "Apply Options" modal branch verbatim (body text ours);
 *   - everything else (intro stagger, open/close transitions, focus/nav
 *     model, restart-modal suppress/re-arm rules) is their code as ripped.
 */
// Credits: TwilitRealm

#include "prelaunch.hpp"

#include "content_setup.hpp"  /* [MP6] A4: first-run content onboarding dialog */
#include "launcher_state.hpp" /* [MP6] replaces <port/settings.h> + prelaunch state */
#include "modal.hpp"
#include "settings.hpp"

#include "lib/logging.hpp" /* [MP6] aurora internal, via -I AURORA_ROOT */
#include <dolphin/pad.h>   /* [MP6] PADInit (their HuPadInit position) */
#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <string_view>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {
    aurora::Module PrelaunchLog { "mp6::ui::prelaunch" };

    /* [MP6] SUPPORTS_PROCESS_RESTART: partyboard defines this per-platform;
     * our port has no in-process restart, so the single-OK modal branch of
     * their code is the live one. */
    constexpr bool kSupportsProcessRestart = false;

    /* [MP6] document skeleton: their structure with the .background div
     * (their bundled prelaunch-bg.png art) removed, the hero img swapped
     * for our runtime-decoded wordmark, and the faded watermark added.
     * The hero/menu/disc-info/version-info blocks and every intro-item
     * delay class are theirs verbatim. The two BRANDED variants exist
     * because a missing wordmark (no disc content yet) must fall back to
     * text rather than a broken <img>. */
    const Rml::String kDocumentSourceWithWordmark = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/prelaunch.rcss" />
</head>
<body>
    <div class="gradient" />
    <img class="watermark" src="mp6tex://watermark" />
    <content id="root" open>
        <menu>
            <hero class="intro-item delay-0">
                <div class="eyebrow"><span>Mario Party 6</span> native port</div>
                <img src="mp6tex://wordmark" />
            </hero>
            <div id="menu-list" />
        </menu>
        <disc-info class="intro-item delay-4">
            <div id="disc-status">
                <icon />
                <span id="disc-status-label" />
            </div>
            <span id="disc-version" class="detail" />
        </disc-info>
        <version-info class="intro-item delay-5">
            <div class="version">Version <span id="version-text"></span></div>
        </version-info>
    </content>
</body>
</rml>
)RML";

    const Rml::String kDocumentSourceTextHero = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/prelaunch.rcss" />
</head>
<body>
    <div class="gradient" />
    <content id="root" open>
        <menu>
            <hero class="intro-item delay-0">
                <div class="eyebrow"><span>Mario Party 6</span> native port</div>
                <div class="hero-fallback">MARIO PARTY 6</div>
            </hero>
            <div id="menu-list" />
        </menu>
        <disc-info class="intro-item delay-4">
            <div id="disc-status">
                <icon />
                <span id="disc-status-label" />
            </div>
            <span id="disc-version" class="detail" />
        </disc-info>
        <version-info class="intro-item delay-5">
            <div class="version">Version <span id="version-text"></span></div>
        </version-info>
    </content>
</body>
</rml>
)RML";

    /* [MP6] res-base href/src rewrite (see launcher_state.hpp). */
    Rml::String formatted_source(const Rml::String &raw)
    {
        char buf[4096];
        mp6::ui::format_document_source(raw.c_str(), buf, sizeof(buf));
        return Rml::String(buf);
    }

    Rml::String select_document_source()
    {
        /* wordmark_available() also triggers the one-time disc-asset decode
         * (the [LAUNCHER] wordmark/watermark log lines, same as L2). */
        if (wordmark_available() && watermark_available()) {
            return formatted_source(kDocumentSourceWithWordmark);
        }
        return formatted_source(kDocumentSourceTextHero);
    }

} // namespace

/* Their helper, kept verbatim. */
void apply_intro_animation(Rml::Element *element, const char *delay_class)
{
    if (element == nullptr || delay_class == nullptr) {
        return;
    }
    element->SetClass("intro-item", true);
    element->SetClass(delay_class, true);
}

Prelaunch::Prelaunch()
    : Document(select_document_source())
{
    mRoot = mDocument != nullptr ? mDocument->GetElementById("root") : nullptr;

    PADInit(); /* [MP6] their HuPadInit() position -- default keyboard bindings for the warning logic */

    if (auto *menuList = mDocument->GetElementById("menu-list")) {
        const bool contentLoaded = content_ready();
        mMenuButtons.push_back(std::make_unique<Button>(menuList, contentLoaded ? "Play" : "Select Game"));
        mMenuButtons.back()->on_pressed([this] {
            if (!content_ready()) {
                /* [MP6] their open_iso_picker() becomes the A4 content
                 * onboarding dialog (disc image via nod, or an extracted
                 * GP6E01 folder -- content_setup.cpp); it was the jump to
                 * the Settings GAME tab before A4, which remains reachable
                 * through Settings for hand-typed paths. */
                mRestartSuppressed = false;
                push(std::make_unique<ContentSetup>());
                return;
            }

            // mDoAud_seStartMenu(kSoundPlay); // TODO PC (their punt, kept)

            request_play(); /* [MP6] their PartyBoard_IsGameLaunched = true */
            hide(true);
        });
        apply_intro_animation(mMenuButtons.back()->root(), "delay-1");

        mMenuButtons.push_back(std::make_unique<Button>(menuList, "Settings"));
        mMenuButtons.back()->on_pressed([this] {
            mRestartSuppressed = false;
            push(std::make_unique<SettingsWindow>(true));
        });
        apply_intro_animation(mMenuButtons.back()->root(), "delay-2");

        mMenuButtons.push_back(std::make_unique<Button>(menuList, "Quit"));
        mMenuButtons.back()->on_pressed([] { request_quit(); }); /* [MP6] their PartyBoard_IsRunning = false */
        apply_intro_animation(mMenuButtons.back()->root(), "delay-3");
    }

    mDiscStatus = mDocument->GetElementById("disc-status");
    mDiscDetail = mDocument->GetElementById("disc-version");
    mVersion = mDocument->GetElementById("version-text");

    if (mVersion != nullptr) {
        /* [MP6] our git-describe equivalent; their leading-'v' strip is
         * moot for a short hash but the shape is theirs. */
        std::string_view versionStr(port_version());
        if (!versionStr.empty() && versionStr[0] == 'v') {
            versionStr = versionStr.substr(1);
        }
        mVersion->SetInnerRML(escape(versionStr));
    }

    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target == nullptr) {
            return;
        }
        if (target == mDocument && !mDocument->HasAttribute("open")) {
            Document::hide(true);
        }
        else if (target->GetTagName() == "button" && !target->IsClassSet("anim-done")) {
            target->SetClass("anim-done", true);
        }
    });
}

void Prelaunch::show()
{
    Document::show();
    mDocument->SetAttribute("open", "");
    mRoot->SetAttribute("open", "");

    /* [MP6] returning from Settings re-validates the content root (their
     * ensure_initialized/refresh path did the disc equivalent). */
    refresh_content_state();
    mShownContentState = -99;

    if (restart_pending() && !mRestartSuppressed) {
        const auto dismiss = [this](Modal &modal) {
            mRestartSuppressed = true;
            modal.pop();
        };
        std::vector<ModalAction> actions;
        if constexpr (kSupportsProcessRestart) {
            actions.push_back(ModalAction {
                .label = "Restart later",
                .onPressed = dismiss,
            });
            actions.push_back(ModalAction {
                .label = "Restart now",
                .onPressed = [](Modal &) { /* unreachable in this port */ },
            });
        }
        else {
            actions.push_back(ModalAction {
                .label = "OK",
                .onPressed = dismiss,
            });
        }
        push(std::make_unique<Modal>(Modal::Props {
            .title = "Apply Options",
            .bodyRml = kSupportsProcessRestart ? "A restart is required to apply selected options.<br/><br/>Restart now to "
                                                 "apply them immediately?"
                                               : "A restart is required to apply selected options.<br/><br/>Close and reopen "
                                                 "the port to apply them.", /* [MP6] body text ours */
            .actions = std::move(actions),
            .onDismiss = dismiss,
        }));
    }
}

void Prelaunch::hide(bool close)
{
    if (close) {
        if (!mEntranceAnimationStarted) {
            // Close document immediately
            Document::hide(true);
        }
        else {
            mPendingClose = true;
        }
        mDocument->RemoveAttribute("open");
    }
    else {
        mRoot->RemoveAttribute("open");
    }
}

void Prelaunch::update()
{
    const bool contentLoaded = content_ready();

    if (!mEntranceAnimationStarted && mDocument != nullptr) {
        mDocument->SetClass("animate-in", true);
        mEntranceAnimationStarted = true;
    }

    if (!mMenuButtons.empty()) {
        /* Button::set_text guards on change internally (their code). */
        mMenuButtons[0]->set_text(contentLoaded ? "Play" : "Select Game");
    }

    /* [MP6] their per-frame disc-status refresh, reduced to our two content
     * states (good/bad -- L3's mapping) and guarded to only touch the DOM
     * when the state actually changes (SetInnerRML re-parses; the menu-idle
     * leakgate wants allocation-flat frames). */
    const int state = contentLoaded ? 1 : 0;
    if (state != mShownContentState && mDiscStatus != nullptr) {
        mShownContentState = state;
        const auto discStatusLabel = mDiscStatus->GetElementById("disc-status-label");
        if (discStatusLabel != nullptr) {
            if (contentLoaded) {
                mDiscStatus->SetAttribute("status", "good");
                discStatusLabel->SetInnerRML("Content ready.");
            }
            else {
                mDiscStatus->SetAttribute("status", "bad");
                discStatusLabel->SetInnerRML("Game content not found.");
            }
        }
        if (mDiscDetail != nullptr) {
            if (contentLoaded) {
                mDiscDetail->SetProperty(Rml::PropertyId::Display, Rml::Style::Display::Block);
                Rml::String innerRML = "GameCube \xE2\x80\xA2 USA"; /* their detail-line shape; our disc is USA */
                mDiscDetail->SetInnerRML(innerRML);
            }
            else {
                mDiscDetail->SetProperty(Rml::PropertyId::Display, Rml::Style::Display::None);
            }
        }
    }

    Document::update();
}

bool Prelaunch::focus()
{
    if (mMenuButtons.empty()) {
        return false;
    }
    return mMenuButtons.front()->focus();
}

bool Prelaunch::visible() const
{
    return mDocument->HasAttribute("open") && mRoot->HasAttribute("open");
}

bool Prelaunch::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    int direction = 0;
    if (cmd == NavCommand::Down) {
        direction = 1;
    }
    else if (cmd == NavCommand::Up) {
        direction = -1;
    }
    else {
        return false;
    }
    auto *target = event.GetTargetElement();
    int focusedButton = -1;
    for (int i = 0; i < mMenuButtons.size(); ++i) {
        if (mMenuButtons[i]->contains(target)) {
            focusedButton = i;
            break;
        }
    }
    const auto n = static_cast<int>(mMenuButtons.size());
    int i = ((focusedButton + direction) % n + n) % n;
    while (i >= 0 && i < mMenuButtons.size()) {
        if (mMenuButtons[i]->focus()) {
            // mDoAud_seStartMenu(kSoundItemFocus); // TODO (their punt, kept)
            event.StopPropagation();
            return true;
        }
        i += direction;
    }
    return false;
}

} // namespace mp6::ui
