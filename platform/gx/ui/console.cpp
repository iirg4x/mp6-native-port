/* MP6 native port -- the developer console's RmlUi view. See console.hpp for
 * why RmlUi, why this class owns no state, and why it is a bar rather than a
 * window. */

#include "console.hpp"

#include <aurora/rmlui.hpp>

#include "launcher_state.hpp" /* format_document_source -- the res/ href rewrite */
#include "ui.hpp"

#include "mp6_console.h" /* the C seam: every piece of console state */

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {

    /* Bottom-up, in DOM order: the scrollback page (LOG state only), the
     * autocomplete popup, then the bar itself. The container is anchored to
     * the bottom edge and sized by its content, so everything grows UPWARD
     * from the prompt -- which is what makes the popup appear above the line
     * being typed without a single measured coordinate. */
    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/console.rcss" />
</head>
<body>
    <console id="console">
        <logpage id="logpage"><output id="output"/></logpage>
        <acpopup id="acpopup"/>
        <cmdbar id="cmdbar"><prompt>&gt;</prompt></cmdbar>
    </console>
</body>
</rml>
)RML";

    Rml::String formatted_source(const Rml::String &raw)
    {
        char buf[4096];
        mp6::ui::format_document_source(raw.c_str(), buf, sizeof(buf));
        return Rml::String(buf);
    }

    /* The whole output ring is emitted and the pane scrolls (mouse wheel and
     * PgUp/PgDn on desktop, RmlUi's own touch-drag + inertia on Android --
     * Context::ProcessTouchMove finds the nearest scroll container by itself,
     * so `overflow-y: auto` in console.rcss is the entire Android story).
     * 512 is deliberately larger than console_core.c's CON_LOG_LINES (256) so
     * this constant can never become the real cap without anyone noticing. */
    constexpr int kOutputLines = 512;

    /* Treat "within one line of the bottom" as pinned, so a fresh log line
     * still follows the tail while the user is reading it, and does NOT yank
     * the view back down while they are scrolled up. */
    constexpr float kOutputPinSlackPx = 24.0f;

    /* One record's fields, after the leading kind character. */
    void split_record(const Rml::String &line, std::vector<Rml::String> &out)
    {
        out.clear();
        size_t at = (line.size() > 1 && line[1] == '\t') ? 2 : 1;
        if (at > line.size()) {
            out.emplace_back();
            return;
        }
        for (;;) {
            const size_t tab = line.find('\t', at);
            if (tab == Rml::String::npos) {
                out.push_back(line.substr(at));
                break;
            }
            out.push_back(line.substr(at, tab - at));
            at = tab + 1;
        }
    }

    /* R-record kind -> the rcss class carrying the reference's colour. '.' is
     * the plain green value row and needs no class at all. */
    const char *row_class(char kind)
    {
        switch (kind) {
            case 'p': return "pool";
            case 'c': return "cat";
            case 'g': return "good";
            case 'r': return "bad";
            case 'x': return "info";
            default: return nullptr;
        }
    }

    Rml::Input::KeyIdentifier event_key(const Rml::Event &event)
    {
        return static_cast<Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
    }

} // namespace

