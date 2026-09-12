/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "overlay.hpp"

#include "lib/logging.hpp" /* [MP6] aurora internal, via -I AURORA_ROOT */
/* [MP6] dropped: #include "port/achievements.h" */
/* [MP6] dropped: #include "magic_enum.hpp" */
#include "window.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <dolphin/pad.h>
#include "launcher_state.hpp" /* [MP6] replaces <port/settings.h> */

/* [MP6] EVERY STAT PANEL IS AN OVERLAY, and they all live in THIS document.
 *
 * `stat unit` was the precedent: a corner HUD that replaced the FPS badge
 * below (same screen furniture, same corner, never both at once) and survived
 * the console closing. This increment generalizes that -- each panel is its
 * own latch in console_core.c, and each latched one gets an element here.
 *
 * The console's own document holds no panel at all any more, for the reason
 * the latch existed in the first place: a panel that dies when the console
 * closes cannot be watched while playing. This document is passive (never
 * focused, never a nav target, `pointer-events: none`), so overlays never
 * capture input -- only the bar does.
 *
 * build_stat_records() is the console's own record layout, shared so the
 * reference's colours and right-aligned columns are defined once. */
#include "console.hpp"
#include "mp6_console.h"
#include "mp6_boot.h" /* mp6_tick_count -- the overlays' tick-based refresh limit */
#include "mp6_display.h" /* the badge names the OUTPUT'S refresh cap and the present mode */

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace mp6::ui {
namespace {
    aurora::Module Log { "mp6::ui::overlay" };

    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/overlay.rcss" />
</head>
<body>
    <fps id="fps" />
    <statunit id="statunit" />
    <statstack id="statstack" />
</body>
</rml>
)RML";

    constexpr auto kMenuNotificationDuration = std::chrono::milliseconds(2500);

    constexpr std::array<const char *, 4> kFpsCorners = { "tl", "tr", "bl", "br" };

    /* [MP6] How often a stat overlay is rebuilt, in GAME TICKS.
     *
     * Ticks, not wall time, on purpose: mp6_tick_count only advances on a REAL
     * tick, so an interpolated present -- which also calls
     * mp6_launcher_frame_overlay() and therefore also reaches update() -- can
     * never trigger a rebuild. That is the mitigation for the one way these
     * overlays could hurt the thing they measure: anything expensive done
     * inside the replay path lands in the interval fi_cost_observe() measures,
     * and fi_budget_observe() latches any larger cost immediately while giving
     * ground only a quarter at a time, so one heavy replay frame throttles
     * replays for several windows afterwards.
     *
     * 15 ticks is 4 Hz at 60 Hz -- twice the FPS badge's own refresh rate,
     * which is the precedent for "fast enough to read, slow enough not to
     * matter". Each panel keeps its OWN last-rebuild tick, so raising a second
     * overlay staggers against the first instead of doubling one tick's cost. */
    constexpr long kPanelRefreshTicks = 15;

    /* [MP6] Which corner-shaped panel is the HUD. It is the one latch whose
     * element is NOT a stacked table: `stat unit` is the reference's compact
     * corner block and it absorbs the FPS badge, so it keeps the badge's
     * corner setting and its own element. */
    constexpr int kHudPanel = MP6_CONSOLE_PANEL_UNIT;

    /* [MP6] replaces magic_enum::enum_name in the nav-command warn log. */
    const char *nav_command_name(NavCommand cmd)
    {
        switch (cmd) {
            case NavCommand::Up: return "Up";
            case NavCommand::Down: return "Down";
            case NavCommand::Left: return "Left";
            case NavCommand::Right: return "Right";
            case NavCommand::Next: return "Next";
            case NavCommand::Previous: return "Previous";
            case NavCommand::Confirm: return "Confirm";
            case NavCommand::Cancel: return "Cancel";
            case NavCommand::Menu: return "Menu";
            default: return "None";
        }
    }

    /* [MP6] rewrites the `"res/` href to our resolved res/ base. */
    Rml::String formatted_source(const Rml::String &raw)
    {
        char buf[4096];
        mp6::ui::format_document_source(raw.c_str(), buf, sizeof(buf));
        return Rml::String(buf);
    }

    Rml::Element *create_toast(Rml::Element *parent, const Toast &toast)
    {
        auto *elem = append(parent, "toast");
        if (!toast.type.empty()) {
            elem->SetClass(toast.type, true);
        }
        {
            auto *heading = append(elem, "heading");
            if (toast.title.starts_with("<")) {
                heading->SetInnerRML(toast.title);
            }
            else {
                auto *span = append(heading, "span");
                span->SetInnerRML(toast.title);
            }
            if (toast.type == "achievement") {
                auto *icon = append(heading, "icon");
                icon->SetClass("trophy", true);
                // mDoAud_seStartMenu(kSoundAchievementUnlock); // TODO PC
            }
            else if (toast.type == "controller") {
                auto *icon = append(heading, "icon");
                icon->SetClass("controller", true);
            }
        }
        {
            auto *message = append(elem, "message");
            if (toast.content.starts_with("<")) {
                message->SetInnerRML(toast.content);
            }
            else {
                auto *span = append(message, "span");
                span->SetInnerRML(toast.content);
            }
        }
        {
            auto *progress = append(elem, "progress");
            progress->SetAttribute("value", 1.f);
        }
        return elem;
    }

    Rml::Element *create_controller_warning(Rml::Element *parent)
    {
        auto *elem = append(parent, "toast");
        elem->SetClass("controller-warning", true);

        auto *heading = append(elem, "heading");
        auto *title = append(heading, "span");
        title->SetInnerRML("No controller assigned");
        auto *icon = append(heading, "icon");
        icon->SetClass("warning", true);

        auto *message = append(elem, "message");
        auto *content = append(message, "span");
        content->SetInnerRML("Configure controller port 1 in Settings.");

        return elem;
    }

    SDL_Gamepad *gamepad_for_port(u32 port) noexcept
    {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            return nullptr;
        }
        return PADGetSDLGamepadForIndex(static_cast<u32>(index));
    }

    Rml::String back_button_name()
    {
        if (auto *gamepad = gamepad_for_port(PAD_CHAN0)) {
            switch (SDL_GetGamepadType(gamepad)) {
                case SDL_GAMEPAD_TYPE_PS3:
                    return "Select";
                case SDL_GAMEPAD_TYPE_PS4:
                    return "Share";
                case SDL_GAMEPAD_TYPE_PS5:
                    return "Create";
                case SDL_GAMEPAD_TYPE_XBOX360:
                    return "Back";
                case SDL_GAMEPAD_TYPE_XBOXONE:
                    return "View";
                case SDL_GAMEPAD_TYPE_GAMECUBE:
                    return "R + Start";
                default:
                    break;
            }
        }
        return "Back";
    }

