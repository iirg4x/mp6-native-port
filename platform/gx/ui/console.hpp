/* MP6 native port -- the developer console's RmlUi VIEW.
 *
 * It holds no console state. The bar's state, the overlay latches, the command
 * registry, the output ring, the history, the completion rules and every
 * runtime lever live in platform/gx/console/console_core.c
 * (shim/include/mp6_console.h), which links headless and is driven directly by
 * tools/console_selftest.c. This class only mirrors that state into a document
 * and feeds typed lines back into mp6_console_exec().
 *
 * WHAT THIS DOCUMENT IS NOW. A BOTTOM INPUT BAR, not a window: one translucent
 * line pinned to the bottom edge with a `> ` prompt, an autocomplete popup
 * rising above it, and -- on the toggle key's second press -- a scrollback page
 * above that. It contains no stat panel at all. Every stat panel is a
 * persistent OVERLAY drawn by overlay.cpp, because a panel that dies when the
 * console closes cannot be watched while playing, which is the only reason to
 * have one.
 *
 * WHY RMLUI RATHER THAN IMGUI. Both overlay layers are already live on every
 * frame, but the console needs a real text field, and the RmlUi path is the
 * one proven end to end in this tree: platform/gx/ui/string_button.cpp creates
 * an Rml::ElementFormControlInput and calls aurora::rmlui::set_input_type(),
 * which reaches SystemInterface_Aurora's SDL_StartTextInput -- the exact call
 * that raises the Android soft keyboard. Event arbitration is already correct
 * too (aurora's window.cpp gives ImGui first refusal and rmlui.cpp early-
 * returns when ImGui claims an event), so no aurora patch is needed.
 */
#pragma once

#include "document.hpp"

#include <RmlUi/Config/Config.h>

namespace mp6::ui {

/* Lays one PANEL RECORD BLOCK (the T/G/C/R/M/N grammar documented in
 * shim/include/mp6_console.h) out under `parent`, replacing its contents.
 *
 * It lives here rather than inside Console because the stat panels are drawn
 * by overlay.cpp -- the always-on passive document that also owns the FPS
 * badge `stat unit` absorbs -- and both consume the same grammar. One builder
 * means the reference's colours, its right-aligned numeric columns and its red
 * proportional bar are defined once, in res/rml/console.rcss and
 * res/rml/overlay.rcss, for every surface that shows a panel.
 *
 * Column alignment is LAYOUT, not a font: numeric cells are fixed-width and
 * right-aligned so they line up under a proportional face. res/fonts has no
 * monospace family and adding one is a licensing decision, not a port one. */
void build_stat_records(Rml::Element *parent, const char *records) noexcept;

class Console : public Document {
public:
    Console();

    void update() override;
    void show() override;
    void hide(bool close) override;
    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

private:
    /* The completion popup. Every one of these consults
     * mp6_console_complete(); this class stores only which row is highlighted
     * and which line the current list was built for. */
    static constexpr int kAcMax = 8; /* rows shown; the rest become "[N more]" */

    bool handle_console_key(Rml::Event &event); /* true = consumed */
    void refresh_completions(bool force);
    void move_completion(int delta);
    bool apply_completion();
    void hide_completions();

    void refresh_output();
    void submit_line();
    void recall_history(int direction);
    void scroll_output(float pages);
    bool output_at_bottom() const;

    Rml::Element *mRoot = nullptr;
    Rml::Element *mLogPage = nullptr;
    Rml::Element *mOutput = nullptr;
    Rml::Element *mPopup = nullptr;
    Rml::ElementFormControlInput *mInput = nullptr;

    int  mSeenLogTotal = -1;
    int  mSeenBar = -1;            /* the bar state this view last applied */
    int  mPendingFocusFrames = 0;
    int  mHistoryCursor = -1;      /* -1 = editing a fresh line */
    bool mSwallowNextText = false; /* the toggle key's own character (see the ctor) */
    int  mPinOutputFrames = 0;     /* re-pin the scrollback to the tail, post-layout */

    Rml::String mAcLine;           /* the line the popup was last built for */
    int  mAcTotal = 0;             /* mp6_console_complete()'s TOTAL, not the shown count */
    int  mAcShown = 0;
    int  mAcSel = 0;
    int  mAcTokenStart = 0;
    bool mAcDismissed = false;     /* Esc closed it; re-arms when the line changes */
    Rml::String mAcMatch[kAcMax];
};

} // namespace mp6::ui