void build_stat_records(Rml::Element *parent, const char *records) noexcept
{
    if (parent == nullptr) {
        return;
    }
    parent->SetInnerRML("");
    if (records == nullptr) {
        return;
    }

    /* A <ptable> per column-header record, so each section's fixed-width
     * numeric columns are scoped to its own rows -- two sections with
     * different column counts must not fight over one width. */
    Rml::Element *table = nullptr;
    std::vector<Rml::String> f;
    const char *p = records;
    while (*p != '\0') {
        const char *eol = std::strchr(p, '\n');
        const Rml::String line(p, eol != nullptr ? static_cast<size_t>(eol - p) : std::strlen(p));
        p = (eol != nullptr) ? eol + 1 : p + line.size();
        if (line.empty()) {
            continue;
        }
        split_record(line, f);
        switch (line[0]) {
            case MP6_CONSOLE_REC_TITLE:
                append(parent, "ptitle")->SetInnerRML(escape(f[0]));
                table = nullptr;
                break;
            case MP6_CONSOLE_REC_GROUP:
                append(parent, "pgroup")->SetInnerRML(escape(f[0]));
                table = nullptr;
                break;
            case MP6_CONSOLE_REC_COLS: {
                table = append(parent, "ptable");
                auto *head = append(table, "phead");
                for (size_t i = 0; i < f.size(); ++i) {
                    auto *cell = append(head, "pcell");
                    if (i == 0) {
                        cell->SetClass("lbl", true);
                    }
                    cell->SetInnerRML(escape(f[i]));
                }
                break;
            }
            case MP6_CONSOLE_REC_ROW: {
                if (f.size() < 3) {
                    break;
                }
                if (table == nullptr) {
                    table = append(parent, "ptable");
                }
                auto *row = append(table, "prow");
                if (const char *cls = row_class(f[0].empty() ? '.' : f[0][0])) {
                    row->SetClass(cls, true);
                }
                /* The reference's red proportional bar: an absolutely
                 * positioned strip behind the row, width = this row's share of
                 * the hottest one. Inline because it is per-row data, not
                 * style.
                 *
                 * The bar is why <prow> is a plain block and the cells live in
                 * a nested <pcells> flex box rather than in the row directly.
                 * RmlUi's FlexFormattingContext hands an absolutely positioned
                 * child of a flex container to LayoutDetails::GetContainingBlock
                 * (FlexFormattingContext.cpp:258), which resolves PAST the flex
                 * container -- so a bar parented to a flex row had the stat
                 * PANE as its containing block and every row's bar collapsed
                 * into one smudge in the pane's top-left corner. A block row is
                 * its own absolute containing block, so `width: N%` means "N%
                 * of this row". */
                const int bar = std::atoi(f[1].c_str());
                if (bar > 0) {
                    append(row, "pbar")->SetProperty("width", std::to_string(bar) + "%");
                }
                auto *cells = append(row, "pcells");
                for (size_t i = 2; i < f.size(); ++i) {
                    auto *cell = append(cells, "pcell");
                    if (i == 2) {
                        cell->SetClass("lbl", true);
                    }
                    cell->SetInnerRML(escape(f[i]));
                }
                break;
            }
            case MP6_CONSOLE_REC_MORE:
                append(table != nullptr ? table : parent, "pmore")->SetInnerRML(escape(f[0]));
                break;
            case MP6_CONSOLE_REC_NOTE:
                append(parent, "pnote")->SetInnerRML(escape(f[0]));
                table = nullptr;
                break;
            default:
                break;
        }
    }
}

