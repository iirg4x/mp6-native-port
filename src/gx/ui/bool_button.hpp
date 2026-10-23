/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once
#include "select_button.hpp"

namespace mp6::ui {

class BoolButton : public BaseControlledSelectButton {
public:
    struct Props {
        Rml::String key;
        Rml::String icon;
        std::function<bool()> getValue;
        std::function<void(bool)> setValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
    };

    BoolButton(Rml::Element *parent, Props props);

    bool modified() const override;
    bool disabled() const override;
    void update() override;
    bool can_step(int direction) const override { return !disabled() && (direction < 0 ? mGetValue()!=0 : mGetValue()==0); }

protected:
    Rml::String format_value() override;
    bool handle_nav_command(NavCommand cmd) override;

private:
    std::function<int()> mGetValue;
    std::function<void(int)> mSetValue;
    std::function<bool()> mIsDisabled;
    std::function<bool()> mIsModified;
    Rml::Element *mSwitch = nullptr;
};

} // namespace mp6::ui
