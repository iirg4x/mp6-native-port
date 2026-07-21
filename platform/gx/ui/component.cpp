/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "component.hpp"

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

Component::Component(Rml::Element *root)
    : mRoot(root)
{
}

Component::~Component() = default;

void Component::update()
{
    for (const auto &child : mChildren) {
        child->update();
    }
}

bool Component::focus()
{
    if (disabled()) {
        return false;
    }
    // Can we focus self?
    if (mRoot->Focus(true)) {
        mRoot->ScrollIntoView(Rml::ScrollIntoViewOptions {
            Rml::ScrollAlignment::Center,
            Rml::ScrollAlignment::Center,
            Rml::ScrollBehavior::Smooth,
            Rml::ScrollParentage::Closest,
        });
        return true;
    }
    // Otherwise, try to focus a child
    for (const auto &child : mChildren) {
        if (child->focus()) {
            return true;
        }
    }
    return false;
}

void Component::set_selected(bool value)
{
    // Subclasses may override selected() to return a dynamic value, but
    // we're only interested in if the pseudoclass is set or not, so we
    // use Component::selected() directly rather than selected().
    if (Component::selected() == value) {
        return;
    }
    mRoot->SetPseudoClass("selected", value);
}

void Component::set_disabled(bool value)
{
    if (Component::disabled() == value) {
        return;
    }
    if (value) {
        mRoot->SetAttribute("disabled", "");
        mRoot->SetPseudoClass("disabled", true);
        mRoot->Blur();
    }
    else {
        mRoot->RemoveAttribute("disabled");
        mRoot->SetPseudoClass("disabled", false);
    }
}

void Component::listen(Rml::Element *element, Rml::EventId event, ScopedEventListener::Callback callback, bool capture)
{
    if (element == nullptr) {
        element = mRoot;
    }
    mListeners.emplace_back(std::make_unique<ScopedEventListener>(element, event, std::move(callback), capture));
}

bool Component::contains(Rml::Element *element) const
{
    for (const auto *node = element; node != nullptr; node = node->GetParentNode()) {
        if (node == mRoot) {
            return true;
        }
    }
    return false;
}

void Component::clear_children()
{
    mChildren.clear();
    while (mRoot->GetNumChildren() > 0) {
        mRoot->RemoveChild(mRoot->GetFirstChild());
    }
}

} // namespace mp6::ui