Console::Console()
    : Document(formatted_source(kDocumentSource))
{
    mRoot = mDocument->GetElementById("console");
    mLogPage = mDocument->GetElementById("logpage");
    mOutput = mDocument->GetElementById("output");
    mPopup = mDocument->GetElementById("acpopup");

    /* ONE CAPTURE-PHASE KEY HANDLER FOR THE WHOLE BAR.
     *
     * The capture phase is not a stylistic choice, it is the only phase that
     * works here, and the reason is the same one that forced it for the toggle
     * key: aurora's window.cpp process_event() hands every SDL event to
     * rmlui::handle_event() BEFORE appending it to the array the game walks
     * later, and RmlUi dispatches Keydown to the focused input's own
     * WidgetTextInput capture-phase listener -- which is where the character
     * is actually inserted and where Tab/Up/Down are actually consumed. A
     * bubble-phase listener runs after all of that. Rml::EventDispatcher walks
     * listeners sorted by DOM distance, so a capture-phase listener on the
     * DOCUMENT runs before the widget's own, and StopPropagation() breaks the
     * walk at the next DOM level.
     *
     * So every key the bar owns -- Tab and the arrows for the completion
     * popup, Enter, Escape, PgUp/PgDn -- is decided here, in one place, before
     * the text field sees any of it.
     *
     * mSwallowNextText is the second half of the toggle-key fix: closing the
     * bar with ` left a backtick sitting in the field, because the
     * SDL_EVENT_TEXT_INPUT that follows the keydown is a separate event. One
     * keydown arms the swallow, one textinput spends it, any other key disarms
     * it. The `!is_open()` clause is the mirror case: while the C core says
     * the bar is closed, no text may enter the field at all -- a hidden
     * document whose input still holds RmlUi focus would otherwise quietly
     * accumulate whatever the player typed at the game. */
    listen(mDocument, Rml::EventId::Keydown, [this](Rml::Event &event) {
        const auto key = event_key(event);
        mSwallowNextText = (key == Rml::Input::KI_OEM_3 /* ` on the US layout */ ||
                            key == Rml::Input::KI_F9 ||
                            key == Rml::Input::KI_TAB);
        if (mp6_console_is_open() == 0) {
            return;
        }
        if (handle_console_key(event)) {
            mSwallowNextText = true;
            event.StopPropagation();
        }
    }, /*capture=*/true);
    listen(mDocument, Rml::EventId::Textinput, [this](Rml::Event &event) {
        const bool swallow = mSwallowNextText || mp6_console_is_open() == 0;
        mSwallowNextText = false;
        if (swallow) {
            event.StopPropagation();
        }
    }, /*capture=*/true);

    /* The command line. Reusing Rml::ElementFormControlInput verbatim is not a
     * style choice: it is the only text path in this tree that is proven to
     * raise the Android soft keyboard, via aurora::rmlui::set_input_type() ->
     * SystemInterface_Aurora::ActivateKeyboard -> SDL_StartTextInput. */
    if (auto *cmdbar = mDocument->GetElementById("cmdbar")) {
        auto elemPtr = mDocument->CreateElement("input");
        mInput = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(elemPtr.get());
        if (mInput != nullptr) {
            mInput->SetAttribute("type", "text");
            mInput->SetAttribute("value", "");
            mInput->SetAttribute("maxlength", 120);
            cmdbar->AppendChild(std::move(elemPtr));

            /* Clicks inside the field must not bubble out to the document's own
             * nav handling and steal focus back. */
            listen(mInput, Rml::EventId::Click,
                   [](Rml::Event &event) { event.StopPropagation(); });
        }
    }

    mp6_console_log("dev console ready -- `help` lists commands, TAB completes");
    mp6_console_log("` cycles bar -> bar+log -> closed (shift+` jumps to the log)");
    mp6_console_log("`stat <panel>` raises a PERSISTENT overlay that survives closing this");
    mp6_console_log("bar -- that is the point. `stat none` drops every one of them.");
}

/* --- the autocomplete popup ------------------------------------------- */

void Console::hide_completions()
{
    mAcTotal = 0;
    mAcShown = 0;
    mAcSel = 0;
    if (mPopup != nullptr) {
        mPopup->SetInnerRML("");
        mPopup->RemoveAttribute("open");
    }
}

