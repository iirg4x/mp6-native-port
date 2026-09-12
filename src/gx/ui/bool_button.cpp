/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "bool_button.hpp"

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
    if (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right) {
        const bool newValue = !mGetValue();
        mSetValue(newValue);
        // mDoAud_seStartMenu(newValue ? kSoundItemEnable : kSoundItemDisable); // TODO PC
        return true;
    }
    return false;
}

} // namespace mp6::ui
