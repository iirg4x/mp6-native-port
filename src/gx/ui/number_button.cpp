/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "number_button.hpp"
#include "../../audio/menu_sfx.hpp"

#include <charconv>
#include <fmt/format.h>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
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

SliderButton::SliderButton(Rml::Element *parent, Props props)
    : BaseControlledSelectButton(parent, {.key = props.key, .submit = false})
    , mSliderProps(std::move(props))
{
    mRoot->SetClass("slider-row", true);
    listen(Rml::EventId::Blur, [this](Rml::Event &) { mRoot->SetClass("card-editing", false); });
    mRange = append(mRoot, "input");
    mRange->SetAttribute("type", "range");
    mRange->SetAttribute("min", mSliderProps.min);
    mRange->SetAttribute("max", mSliderProps.max);
    mRange->SetAttribute("step", mSliderProps.step);
    mRange->SetAttribute("aria-label", mSliderProps.key);
    Component::listen(mRange, Rml::EventId::Change, [this](Rml::Event &event) {
        if (!disabled()) set_value(event.GetParameter<int>("value", mSliderProps.getValue()));
        event.StopPropagation();
    });
}

void SliderButton::set_value(int value)
{
    const int step = std::max(1, mSliderProps.step);
    value = mSliderProps.min + ((std::clamp(value, mSliderProps.min, mSliderProps.max) -
        mSliderProps.min + step / 2) / step) * step;
    value = std::clamp(value, mSliderProps.min, mSliderProps.max);
    if (value != mSliderProps.getValue()) {mSliderProps.setValue(value);audio::play_menu_sound(audio::MenuSound::Move);}
}

void SliderButton::update()
{
    BaseControlledSelectButton::update();
    const int value = mSliderProps.getValue();
    if (mLastValue != value) {
        mLastValue = value;
        mRange->SetAttribute("value", value);
    }
    if (disabled()) mRange->SetAttribute("disabled", "");
    else mRange->RemoveAttribute("disabled");
}

bool SliderButton::disabled() const { return mSliderProps.isDisabled && mSliderProps.isDisabled(); }
bool SliderButton::modified() const { return mSliderProps.isModified && mSliderProps.isModified(); }
Rml::String SliderButton::format_value()
{
    return fmt::format("{}{}{}", mSliderProps.prefix, mSliderProps.getValue(), mSliderProps.suffix);
}
bool SliderButton::handle_nav_command(NavCommand cmd)
{
    if (mRoot->Closest(".card-deck")) {
        if (cmd == NavCommand::Confirm) {
            mRoot->SetClass("card-editing", !mRoot->IsClassSet("card-editing"));
            return true;
        }
        if (cmd == NavCommand::Cancel && mRoot->IsClassSet("card-editing")) {
            mRoot->SetClass("card-editing", false);
            return true;
        }
        if (!mRoot->IsClassSet("card-editing")) return false;
    }
    if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
        set_value(mSliderProps.getValue() + (cmd == NavCommand::Right ? mSliderProps.step : -mSliderProps.step));
        return true;
    }
    return cmd == NavCommand::Confirm;
}

} // namespace mp6::ui
