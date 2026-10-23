/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "bool_button.hpp"
#include "../../audio/menu_sfx.hpp"

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

BoolButton::BoolButton(Rml::Element *parent, Props props)
    : BaseControlledSelectButton(parent,
          {
              .key = std::move(props.key),
              .icon = std::move(props.icon),
          })
    , mGetValue(std::move(props.getValue))
    , mSetValue(std::move(props.setValue))
    , mIsDisabled(std::move(props.isDisabled))
    , mIsModified(std::move(props.isModified))
{
    mRoot->SetClass("toggle-row", true);
    mSwitch = append(mRoot, "toggle-switch");
    mSwitch->SetInnerRML("<toggle-knob/>");
}

void BoolButton::update()
{
    BaseControlledSelectButton::update();
    mSwitch->SetPseudoClass("checked", mGetValue() != 0);
}

bool BoolButton::modified() const
{
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool BoolButton::disabled() const
{
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

Rml::String BoolButton::format_value()
{
    return mGetValue() ? "On" : "Off";
}

bool BoolButton::handle_nav_command(NavCommand cmd)
{
    // In the deck, horizontal input browses cards; A/tap changes a toggle.
    if ((cmd == NavCommand::Left || cmd == NavCommand::Right) && mRoot->Closest(".card-deck")) return false;
    if (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right) {
        const bool newValue = cmd == NavCommand::Left ? false : cmd == NavCommand::Right ? true : !mGetValue();
        if (newValue != static_cast<bool>(mGetValue())) {mSetValue(newValue);audio::play_menu_sound(audio::MenuSound::Confirm);}
        return true;
    }
    return false;
}

} // namespace mp6::ui
