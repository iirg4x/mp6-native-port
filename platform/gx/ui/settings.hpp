/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once
#include "window.hpp"

namespace mp6::ui {

/* [MP6] our tab order (partyboard's: Prelaunch/Video/Input/Gameplay/Cheats/
 * Interface). The GAME index is exported so the prelaunch first-button can
 * jump straight to the content-root row. Save States sits between Mods and
 * About but only exists on the IN-GAME instance (inGame below), so About
 * has no stable index constant. */
inline constexpr int kSettingsTabVideo = 0;
inline constexpr int kSettingsTabAudio = 1;
inline constexpr int kSettingsTabGame = 2;
inline constexpr int kSettingsTabMods = 3;

class SettingsWindow : public Window {
public:
    /* [MP6] initialTab; inGame = the persistent mid-game instance (summoned
     * over the running game by F10/F1/pad Back/gear): adds the Save States
     * tab, applies Widescreen flips to the live engine, and HIDES instead
     * of closing on Cancel so the same window can be re-summoned. */
    SettingsWindow(bool prelaunch = false, int initialTab = 0, bool inGame = false);

    void update() override;

    bool focus_content(); /* [MP6] focus the first content row (prelaunch content-root jump) */

protected:
    bool consume_close_request() override; /* [MP6] inGame: hide, don't close */
    bool mPrelaunch;
    bool mInGame;
    unsigned int mSeenSavestateGen = 0; /* [MP6] Save States tab refresh latch */
};

} // namespace mp6::ui
