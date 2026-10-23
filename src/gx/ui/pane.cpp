/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#include "pane.hpp"
#include "../../audio/menu_sfx.hpp"

#include "ui.hpp"

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
        auto elem = doc->CreateElement("pane");
        return parent->AppendChild(std::move(elem));
    }

} // namespace

Pane::Pane(Rml::Element *parent, Type type)
    : FluentComponent(createRoot(parent))
    , mType(type)
{
    mRoot->SetClass(type == Type::Controlled ? "settings-list" : "settings-detail", true);
    if (type == Type::Uncontrolled) {
        listen(Rml::EventId::Focus, [this](Rml::Event &event) {
            for (const auto &child : mChildren)
                if (child->contains(event.GetTargetElement())) mDetailOwner = child.get();
        }, true);
    }
    listen(Rml::EventId::Keydown, [this](Rml::Event &event) {
        const auto cmd = map_nav_event(event);

        if (mType == Type::Controlled && mRoot->Closest(".card-deck")) {
            if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
                move_focus(cmd == NavCommand::Right ? 1 : -1);
                event.StopPropagation();
            }
            return;
        }

        // If navigating to the next pane, select the focused item
        if (mType == Type::Controlled && cmd == NavCommand::Right && !mRoot->Closest(".storybook")) {
            auto *target = event.GetTargetElement();
            int focusedChild = -1;
            for (size_t i = 0; i < mChildren.size(); ++i) {
                if (mChildren[i]->contains(target)) {
                    focusedChild = i;
                    break;
                }
            }
            if (focusedChild == -1) {
                return;
            }
            set_selected_item(focusedChild);
            return;
        }

        int direction = 0;
        if (cmd == NavCommand::Down) {
            direction = 1;
        }
        else if (cmd == NavCommand::Up) {
            direction = -1;
        }
        else {
            return;
        }
        auto *target = event.GetTargetElement();
        int focusedChild = -1;
        for (size_t i = 0; i < mChildren.size(); ++i) {
            if (mChildren[i]->contains(target)) {
                focusedChild = i;
                break;
            }
        }
        if (focusedChild == -1) {
            return;
        }
        int i = focusedChild + direction;
        while (i >= 0 && i < mChildren.size()) {
            if (mChildren[i]->focus()) {
                // mDoAud_seStartMenu(kSoundItemFocus); // TODO PC
                event.StopPropagation();
                break;
            }
            i += direction;
        }
    });

    if (type == Type::Controlled) {
        // For controlled panes, handle SelectButton Submit events for item selection
        listen(Rml::EventId::Submit, [this](Rml::Event &event) {
            int childIndex = -1;
            for (int i = 0; i < mChildren.size(); ++i) {
                if (event.GetTargetElement() == mChildren[i]->root()) {
                    childIndex = i;
                }
            }
            // If item already selected, deselect
            if (childIndex >= 0 && childIndex < mChildren.size() && mChildren[childIndex]->selected()) {
                childIndex = -1;
            }
            set_selected_item(childIndex);
            // If the selection was handled locally, don't allow it to bubble up to window
            if (event.GetParameter("handled", false)) {
                event.StopPropagation();
            }
        });
    }
}

void Pane::update()
{
    finalize();
    Component::update();
}

void Pane::set_selected_item(int index)
{
    if (mType == Type::Uncontrolled) {
        return;
    }
    for (int i = 0; i < mChildren.size(); ++i) {
        mChildren[i]->set_selected(i == index);
    }
}

int Pane::focused_index() const
{
    for (int i = 0; i < static_cast<int>(mChildren.size()); ++i)
        if (mChildren[i].get() == mDetailOwner) return i;
    return -1;
}

void Pane::promote_first(const Rml::String &title)
{
    for (auto it=mChildren.begin(); it!=mChildren.end(); ++it) {
        if ((*it)->root()->GetAttribute<Rml::String>("data-title", "") != title) continue;
        auto *row=(*it)->root();
        if (row!=mRoot->GetFirstChild()) mRoot->InsertBefore(mRoot->RemoveChild(row),mRoot->GetFirstChild());
        std::rotate(mChildren.begin(),it,it+1);
        break;
    }
}

bool Pane::move_focus(int direction)
{
    if (direction != -1 && direction != 1) return false;
    for (int i = focused_index() + direction; i >= 0 && i < static_cast<int>(mChildren.size()); i += direction)
        if (mChildren[i]->focus()) return true;
    return false;
}

void Pane::activate_focused()
{
    if (mDetailOwner && !mDetailOwner->disabled())
        mDetailOwner->root()->DispatchEvent(Rml::EventId::Click, {});
}

bool Pane::cycle_choice(int direction)
{
    // Only enumerated choices participate, never Save/Load/reset actions.
    std::vector<ControlledButton *> options;
    for (const auto &child : mChildren)
        if (auto *choice = dynamic_cast<ControlledButton *>(child.get()); choice && !choice->disabled())
            options.push_back(choice);
    if (options.empty()) return false;
    int current = direction > 0 ? -1 : int(options.size());
    for (int i=0; i<int(options.size()); ++i)
        if (options[i]->selected()) { current=i; break; }
    current += direction;
    if(current>=0 && current<int(options.size())) options[current]->root()->DispatchEvent(Rml::EventId::Click, {});
    return true;
}

