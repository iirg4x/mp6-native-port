/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "select_button.hpp"

#include "ui.hpp"

#include <fmt/format.h>
#include <utility>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {

    Rml::Element *createRoot(Rml::Element *parent)
    {
        auto *doc = parent->GetOwnerDocument();
        auto elem = doc->CreateElement("select-button");
        return parent->AppendChild(std::move(elem));
    }

} // namespace

SelectButton::SelectButton(Rml::Element *parent, Props props)
    : FluentComponent(createRoot(parent))
{
    if (parent->Closest("window.party-settings")) {
        auto *star = append(mRoot, "focus-star");
        star->SetInnerRML("<story-shape kind=\"star\"/>");
    }
    mKeyElem = append(mRoot, "key");
    mIconElem = append(mRoot, "icon");
    if (parent->Closest(".storybook")) {
        auto *left = append(mRoot, "choice-step");
        left->SetAttribute("data-step", -1);
        left->SetInnerRML("<span>&#x2039;</span>");
        left->SetAttribute("aria-label", "Previous value");
    }
    mValueElem = append(mRoot, "value");
    if (parent->Closest(".storybook")) {
        auto *right = append(mRoot, "choice-step");
        right->SetAttribute("data-step", 1);
        right->SetInnerRML("<span>&#x203a;</span>");
        right->SetAttribute("aria-label", "Next value");
    }
    if (parent->Closest("window.party-settings") && props.submit) {
        auto *arrow = append(mRoot, "icon");
        arrow->SetClass("choice-arrow", true);
        arrow->SetInnerRML("&#xe5cc;");
    }
    update_props(std::move(props));
    on_nav_command([this](Rml::Event &event, NavCommand cmd) {
        if (cmd == NavCommand::Confirm && event.GetId() == Rml::EventId::Click) {
            // RmlUi's Closest searches ancestors only. Native mesh hit-testing
            // dispatches on the arrow itself, rather than its printed span.
            auto *target = event.GetTargetElement();
            if (auto *step = target->GetTagName()=="choice-step" ? target : target->Closest("choice-step"))
                cmd = step->GetAttribute<int>("data-step", 1) < 0 ? NavCommand::Left : NavCommand::Right;
        }
        return handle_nav_command(cmd);
    });
    listen(Rml::EventId::Mouseover, [this](Rml::Event &) {
        if (!disabled() && !mRoot->IsPseudoClassSet("focus")) mRoot->Focus();
    });
}

bool SelectButton::modified() const
{
    return mProps.modified;
}

void SelectButton::set_modified(bool value)
{
    if (mProps.modified != value) {
        mValueElem->SetClass("modified", value);
        if (value && !mRoot->Closest("window.party-settings")) {
            mValueElem->SetInnerRML(fmt::format("•&nbsp;{}", escape(mProps.value)));
        }
        else {
            mValueElem->SetInnerRML(escape(mProps.value));
        }
        mProps.modified = value;
    }
}

void SelectButton::set_value_label(const Rml::String &value)
{
    if (mProps.value != value) {
        if (mProps.modified && !mRoot->Closest("window.party-settings")) {
            mValueElem->SetInnerRML(fmt::format("•&nbsp;{}", escape(value)));
        }
        else {
            mValueElem->SetInnerRML(escape(value));
        }
        mProps.value = value;
    }
}

SelectButton &SelectButton::on_pressed(SelectButtonCallback callback)
{
    if (!callback) {
        return *this;
    }
    listen(Rml::EventId::Submit, [this, callback = std::move(callback)](Rml::Event &event) {
        if (!disabled() && event.GetTargetElement() == mRoot) {
            callback();
            event.StopPropagation();
        }
    });
    return *this;
}

void SelectButton::update_props(Props props)
{
    if (mProps.key != props.key) {
        mKeyElem->SetInnerRML(escape(props.key));
    }
    if (mProps.icon != props.icon) {
        Rml::StringList iconClasses;
        Rml::StringUtilities::ExpandString(iconClasses, mIconElem->GetClassNames(), ' ', true);
        for (const auto &className : iconClasses) {
            mIconElem->SetClass(className, false);
        }
        if (!props.icon.empty()) {
            mIconElem->SetClass(props.icon, true);
        }
    }
    set_value_label(props.value);
    set_modified(props.modified);
    mProps = std::move(props);
}

bool SelectButton::handle_nav_command(NavCommand cmd)
{
    // The storybook keeps choices inline. A/Enter advances the same value as
    // the right arrow; never summon the legacy detail pane over the preview.
    if (cmd == NavCommand::Confirm && mRoot->Closest(".native-storybook")) {
        if (mStep && can_step(1)) mStep(1);
        return true;
    }
    if (mStep && (cmd == NavCommand::Left || cmd == NavCommand::Right)) {
        const int direction=cmd == NavCommand::Right ? 1 : -1;
        if (can_step(direction)) mStep(direction);
        return true;
    }
    if (cmd == NavCommand::Confirm && mProps.submit) {
        mRoot->DispatchEvent(Rml::EventId::Submit, {});
        return true;
    }
    return false;
}

void SelectButton::update()
{
    mRoot->SetAttribute("data-can-left",can_step(-1));
    mRoot->SetAttribute("data-can-right",can_step(1));
    for(int i=0;i<mRoot->GetNumChildren();++i) {
        auto *child=mRoot->GetChild(i);
        if(child->GetTagName()!="choice-step")continue;
        const bool available=can_step(child->GetAttribute<int>("data-step",1));
        child->SetPseudoClass("disabled",!available);
        child->SetAttribute("aria-disabled",available?"false":"true");
    }
    FluentComponent::update();
}

void BaseControlledSelectButton::update()
{
    set_disabled(disabled());
    set_value_label(format_value());
    set_modified(modified());
    SelectButton::update();
}

bool ControlledSelectButton::modified() const
{
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool ControlledSelectButton::disabled() const
{
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

Rml::String ControlledSelectButton::format_value()
{
    if (!mGetValue) {
        return "";
    }
    return mGetValue();
}

} // namespace mp6::ui
