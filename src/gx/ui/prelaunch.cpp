/* Launcher callbacks adapted from mariopartyrd/partyboard; credit TwilitRealm.
 * Presentation: the native MP6 storybook, shared with Settings.
 * Intent: get a player into their game, including a first launch with no disc.
 * Ivory layered paper, red ribbon, gold star, day/night skies and Titan One;
 * generous 112-unit action rhythm keeps all three actions above the hosts. */
#include "prelaunch.hpp"
#include "../../audio/menu_sfx.hpp"
#include "content_setup.hpp"
#include "launcher_state.hpp"
#include "modal.hpp"
#include "settings.hpp"
#include "storybook_view.hpp"
#include <dolphin/pad.h>
#include <cmath>
#include "mp6_host_section.h"


namespace mp6::ui {
namespace {
Rml::String launcher_source() {
    const char* source=R"RML(<rml><head><link type="text/rcss" href="res/rml/prelaunch.rcss" /></head>
<body><content id="root" open data-full-window-ui><div id="menu-list" /></content></body></rml>)RML";
    char formatted[4096];format_document_source(source,formatted,sizeof(formatted));return formatted;
}
}

Prelaunch::Prelaunch() : Document(launcher_source()) {
    mRoot=mDocument->GetElementById("root");
    PADInit();
    static Rml::ElementInstancerGeneric<StorybookView> instancer;
    Rml::Factory::RegisterElementInstancer("storybook-view",&instancer);
    mStorybook=static_cast<StorybookView*>(append(mRoot,"storybook-view"));
    auto* list=mDocument->GetElementById("menu-list");
    for(const char* label:{"Select Game","Settings","Quit"}) {
        const int index=int(mMenuButtons.size());
        mMenuButtons.push_back(std::make_unique<Button>(list,label));
        listen(mMenuButtons.back()->root(),Rml::EventId::Focus,[this,index](Rml::Event&){
            if(mSelected==index)return;
            audio::play_menu_sound(audio::MenuSound::Move);
            mSelected=index;
            mStoryStateDirty=true;
        });
    }
    mMenuButtons[0]->on_pressed([this]{
        if(!content_ready()){mCovered=true;push_document(std::make_unique<ContentSetup>());return;}
        mPlayOnClose=true;hide(true);
    });
    mMenuButtons[1]->on_pressed([this]{
        mRestartSuppressed=false;
        auto settings=std::make_unique<SettingsWindow>(true);
        settings->turn_from_launcher(*mStorybook);
        push(std::move(settings));
    });
    mMenuButtons[2]->on_pressed([this]{mQuitOnClose=true;hide(true);});
    listen(mStorybook,Rml::EventId::Mousedown,[this](Rml::Event& event){
        if(visible()&&event.GetParameter<int>("button",0)==0)mMenuButtons[mSelected]->focus();
    });
    listen(mStorybook,Rml::EventId::Mousemove,[this](Rml::Event& event){storybook_pointer(event,false);});
    listen(mStorybook,Rml::EventId::Click,[this](Rml::Event& event){storybook_pointer(event,true);});
}

void Prelaunch::show() {
    const bool returning=mHasShown;
    mCovered=mClosing=mPendingClose=false;mRoot->SetAttribute("open","");
    refresh_content_state();mShownContentState=-99;
    mStoryStateDirty=true;
    Document::show();
    mStorybook->enter(cfg().menuMotion!=0&&!returning);mHasShown=true;
    if(returning)mStorybook->spring_chapters(cfg().menuMotion!=0);
    if(restart_pending()&&!mRestartSuppressed){
        mCovered=true;
        const auto dismiss=[this](Modal& modal){mRestartSuppressed=true;modal.pop();};
        push_document(std::make_unique<Modal>(Modal::Props{
            .title="Apply Options",
            .bodyRml="A restart is required to apply selected options.<br/><br/>Close and reopen the port to apply them.",
            .actions={{.label="OK",.onPressed=dismiss}},.onDismiss=dismiss}));
    }
}

