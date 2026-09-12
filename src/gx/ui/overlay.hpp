/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once

#include "document.hpp"

#include <array>
#include <chrono>
#include <deque>

#include "mp6_console.h" /* [MP6] MP6_CONSOLE_PANEL_COUNT -- the latch matrix below */

namespace mp6::ui {

class Overlay : public Document {
public:
    Overlay();

    void show() override;
    void update() override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

    /* [MP6] The stat overlays. One element per latched panel, created when the
     * latch goes up and destroyed when it goes down, so a panel nobody asked
     * for costs one predicate per frame and no DOM at all. They live in THIS
     * document because it is passive (never focused, never a nav target) and
     * alive on every frame -- which is exactly what "survives the console
     * closing and never captures input" requires. */
    void refresh_stat_overlays();

    Rml::Element *mFpsCounter = nullptr;
    std::array<Rml::Element *, MP6_CONSOLE_PANEL_COUNT> mPanel {};
    std::array<long, MP6_CONSOLE_PANEL_COUNT> mPanelTick {};
    Rml::Element *mPanelStack = nullptr; /* [MP6] the tiling container for them */
    Rml::Element *mCurrentToast = nullptr;
    Rml::Element *mControllerWarning = nullptr;
    Rml::Element *mMenuNotification = nullptr;
    clock::time_point mCurrentToastStartTime;
    clock::time_point mMenuNotificationStartTime;

    struct FpsFrameEvent {
        Uint64 endCounter;
        Uint64 processingTicks;
    };

    std::deque<FpsFrameEvent> mFpsFrameEvents;
    Uint64 mFpsSumTicks = 0;
    bool mFpsHavePrevCounter = false;
    Uint64 mFpsPrevCounter = 0;
    Uint64 mFpsLastUpdate = 0;

    void advance_fps_counter(float &outFps, Uint64 perfFreq);
};

} // namespace mp6::ui
