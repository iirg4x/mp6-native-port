/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
/* [MP6] adaptation summary: their disc-image concept (iso validation state,
 * picker, verification modals, update checker) is replaced by our
 * content-root concept -- PrelaunchState and its helpers are gone
 * (launcher_state.hpp carries our equivalents); the Prelaunch document class
 * itself keeps their shape. */
// Credits: TwilitRealm

#pragma once

#include "button.hpp"
#include "document.hpp"

#include <memory>
#include <vector>

namespace mp6::ui {

class Prelaunch : public Document {
public:
    Prelaunch();

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

private:
    bool mEntranceAnimationStarted = false;
    bool mRestartSuppressed = false;
    std::vector<std::unique_ptr<Button>> mMenuButtons;
    Rml::Element *mRoot = nullptr;
    Rml::Element *mDiscStatus = nullptr;  /* [MP6] the CONTENT status block */
    Rml::Element *mDiscDetail = nullptr;
    Rml::Element *mVersion = nullptr;
    int mShownContentState = -99; /* [MP6] SetInnerRML-on-change guard */
};

} // namespace mp6::ui
