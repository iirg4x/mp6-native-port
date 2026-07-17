/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once

#include "button.hpp"
#include "window.hpp"

namespace mp6::ui {
class Modal;

struct ModalAction {
    Rml::String label;
    std::function<void(Modal &)> onPressed;
};

class Modal : public WindowSmall {
public:
    struct Props {
        Rml::String title;
        Rml::String bodyRml;
        std::vector<ModalAction> actions;
        std::function<void(Modal &)> onDismiss;
        Rml::String variant;
        Rml::String icon = "";
    };

    explicit Modal(Props props);

    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

private:
    void dismiss();

    Props mProps;
    std::vector<std::unique_ptr<Button>> mButtons;
};

} // namespace mp6::ui
