/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once

namespace mp6::ui {

enum class NavCommand {
    None,
    Up,
    Down,
    Left,
    Right,
    Next, // R1
    Previous, // L1
    Confirm, // A
    Cancel, // B
    Menu, // Back/Minus, or R + Start
};

} // namespace mp6::ui