/* [MP6] desktop: F10 is the documented in-game menu hotkey (F1 and the
 * R+Start chord also work -- the ripped partyboard bindings); Android: the
 * gear button on the touch overlay or a 3-finger tap. */
#if defined(__ANDROID__) || defined(TARGET_ANDROID) || (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
    constexpr auto kMenuNotificationPrefix = "Gear button, 3-finger tap or";
#else
    constexpr auto kMenuNotificationPrefix = "Press F10 or";
#endif

    Rml::Element *create_menu_notification(Rml::Element *parent)
    {
        auto *elem = append(parent, "toast");
        elem->SetClass("menu-notification", true);

        auto *message = append(elem, "message");
        auto *row = append(message, "row");
        append(row, "span")->SetInnerRML(kMenuNotificationPrefix);
        auto *icon = append(row, "icon");
        icon->SetClass("controller", true);
        append(row, "span")->SetInnerRML(escape(back_button_name()));
        append(row, "span")->SetInnerRML("to open menu");

        return elem;
    }

    void remove_element(Rml::Element *&elem) noexcept
    {
        if (elem == nullptr) {
            return;
        }
        if (auto *parent = elem->GetParentNode()) {
            parent->RemoveChild(elem);
        }
        elem = nullptr;
    }

} // namespace