Component &Pane::register_control(Component &component, Pane &nextPane, std::function<void(Pane &)> callback)
{
    const bool book = mRoot->Closest(".storybook") != nullptr;
    if (mRoot->Closest("window.party-settings")) {
        if (auto *button = dynamic_cast<Button *>(&component); button && !button->has_action())
            button->on_pressed([root = button->root()] { root->DispatchEvent(Rml::EventId::Submit, {}); });
        Rml::String title;
        if (auto *key = component.root()->QuerySelector("key")) title = key->GetInnerRML();
        else if (auto *button = dynamic_cast<Button *>(&component)) title = button->get_text();
        component.root()->SetAttribute("data-title", title);
        component.root()->SetAttribute("data-group", mSectionTitle);
        if (book) {
            if (auto *choice = dynamic_cast<SelectButton *>(&component))
                choice->on_step([&nextPane](int direction) { return nextPane.cycle_choice(direction); });
            if (auto *choice = dynamic_cast<ControlledSelectButton *>(&component); choice && callback) {
                // Cache value predicates, not temporary option-widget pointers.
                // Every row's limits remain current even when it isn't focused.
                nextPane.clear();callback(nextPane);
                std::vector<std::function<bool()>> selected;
                for(const auto& child:nextPane.mChildren)
                    if(auto* option=dynamic_cast<ControlledButton*>(child.get());option&&!option->disabled())
                        selected.push_back(option->selection_predicate());
                choice->step_availability([selected=std::move(selected)](int direction) {
                    if(selected.empty())return false;
                    for(int i=0;i<int(selected.size());++i)if(selected[i]&&selected[i]())
                        return direction<0 ? i>0 : i+1<int(selected.size());
                    return true; // A custom value can enter the preset range from either end.
                });
                nextPane.clear();
            }
        }
        callback = [title, build = std::move(callback)](Pane &pane) {
            if (build) build(pane);
            auto *first = pane.root()->GetFirstChild();
            if (!title.empty() && (!first || !first->IsClassSet("section-heading"))) {
                auto *heading = pane.add_section(title);
                if (first) pane.root()->InsertBefore(pane.root()->RemoveChild(heading), first);
            }
            auto *window = pane.root()->Closest(".storybook");
            auto *guide = window ? window->QuerySelector("guide-copy") : nullptr;
            if (guide) {
                Rml::String brief;
                for (int i=0; i<pane.root()->GetNumChildren(); ++i) {
                    auto *child=pane.root()->GetChild(i);
                    if (child->GetTagName() != "div" || child->IsClassSet("section-heading") ||
                        child->IsClassSet("touch-preview")) continue;
                    bool tag=false;
                    for(char c:child->GetInnerRML()) {
                        if(c=='<') {tag=true;brief+=' ';}
                        else if(c=='>')tag=false;
                        else if(!tag)brief+=c;
                    }
                    if(!brief.empty())break;
                }
                const auto start=brief.find_first_not_of(" \r\n\t");
                brief=start==Rml::String::npos ? "Choose an option to make it your party." : brief.substr(start);
                const auto sentence=brief.find(". ");
                if(sentence!=Rml::String::npos)brief.resize(sentence+1);
                if(brief.size()>200) {
                    const auto word=brief.rfind(' ',197);
                    brief.resize(word==Rml::String::npos ? 197 : word);
                    brief+="...";
                }
                guide->SetInnerRML(brief);
                window->QuerySelector("guide-title")->SetInnerRML(title);
            }
        };
    }
    auto select = [this, &component, &nextPane, callback, book](Rml::Event &) {
        if (component.disabled() || mDetailOwner == &component) return;
        if(book&&mDetailOwner)audio::play_menu_sound(audio::MenuSound::Move);
        mDetailOwner = &component;
        if (book) {
            for (const auto &child:mChildren)
                child->root()->SetClass("current-row", child.get()==&component);
            mRoot->Closest(".storybook")->SetClass("detail-open", false);
        }
        set_selected_item(-1);
        nextPane.clear();
        if (callback) callback(nextPane);
    };
    component.listen(component.root(), Rml::EventId::Mouseover, select);
    component.listen(component.root(), Rml::EventId::Focus, std::move(select));
    return component;
}

bool Pane::focus()
{
    if ((mRoot->Closest(".card-deck") || mRoot->Closest(".storybook")) && mDetailOwner && mDetailOwner->focus()) return true;
    // Focus the first selected child
    for (const auto &child : mChildren) {
        if (child->selected() && child->focus()) {
            return true;
        }
    }
    // Otherwise, focus the first focusable child
    for (const auto &child : mChildren) {
        if (child->focus()) {
            return true;
        }
    }
    return mRoot->Closest("window.party-settings") ? Component::focus() : false;
}

Rml::Element *Pane::add_section(const Rml::String &text)
{
    mSectionTitle = text;
    auto *elem = append(mRoot, "div");
    elem->SetClass("section-heading", true);
    elem->SetInnerRML(escape(text));
    return elem;
}

Rml::Element *Pane::add_text(const Rml::String &text)
{
    auto *elem = append(mRoot, "div");
    elem->SetInnerRML(escape(text));
    return elem;
}

Rml::Element *Pane::add_rml(const Rml::String &rml)
{
    auto *elem = append(mRoot, "div");
    elem->SetInnerRML(rml);
    return elem;
}

void Pane::finalize()
{
    if (finalized) {
        return;
    }
    finalized = true;

    // Append spacer element to the bottom. RmlUi does not properly handle
    // padding-bottom or margin-bottom on a scrollable flex container, so
    // we need to create a fake spacer with an actual layout height to get
    // padding at the bottom of a scrollable container.
    append(mRoot, "spacer");
}

void Pane::clear()
{
    mDetailOwner = nullptr;
    clear_children();
    finalized = false;
}

} // namespace mp6::ui