void Console::refresh_completions(bool force)
{
    if (mInput == nullptr || mPopup == nullptr) {
        return;
    }
    const Rml::String line = mInput->GetValue();
    if (!force && line == mAcLine) {
        return;
    }
    const bool lineChanged = (line != mAcLine);
    mAcLine = line;
    if (lineChanged) {
        mAcDismissed = false; /* a new keystroke re-arms a popup Esc dismissed */
    }

    if (mAcDismissed || line.empty()) {
        hide_completions();
        return;
    }

    const char *raw[kAcMax] = {};
    int tokenStart = 0;
    const int total = mp6_console_complete(line.c_str(), raw, kAcMax, &tokenStart);
    mAcTokenStart = tokenStart;
    mAcTotal = total;
    mAcShown = (total < kAcMax) ? total : kAcMax;
    if (mAcSel >= mAcShown) {
        mAcSel = 0;
    }
    if (mAcShown == 0) {
        hide_completions();
        return;
    }
    for (int i = 0; i < mAcShown; ++i) {
        mAcMatch[i] = (raw[i] != nullptr) ? Rml::String(raw[i]) : Rml::String();
    }

    /* The reference caps the list and says how many it hid, with the count row
     * at the TOP of the popup -- which is where it stays readable when the
     * list is rising off a bar at the bottom of the screen. */
    mPopup->SetInnerRML("");
    if (total > mAcShown) {
        append(mPopup, "acmore")
            ->SetInnerRML(escape("[" + std::to_string(total - mAcShown) + " more matches]"));
    }
    for (int i = 0; i < mAcShown; ++i) {
        auto *row = append(mPopup, "acrow");
        if (row == nullptr) {
            continue;
        }
        row->SetClass("sel", i == mAcSel);
        row->SetInnerRML(escape(mAcMatch[i]));
    }
    mPopup->SetAttribute("open", "");
}

void Console::move_completion(int delta)
{
    if (mAcShown <= 0) {
        return;
    }
    mAcSel += delta;
    while (mAcSel < 0) {
        mAcSel += mAcShown;
    }
    mAcSel %= mAcShown;
    /* Only the highlight moves -- rebuilding the rows would be a DOM churn per
     * keypress for a class flip. */
    int index = 0;
    for (int i = 0; i < mPopup->GetNumChildren(); ++i) {
        auto *child = mPopup->GetChild(i);
        if (child == nullptr || child->GetTagName() != "acrow") {
            continue;
        }
        child->SetClass("sel", index == mAcSel);
        ++index;
    }
}

/* Returns false when there is nothing left to complete -- which is what makes
 * Enter unambiguous. `stat memory` typed in full still has a live popup (one
 * match: `memory`), and completing it there would replace the word with
 * itself, so the first Enter would only append a space and the command would
 * need a SECOND Enter to run. Comparing the typed token against the
 * highlighted match is the whole fix: identical means the word is finished, so
 * Enter submits and Tab is a no-op. */
bool Console::apply_completion()
{
    if (mInput == nullptr || mAcShown <= 0 || mAcSel < 0 || mAcSel >= mAcShown) {
        return false;
    }
    const Rml::String line = mInput->GetValue();
    const size_t start = static_cast<size_t>(mAcTokenStart);
    if (start > line.size()) {
        return false;
    }
    if (line.compare(start, Rml::String::npos, mAcMatch[mAcSel]) == 0) {
        return false; /* already exactly this word */
    }
    mInput->SetValue(line.substr(0, start) + mAcMatch[mAcSel] + " ");
    refresh_completions(/*force=*/true);
    return true;
}

/* --- keys -------------------------------------------------------------- */

bool Console::handle_console_key(Rml::Event &event)
{
    const auto key = event_key(event);
    const bool popup = (mAcShown > 0);
    switch (key) {
        case Rml::Input::KI_TAB:
            /* Always consumed while the bar is up: letting Tab through would
             * move RmlUi's focus off the command line, and a console you have
             * to click back into is broken. */
            apply_completion();
            return true;
        case Rml::Input::KI_UP:
            if (popup) {
                move_completion(-1);
            } else {
                recall_history(+1);
            }
            return true;
        case Rml::Input::KI_DOWN:
            if (popup) {
                move_completion(+1);
            } else {
                recall_history(-1);
            }
            return true;
        case Rml::Input::KI_RETURN:
        case Rml::Input::KI_NUMPADENTER:
            /* Enter COMPLETES when the highlighted match is not already what
             * is typed, and SUBMITS otherwise. That is the reference's
             * behaviour and it is deterministic: the first Enter finishes the
             * word, the second runs it. */
            if (popup && apply_completion()) {
                return true;
            }
            submit_line();
            return true;
        case Rml::Input::KI_ESCAPE:
            if (popup) {
                mAcDismissed = true;
                hide_completions();
            } else {
                mp6_console_close();
            }
            return true;
        /* PgUp/PgDn are the desktop scrollback keys. They have to be handled
         * here rather than left to RmlUi's own scroll handling: the command
         * line owns keyboard focus for the whole session, and an input element
         * is not a scroll container, so nothing would otherwise move the log
         * page without a mouse. */
        case Rml::Input::KI_PRIOR:
            scroll_output(-1.0f);
            return true;
        case Rml::Input::KI_NEXT:
            scroll_output(+1.0f);
            return true;
        default:
            break;
    }
    return false;
}