// https://vplesko.com/posts/how_to_implement_an_fps_counter.html
void Overlay::advance_fps_counter(float &outFps, Uint64 perfFreq)
{
    if (perfFreq == 0) {
        outFps = 0.f;
        return;
    }

    const Uint64 curr = SDL_GetPerformanceCounter();
    if (!mFpsHavePrevCounter) {
        mFpsPrevCounter = curr;
        mFpsHavePrevCounter = true;
        outFps = 0.f;
        return;
    }

    const Uint64 processingTicks = curr - mFpsPrevCounter;
    mFpsPrevCounter = curr;

    mFpsFrameEvents.push_back({ curr, processingTicks });
    mFpsSumTicks += processingTicks;

    while (!mFpsFrameEvents.empty() && mFpsFrameEvents.front().endCounter + perfFreq < curr) {
        mFpsSumTicks -= mFpsFrameEvents.front().processingTicks;
        mFpsFrameEvents.pop_front();
    }

    const auto n = mFpsFrameEvents.size();
    if (n == 0 || mFpsSumTicks == 0) {
        outFps = 0.f;
        return;
    }

    const double avgSeconds = static_cast<double>(mFpsSumTicks) / static_cast<double>(n) / static_cast<double>(perfFreq);
    outFps = static_cast<float>(1.0 / avgSeconds);
}

Overlay::Overlay()
    : Document(formatted_source(kDocumentSource))
{
    mFpsCounter = mDocument->GetElementById("fps");
    /* [MP6] the stat overlays: `stat unit` keeps its own corner element, every
     * other panel is created on demand inside the tiling stack. */
    mPanel.fill(nullptr);
    mPanelTick.fill(-1);
    mPanel[kHudPanel] = mDocument->GetElementById("statunit");
    mPanelStack = mDocument->GetElementById("statstack");

    listen(mDocument, Rml::EventId::Focus, [](Rml::Event &) { Log.warn("Overlay received focus"); });
    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        if (event.GetTargetElement() == mCurrentToast) {
            if (get_toasts().empty() || clock::now() >= mCurrentToastStartTime + get_toasts().front().duration) {
                mCurrentToast->SetPseudoClass("done", true);
            }
        }
        else if (mControllerWarning != nullptr && event.GetTargetElement() == mControllerWarning && !mControllerWarning->HasAttribute("open")) {
            mControllerWarning->SetPseudoClass("done", true);
        }
        else if (mMenuNotification != nullptr && event.GetTargetElement() == mMenuNotification && !mMenuNotification->HasAttribute("open")) {
            mMenuNotification->SetPseudoClass("done", true);
        }
    });
}

