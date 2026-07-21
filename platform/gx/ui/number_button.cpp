/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "number_button.hpp"

#include <charconv>
#include <fmt/format.h>

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

NumberButton::NumberButton(Rml::Element *parent, Props props)
    : BaseStringButton(parent, { .key = std::move(props.key), .type = "number" })
    , mGetValue(std::move(props.getValue))
    , mSetValue(std::move(props.setValue))
    , mIsDisabled(std::move(props.isDisabled))
    , mIsModified(std::move(props.isModified))
    , mMin(props.min)
    , mMax(props.max)
    , mStep(props.step)
    , mPrefix(std::move(props.prefix))
    , mSuffix(std::move(props.suffix))
{
}

bool NumberButton::modified() const
{
    if (mIsModified) {
        return mIsModified();
    }
    return BaseStringButton::modified();
}

bool NumberButton::disabled() const
{
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseStringButton::disabled();
}

Rml::String NumberButton::format_value()
{
    return fmt::format("{}{}{}", mPrefix, mGetValue(), mSuffix);
}

Rml::String NumberButton::input_value()
{
    return fmt::to_string(mGetValue());
}

void NumberButton::set_value(Rml::String value)
{
    if (!mSetValue) {
        return;
    }

    int parsedValue = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsedValue);
    if (result.ec != std::errc() || result.ptr != end) {
        return;
    }

    mSetValue(std::clamp(parsedValue, mMin, mMax));
}

bool NumberButton::handle_nav_command(NavCommand cmd)
{
    if (!is_editing() && (cmd == NavCommand::Left || cmd == NavCommand::Right)) {
        const int newValue = std::clamp(mGetValue() + (cmd == NavCommand::Right ? mStep : -mStep), mMin, mMax);
        if (newValue != mGetValue()) {
            mSetValue(newValue);
            // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
        }
        return true;
    }
    return BaseStringButton::handle_nav_command(cmd);
}

} // namespace mp6::ui