/* --- the scrollback page ----------------------------------------------- */

bool Console::output_at_bottom() const
{
    if (mOutput == nullptr) {
        return true;
    }
    const float max = mOutput->GetScrollHeight() - mOutput->GetClientHeight();
    return max <= 0.0f || mOutput->GetScrollTop() >= max - kOutputPinSlackPx;
}

void Console::scroll_output(float pages)
{
    if (mOutput == nullptr || mp6_console_log_page() == 0) {
        return;
    }
    const float max = mOutput->GetScrollHeight() - mOutput->GetClientHeight();
    if (max <= 0.0f) {
        return;
    }
    float top = mOutput->GetScrollTop() + pages * mOutput->GetClientHeight();
    if (top < 0.0f) {
        top = 0.0f;
    }
    if (top > max) {
        top = max;
    }
    mOutput->SetScrollTop(top);
}

void Console::refresh_output()
{
    if (mOutput == nullptr) {
        return;
    }
    /* Decide BEFORE the rebuild: SetInnerRML resets the scroll offset, so
     * "was the user reading the tail" is only answerable against the old
     * content. */
    const bool pinned = output_at_bottom();

    Rml::String text;
    const int count = mp6_console_log_count();
    const int first = (count > kOutputLines) ? kOutputLines - 1 : count - 1;
    /* The WHOLE retained ring, oldest first, not a window of the last few:
     * a `help` or a `toggles` is longer than the pane, and a scrollback that
     * cannot reach what the command just printed is not a scrollback. */
    for (int back = first; back >= 0; --back) {
        const char *line = mp6_console_log_line(back);
        if (line == nullptr) {
            continue;
        }
        if (!text.empty()) {
            text += "\n";
        }
        text += escape(line);
    }
    mOutput->SetInnerRML(text);

    /* Element::SetScrollTop clamps against GetScrollHeight(), which is still
     * the PRE-rebuild height until RmlUi lays the new content out -- and that
     * happens inside aurora_end_frame()'s record_frame(), after this call.
     * So the re-pin is armed here and spent one update() later, when the
     * clamp is against the height the user will actually see. */
    if (pinned) {
        mPinOutputFrames = 2;
    }
}

void Console::submit_line()
{
    if (mInput == nullptr) {
        return;
    }
    const Rml::String line = mInput->GetValue();
    mInput->SetValue("");
    mHistoryCursor = -1;
    hide_completions();
    mAcLine.clear();
    if (line.empty()) {
        return;
    }
    mp6_console_log("> %s", line.c_str());
    mp6_console_exec(line.c_str());
}

void Console::recall_history(int direction)
{
    if (mInput == nullptr) {
        return;
    }
    const int count = mp6_console_history_count();
    if (count == 0) {
        return;
    }
    int next = mHistoryCursor + direction;
    if (next < -1) {
        next = -1;
    }
    if (next >= count) {
        next = count - 1;
    }
    mHistoryCursor = next;
    if (mHistoryCursor < 0) {
        mInput->SetValue("");
        return;
    }
    const char *entry = mp6_console_history(mHistoryCursor);
    mInput->SetValue(entry != nullptr ? entry : "");
    /* A recalled line is a whole command, not a prefix being typed: showing
     * its completions would cover the log with a popup nobody asked for. */
    mAcLine = mInput->GetValue();
    hide_completions();
}