void Overlay::show()
{
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

/* [MP6] The stat overlays. One pass over the latch array per frame; the DOM is
 * touched only when a latch CHANGES or a panel's own refresh tick comes due.
 *
 * `stat unit` is the corner HUD -- its element pre-exists in the document
 * source and it takes the FPS badge's corner. Every other panel is a table
 * created inside <statstack> when its latch goes up and REMOVED when it goes
 * down: an overlay nobody asked for must cost one predicate and no elements,
 * because this document runs on every frame of every launcher session whether
 * or not anyone ever opened the console.
 *
 * Panels are appended in enum order, so the stack's reading order does not
 * shuffle when a latch in the middle is dropped and re-raised... which it
 * would if new panels were always appended last. That is what the insert-
 * before search below buys. */
void Overlay::refresh_stat_overlays()
{
    /* Tell the stack which corner the HUD is occupying so it can yield it
     * (res/rml/overlay.rcss statstack[hud=...]). RCSS cannot ask whether the
     * HUD is up; this view already knows, so one attribute is the whole
     * mechanism -- and without it a `stat unit` in the default top-left corner
     * would sit on top of the first stacked panel. */
    if (mPanelStack != nullptr) {
        if (mp6_console_overlay(kHudPanel) != 0) {
            mPanelStack->SetAttribute("hud", kFpsCorners[cfg().fpsCorner]);
        }
        else if (mPanelStack->HasAttribute("hud")) {
            mPanelStack->RemoveAttribute("hud");
        }
    }

    for (int i = MP6_CONSOLE_PANEL_NONE + 1; i < MP6_CONSOLE_PANEL_COUNT; ++i) {
        const bool on = mp6_console_overlay(i) != 0;
        Rml::Element *elem = mPanel[i];

        if (!on) {
            if (elem == nullptr) {
                continue;
            }
            if (i == kHudPanel) {
                if (elem->HasAttribute("open")) {
                    elem->RemoveAttribute("open");
                    elem->SetInnerRML("");
                }
            }
            else if (auto *parent = elem->GetParentNode()) {
                parent->RemoveChild(elem);
                mPanel[i] = nullptr;
            }
            mPanelTick[i] = -1;
            continue;
        }

        if (elem == nullptr) {
            if (mPanelStack == nullptr) {
                continue;
            }
            auto owned = mDocument->CreateElement("statpanel");
            elem = owned.get();
            if (elem == nullptr) {
                continue;
            }
            elem->SetAttribute("panel", mp6_console_panel_name(i));
            /* Keep enum order in the stack: insert before the first later
             * panel that is already up. */
            Rml::Element *before = nullptr;
            for (int j = i + 1; j < MP6_CONSOLE_PANEL_COUNT && before == nullptr; ++j) {
                if (j != kHudPanel && mPanel[j] != nullptr) {
                    before = mPanel[j];
                }
            }
            if (before != nullptr) {
                mPanelStack->InsertBefore(std::move(owned), before);
            }
            else {
                mPanelStack->AppendChild(std::move(owned));
            }
            mPanel[i] = elem;
        }
        if (i == kHudPanel) {
            elem->SetAttribute("open", "");
            elem->SetAttribute("corner", kFpsCorners[cfg().fpsCorner]);
        }
        if (mPanelTick[i] < 0 || mp6_tick_count - mPanelTick[i] >= kPanelRefreshTicks) {
            mPanelTick[i] = mp6_tick_count;
            build_stat_records(elem, mp6_console_panel_text(i));
        }
    }
}

void Overlay::update()
{
    Document::update();
    if (mDocument == nullptr) {
        return;
    }

    /* [MP6] every latched stat panel, drawn over the live game. */
    refresh_stat_overlays();
    const bool hudOn = mp6_console_overlay(kHudPanel) != 0;

    if (mFpsCounter != nullptr) {
        if (cfg().showFps && !hudOn) { /* [MP6] our flat config in place of their ConfigVar registry */
            const int idx = cfg().fpsCorner;
            mFpsCounter->SetAttribute("open", "");
            const char *corner = kFpsCorners[idx];
#if defined(__ANDROID__)
            /* Android touch build: the in-game gear button (src/android/
             * touch_pad.cpp) is pinned to the top-left corner and spans ~13% of
             * the window height from the top. A top-left FPS counter overlaps
             * it, so shift that one corner clear of the gear (overlay.rcss
             * fps[corner=tl-gear]); the gear does not exist off Android, so
             * every other build keeps the plain top-left placement unchanged. */
            if (idx == 0) {
                corner = "tl-gear";
            }
#endif
            mFpsCounter->SetAttribute("corner", corner);

            const Uint64 perfFreq = SDL_GetPerformanceFrequency();
            float fps = 0.f;
            advance_fps_counter(fps, perfFreq);

            const Uint64 now = SDL_GetPerformanceCounter();
            // Limit updates to twice per second
            const bool refreshLabel
                = perfFreq == 0 || mFpsLastUpdate == 0 || static_cast<double>(now - mFpsLastUpdate) >= 0.5 * static_cast<double>(perfFreq);
            if (refreshLabel) {
                mFpsLastUpdate = now;
                /* A bare number is unreadable, and that is not a cosmetic
                 * complaint: this badge counts PRESENTS, and with present sync
                 * on the present cadence is capped by the refresh rate of
                 * whichever OUTPUT the window is on -- not by the tick rate and
                 * not by what the engine could manage. A user reading "70" has
                 * no way to tell a 70 Hz cap from a 70 fps struggle. Naming the
                 * cap and the present mode beside the measurement makes the
                 * number explain itself: "75 / 75 Hz FIFO" says "this IS the
                 * ceiling", where "75 / 240 Hz FIFO" would not.
                 *
                 * The query hangs off THIS twice-per-second gate, so it costs
                 * two SDL calls per 0.5 s rather than per present, and the
                 * display walk behind isHighestRefresh is itself memoized.
                 *
                 * The fallback keeps today's exact string: if the display query
                 * ever fails, the badge degrades to what it always showed
                 * rather than to a blank chip or an invented refresh rate. */
                Mp6DisplayInfo di;
                if (mp6_display_info_get(&di) && di.refreshHz > 0) {
                    mFpsCounter->SetInnerRML(escape(
                        fmt::format("{:.0f} / {} Hz {}", fps, di.refreshHz, di.presentMode)));
                } else {
                    mFpsCounter->SetInnerRML(escape(fmt::format("{:.0f} FPS", fps)));
                }
            }
        }
        else {
            mFpsCounter->RemoveAttribute("open");
            mFpsFrameEvents.clear();
            mFpsSumTicks = 0;
            mFpsHavePrevCounter = false;
            mFpsLastUpdate = 0;
        }
    }

    u32 buttonCount;
    const bool showControllerWarning = PADGetIndexForPort(PAD_CHAN0) < 0 && PADGetKeyButtonBindings(PAD_CHAN0, &buttonCount) == nullptr
        && dynamic_cast<Window *>(top_document()) == nullptr && dynamic_cast<WindowSmall *>(top_document()) == nullptr;
    if (showControllerWarning && mControllerWarning == nullptr) {
        mControllerWarning = create_controller_warning(mDocument);
    }
    else if (showControllerWarning && mControllerWarning != nullptr) {
        mControllerWarning->SetAttribute("open", "");
        mControllerWarning->SetPseudoClass("opened", true);
        mControllerWarning->SetPseudoClass("done", false);
    }
    else if (!showControllerWarning && mControllerWarning != nullptr) {
        if (mControllerWarning->IsPseudoClassSet("done") || !mControllerWarning->IsPseudoClassSet("opened")) {
            remove_element(mControllerWarning);
        }
        else {
            mControllerWarning->RemoveAttribute("open");
        }
    }

    if (mMenuNotification != nullptr) {
        if (clock::now() >= mMenuNotificationStartTime + kMenuNotificationDuration) {
            if (mMenuNotification->IsPseudoClassSet("done") || !mMenuNotification->IsPseudoClassSet("opened")) {
                remove_element(mMenuNotification);
            }
            else {
                mMenuNotification->RemoveAttribute("open");
            }
        }
        else {
            mMenuNotification->SetAttribute("open", "");
            mMenuNotification->SetPseudoClass("opened", true);
            mMenuNotification->SetPseudoClass("done", false);
        }
    }
    if (consume_menu_notification_request()) {
        if (mMenuNotification == nullptr) {
            mMenuNotification = create_menu_notification(mDocument);
        }
        mMenuNotificationStartTime = clock::now();
    }

    auto &toasts = get_toasts();
    if (mCurrentToast == nullptr) {
        if (!toasts.empty()) {
            const auto &toast = toasts.front();
            mCurrentToast = create_toast(mDocument, toast);
            mCurrentToastStartTime = clock::now();
        }
    }
    else if (!toasts.empty()) {
        const auto &toast = toasts.front();
        const float duration = std::chrono::duration<float>(toast.duration).count();
        const float elapsed = std::chrono::duration<float>(clock::now() - mCurrentToastStartTime).count();
        const float ratio = duration > 0.0f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
        const auto remaining = 1.f - ratio;
        Rml::ElementList list;
        mDocument->GetElementsByTagName(list, "progress");
        for (auto *elem : list) {
            elem->SetAttribute("value", remaining);
        }
        if (remaining == 0.f) {
            if (mCurrentToast->IsPseudoClassSet("done") ||
                // Fallback for large gaps in time where we never actually opened it
                !mCurrentToast->IsPseudoClassSet("opened")) {
                remove_element(mCurrentToast);
                toasts.pop_front();
            }
            else {
                mCurrentToast->RemoveAttribute("open");
            }
        }
        else {
            mCurrentToast->SetAttribute("open", "");
            mCurrentToast->SetPseudoClass("opened", true);
        }
    }
}

bool Overlay::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    Log.warn("Overlay received nav command: {}", nav_command_name(cmd)); /* [MP6] no magic_enum */
    return false;
}

} // namespace mp6::ui
