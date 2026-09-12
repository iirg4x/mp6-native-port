/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "event.hpp"

#include <utility>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

ScopedEventListener::ScopedEventListener(Rml::Element *element, Rml::EventId event, Callback callback, bool capture)
    : mElement(element)
    , mEvent(event)
    , mCapture(capture)
    , mCallback(std::move(callback))
{
    mElement->AddEventListener(mEvent, this, mCapture);
}

ScopedEventListener::ScopedEventListener(Rml::Element *element, Rml::String event, Callback callback, bool capture)
    : mElement(element)
    , mEventName(std::move(event))
    , mCapture(capture)
    , mCallback(std::move(callback))
{
    mElement->AddEventListener(mEventName, this, mCapture);
}

ScopedEventListener::~ScopedEventListener()
{
    if (mElement != nullptr) {
        if (!mEventName.empty()) {
            mElement->RemoveEventListener(mEventName, this, mCapture);
        }
        else {
            mElement->RemoveEventListener(mEvent, this, mCapture);
        }
        mElement = nullptr;
    }
}

void ScopedEventListener::ProcessEvent(Rml::Event &event)
{
    if (mCallback) {
        mCallback(event);
    }
}

void ScopedEventListener::OnDetach(Rml::Element *element)
{
    if (element == mElement) {
        mElement = nullptr;
    }
}

} // namespace mp6::ui