void Console::show()
{
    Document::show();
    mp6_console_open();
    /* RmlUi lays the input out during render; focusing it a frame later is what
     * gives the mobile keyboard a valid caret rectangle to place itself
     * against -- the same two-frame wait BaseStringButton uses. */
    mPendingFocusFrames = 2;
}

void Console::hide(bool close)
{
    Document::hide(close);
    mp6_console_close();
}

bool Console::focus()
{
    if (mInput != nullptr && mInput->Focus(true)) {
        return true;
    }
    return false;
}

bool Console::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    (void)event;
    if (cmd == NavCommand::Cancel) {
        mp6_console_close();
        return true;
    }
    if (cmd == NavCommand::Menu) {
        /* F1 / gamepad Back belong to the in-game settings menu. Swallow rather
         * than inherit Document's toggle(), which would fight the ` hotkey for
         * ownership of the bar's state. */
        return true;
    }
    return false;
}

void Console::update()
{
    Document::update();
    if (mDocument == nullptr) {
        return;
    }

    /* The C core is the single source of truth for the bar's state: the ` and
     * F9 hotkeys and the Android button all move it there, and this view
     * follows. A one-frame lag is invisible and keeps the state
     * un-duplicated. */
    const int bar = mp6_console_bar();
    if (bar != mSeenBar) {
        const bool wasOpen = (mSeenBar >= MP6_CONSOLE_BAR_INPUT);
        const bool isOpen = (bar >= MP6_CONSOLE_BAR_INPUT);
        mSeenBar = bar;
        if (mRoot != nullptr) {
            mRoot->SetAttribute("mode", bar == MP6_CONSOLE_BAR_LOG ? "log" : "input");
        }
        if (isOpen && !wasOpen) {
            Document::show();
            mPendingFocusFrames = 2;
        } else if (!isOpen && wasOpen) {
            Document::hide(false);
            hide_completions();
            mAcLine.clear();
        }
        if (bar == MP6_CONSOLE_BAR_LOG) {
            /* Entering the log page: repaint it now rather than waiting for the
             * next log line, and pin it to the tail. */
            mSeenLogTotal = mp6_console_log_total();
            refresh_output();
            mPinOutputFrames = 2;
        }
    }
    if (bar == MP6_CONSOLE_BAR_CLOSED) {
        return;
    }

    if (mPendingFocusFrames > 0 && --mPendingFocusFrames == 0 && mInput != nullptr) {
        /* The proven soft-keyboard path (see console.hpp). */
        aurora::rmlui::set_input_type(aurora::rmlui::InputType::Text);
        mInput->Focus(true);
    }

    if (mPinOutputFrames > 0 && --mPinOutputFrames == 0 && mOutput != nullptr) {
        mOutput->SetScrollTop(mOutput->GetScrollHeight()); /* clamped by RmlUi */
    }

    /* The popup follows what is in the field. One string compare per frame is
     * the whole cost while nothing is being typed; the DOM is only touched
     * when the line actually CHANGED, which takes a keystroke -- so this can
     * never fire on an interpolated present (mp6_launcher_frame_overlay()
     * reaches this function from the replay path too, which is why the panel
     * refresh this file used to own had to be tick-rate-limited). A rebuild
     * bounded by real input needs no rate limit; a rebuild on a clock does. */
    refresh_completions(/*force=*/false);

    /* The log DOM is built ONLY while the log page is up, and only on a
     * generation change -- the ring's own counter. A quiet bar with no log
     * page rebuilds nothing at all, which is what keeps the bar's cost a
     * rounding error inside the excluded overlay bucket. */
    if (bar == MP6_CONSOLE_BAR_LOG) {
        const int logTotal = mp6_console_log_total();
        if (logTotal != mSeenLogTotal) {
            mSeenLogTotal = logTotal;
            refresh_output();
        }
    }
}

} // namespace mp6::ui
