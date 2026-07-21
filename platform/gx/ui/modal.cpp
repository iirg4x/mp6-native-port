/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "modal.hpp"

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md): host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {

Modal::Modal(Props props)
    : WindowSmall("modal", "modal-dialog")
    , mProps(std::move(props))
{
    if (!mProps.variant.empty()) {
        mRoot->SetClass(mProps.variant, true);
    }

    auto *header = append(mDialog, "div");
    header->SetClass("modal-header", true);

    auto *title = append(header, "div");
    title->SetClass("modal-title", true);
    title->SetInnerRML(mProps.title);

    if (!mProps.icon.empty()) {
        auto *icon = append(header, "icon");
        icon->SetClass(mProps.icon, true);
    }

    auto *body = append(mDialog, "div");
    body->SetClass("modal-body", true);
    body->SetInnerRML(mProps.bodyRml);

    auto *actions = append(mDialog, "div");
    actions->SetClass("modal-actions", true);

    for (auto &action : mProps.actions) {
        auto btn = std::make_unique<Button>(actions, action.label);
        btn->root()->SetClass("modal-btn", true);
        btn->on_pressed([this, callback = std::move(action.onPressed)] {
            if (callback) {
                callback(*this);
            }
        });
        mButtons.push_back(std::move(btn));
    }

    // mDoAud_seStartMenu(kSoundWindowOpen); // TODO PC
}

bool Modal::focus()
{
    if (!mButtons.empty()) {
        return mButtons.front()->focus();
    }
    return false;
}

void Modal::dismiss()
{
    if (mProps.onDismiss) {
        mProps.onDismiss(*this);
        return;
    }
    pop();
}

bool Modal::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        // mDoAud_seStartMenu(kSoundWindowClose); // TODO PC
        dismiss();
        return true;
    }

    int direction = 0;
    if (cmd == NavCommand::Left) {
        direction = -1;
    }
    else if (cmd == NavCommand::Right) {
        direction = 1;
    }
    else {
        return false;
    }

    auto *target = event.GetTargetElement();
    for (int i = 0; i < static_cast<int>(mButtons.size()); ++i) {
        if (mButtons[i]->contains(target)) {
            const int next = i + direction;
            if (next >= 0 && next < static_cast<int>(mButtons.size()) && mButtons[next]->focus()) {
                // mDoAud_seStartMenu(kSoundItemFocus); // TODO PC
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace mp6::ui
