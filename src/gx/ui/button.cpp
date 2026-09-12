/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "button.hpp"

#include "ui.hpp"

#include <utility>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {

    Rml::Element *createRoot(Rml::Element *parent, const Rml::String &tagName)
    {
        auto *doc = parent->GetOwnerDocument();
        auto elem = doc->CreateElement(tagName);
        return parent->AppendChild(std::move(elem));
    }

} // namespace

Button::Button(Rml::Element *parent, Props props, const Rml::String &tagName)
    : FluentComponent(createRoot(parent, tagName))
{
    update_props(std::move(props));
    /* [MP6] Hover moves focus (non-visible), so the hovered item is the ONLY
     * highlighted one. Upstream keeps keyboard focus-visible on the first
     * menu button while :hover lights a second element -- two bars at once
     * (their component.cpp Focus(true) + prelaunch.rcss :hover,:focus-visible
     * sharing the decorator). Syncing focus to hover keeps exactly one bar
     * and keyboard nav continues from the hovered item. */
    listen(Rml::EventId::Mouseover, [this](Rml::Event &) {
        if (!disabled() && !mRoot->IsPseudoClassSet("focus")) {
            mRoot->Focus();
        }
    });
}

void Button::set_text(const Rml::String &text)
{
    if (mProps.text != text) {
        mRoot->SetInnerRML(escape(text));
        mProps.text = text;
    }
}

Button &Button::on_pressed(ButtonCallback callback)
{
    if (!callback) {
        return *this;
    }
    // TODO: convert this to a FluentComponent method?
    on_nav_command([callback = std::move(callback)](Rml::Event &, NavCommand cmd) {
        if (cmd == NavCommand::Confirm) {
            callback();
            return true;
        }
        return false;
    });
    return *this;
}

void Button::update_props(Props props)
{
    set_text(props.text);
    mProps = std::move(props);
}

void ControlledButton::update()
{
    if (mIsSelected) {
        set_selected(mIsSelected());
    }
    Button::update();
}

bool ControlledButton::selected() const
{
    if (mIsSelected) {
        return mIsSelected();
    }
    return Button::selected();
}

} // namespace mp6::ui