void Prelaunch::hide(bool close) {
    if(!close){mRoot->RemoveAttribute("open");return;}
    mClosing=mPendingClose=true;mStorybook->leave(cfg().menuMotion!=0);
}

void Prelaunch::update() {
    // Settings and content-link documents hide the launcher by removing its
    // open attribute. Keep the opening spread alive behind a modal (covered
    // keeps the attribute) and while its closing animation is running, but do
    // not rebuild/pump the hidden launcher every frame.
    if (!mRoot->HasAttribute("open") && !mClosing) return;

    const bool linked=content_ready();
    if(int(linked)!=mShownContentState){
        mShownContentState=int(linked);
        mWordmark=linked&&wordmark_available(); // Never needs disc art on first launch.
        mMenuButtons[0]->set_text(linked?"Play":"Select Game");
        mStoryStateDirty=true;
    }
    const bool motion=cfg().menuMotion!=0;
    if(!mStoryMotionValid||motion!=mStoryMotion){
        mStoryMotion=motion;
        mStoryMotionValid=true;
        mStoryStateDirty=true;
    }
    if(mStoryStateDirty){
        StorybookState state;
        state.launcher=true;state.linked=linked;state.wordmark=mWordmark;state.version=port_version();
        state.motion=motion;state.chapters={"Welcome"};state.chapter="Welcome";state.selected=mSelected;
        state.rows={{linked?"Play":"Select Game",""},{"Settings",""},{"Quit",""}};
        state.description=mSelected==0?(linked?"Start Starlit Party with your current settings.":
            "Link your Mario Party 6 (USA) disc image or extracted game folder."):
            mSelected==1?"Open the book to adjust picture, sound and game options.":"Close the native port.";
        mStoryStateDirty=false;
        mStorybook->set_state(std::move(state));
    }
    if(mClosing&&mStorybook->exit_finished()){
        Document::hide(true);
        if(mPlayOnClose)request_play();
        if(mQuitOnClose)request_quit();
        return;
    }
    // Once per launcher session, not every return from Settings or cancellation.
    // Keep the opening spread behind the modal, while its input stays blocked.
    if(!mWelcomeChecked&&!mCovered&&!mClosing){
        mWelcomeChecked=true;
        if(!linked){mCovered=true;push_document(std::make_unique<ContentSetup>());}
        else if(content_missing_movies()){mCovered=true;push_document(std::make_unique<ContentSetup>(true));}
    }
    Document::update();
}

bool Prelaunch::visible() const {return !mCovered&&!mClosing&&mRoot->HasAttribute("open");}
bool Prelaunch::focus() {return visible()&&!mMenuButtons.empty()&&mMenuButtons[mSelected]->focus();}

void Prelaunch::storybook_pointer(Rml::Event& event,bool click) {
    if(!visible()||mStorybook->entering())return;
    const auto p=mStorybook->design_point(event.GetParameter<float>("mouse_x",-100),event.GetParameter<float>("mouse_y",-100));
    if(click&&p.y>=877-mStorybook->footer_lift()&&p.y<=941-mStorybook->footer_lift()&&p.x>=540&&p.x<780){mMenuButtons[mSelected]->root()->DispatchEvent(Rml::EventId::Click,{});return;}
    const float y=StorybookView::unwarp_y(p.x,p.y)-StorybookView::launcherRowTop;
    const int row=int(std::floor(y/StorybookView::launcherRowStride));
    if(p.x<119||p.x>=812||row<0||row>=3||y-row*StorybookView::launcherRowStride>=StorybookView::launcherRowHeight)return;
    mMenuButtons[row]->focus();
    if(click)mMenuButtons[row]->root()->DispatchEvent(Rml::EventId::Click,{});
}

bool Prelaunch::handle_nav_command(Rml::Event& event,NavCommand cmd) {
    if(cmd!=NavCommand::Down&&cmd!=NavCommand::Up)return false;
    const int next=(mSelected+(cmd==NavCommand::Down?1:2))%3;
    mMenuButtons[next]->focus();event.StopPropagation();return true;
}
} // namespace mp6::ui
