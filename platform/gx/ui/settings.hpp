/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once
#include "window.hpp"

namespace mp6::ui {

/* [MP6] our tab order (partyboard's: Prelaunch/Video/Input/Gameplay/Cheats/
 * Interface). The GAME index is exported so the prelaunch first-button can
 * jump straight to the content-root row. */
inline constexpr int kSettingsTabVideo = 0;
inline constexpr int kSettingsTabAudio = 1;
inline constexpr int kSettingsTabGame = 2;
inline constexpr int kSettingsTabAbout = 3;

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false, int initialTab = 0); /* [MP6] initialTab */

    void update() override;

    bool focus_content(); /* [MP6] focus the first content row (prelaunch content-root jump) */

protected:
    bool mPrelaunch;
};

} // namespace mp6::ui
