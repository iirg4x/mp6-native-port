/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
/* MP6 opening storybook. Semantic actions retain the document framework;
 * StorybookView shares the settings renderer. Missing content opens the
 * link/import dialog once, and never prevents rendering the launcher. */
// Credits: TwilitRealm

#pragma once

#include "button.hpp"
#include "document.hpp"

#include <memory>
#include <vector>

namespace mp6::ui {
class StorybookView;

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
    void storybook_pointer(Rml::Event &event, bool click);
    bool mHasShown=false, mWelcomeChecked=false, mCovered=false, mClosing=false;
    bool mWordmark=false, mPlayOnClose=false, mQuitOnClose=false;
    int mSelected=0;
    StorybookView *mStorybook=nullptr;
    bool mRestartSuppressed = false;
    std::vector<std::unique_ptr<Button>> mMenuButtons;
    Rml::Element *mRoot = nullptr;
    int mShownContentState = -99; // Decode branding / update labels only on change.
    bool mStoryMotion = false;
    bool mStoryMotionValid = false;
    bool mStoryStateDirty = true;
};

} // namespace mp6::ui
