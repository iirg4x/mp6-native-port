// Included in settings_qa.hpp's anonymous namespace. Native GPU and real
// callbacks, isolated config, hidden window, no save/load actions executed.
int mp6_settings_qa_run(void *windowPtr)
{
    try {
        using namespace mp6::ui;
        const auto milliseconds=[](Uint64 since){return double(SDL_GetTicksNS()-since)/1e6;};
        mp6_refresh_content_state();
        mp6::audio::update_menu_sounds("",1.f);
        settings_qa_require(!mp6::audio::menu_sounds_ready(),"no linked disc has no UI sounds");
        mp6::audio::update_menu_sounds(mp6_menu_music_root().c_str(),1.f);
        settings_qa_require(mp6::audio::menu_sounds_ready(),"four real UI sounds preload before input");
        for(auto cue:{mp6::audio::MenuSound::Move,mp6::audio::MenuSound::Confirm,mp6::audio::MenuSound::Accept,mp6::audio::MenuSound::Cancel})mp6::audio::play_menu_sound(cue);
        settings_qa_require(mp6::audio::menu_sounds_played()==4,"all four cues queue through separate dummy audio streams");
        {
            mp6::audio::MenuMusic music;
            music.update("",1.f);
            settings_qa_require(!music.playing(),"no linked disc stays silent");
            const auto audioStart=SDL_GetTicksNS();
            music.update(mp6_menu_music_root().c_str(),.35f);
            printf("[SETTINGS-PERF] first-audio %.3f ms\n",milliseconds(audioStart));
            settings_qa_require(music.playing() && music.track_count()==6 && music.queued_frames()>0,
                "launcher music decodes and queues only six linked-disc idle tracks");
            const auto first=music.current_track();
            settings_qa_require(first==2||first==3||first==4||first==6||first==7||first==92,
                "launcher selects an approved idle track");
            music.update(mp6_menu_music_root().c_str(),0.f);
            settings_qa_require(music.playing() && music.current_track()==first,"master mute does not restart menu music");
            music.update("",1.f);
            settings_qa_require(!music.playing() && music.track_count()==0,"unlink closes menu music");
            music.update("missing-menu-music-fixture",1.f);
            settings_qa_require(!music.playing(),"missing bank stays silent");
            music.update(mp6_menu_music_root().c_str(),1.f);
            settings_qa_require(music.playing(),"relink resumes menu music");
        }
        cfg().menuMotion=0;
        const auto createStart=SDL_GetTicksNS();
        auto menu=std::make_unique<SettingsProbe>();auto* probe=menu.get();push_document(std::move(menu));
        printf("[SETTINGS-PERF] construct-show %.3f ms\n",milliseconds(createStart));
        auto* context=probe->document()->GetContext();
        auto* renderer=static_cast<aurora::rmlui::WebGPURenderInterface *>(Rml::GetRenderInterface());
        int width=1440,height=900;float dpi=1;
        unsigned frameNumber=0;
        auto frame=[&](const char* capture=nullptr,int frames=3){
            aurora::rmlui::set_ui_scale(dpi);
            settings_qa_require(SDL_SetWindowSize(static_cast<SDL_Window *>(windowPtr),width,height),"hidden viewport resize");
            auto target=aurora::webgpu::create_render_texture(width,height,false);
            for(int i=0;i<frames;++i){aurora_update();if(!aurora_begin_frame())throw std::runtime_error("hidden frame unavailable");
                context->SetDimensions({width,height});context->SetDensityIndependentPixelRatio(dpi);
                const auto updateStart=SDL_GetTicksNS();
                mp6::ui::update();context->Update();renderer->SetWindowSize({width,height});
                const double updateMs=milliseconds(updateStart);
                renderer->BeginFrame(target,aurora::webgpu::present_source(),aurora::rmlui::BaseLayerContent::Transparent);
                const auto renderStart=SDL_GetTicksNS();
                context->Render();const double renderMs=milliseconds(renderStart);
                renderer->EndFrame();aurora_end_frame();
                if(frameNumber<3)printf("[SETTINGS-PERF] frame %u update=%.3f render=%.3f ms\n",frameNumber,updateMs,renderMs);
                // A generous cold-open budget catches the original ~334ms PNG
                // decode/upload hitch without asserting a machine-specific FPS.
                if(frameNumber==0)settings_qa_require(renderMs<50,"first settings render stays below 50ms");
                ++frameNumber;}
            if(capture)settings_qa_capture(target,capture);
            // SDL may clamp a deliberately tiny/portrait hidden window. Restore
            // the capture viewport before DOM-bound assertions and hit tests.
            context->SetDimensions({width,height});context->SetDensityIndependentPixelRatio(dpi);context->Update();
        };
        auto key=[&](Rml::Input::KeyIdentifier k){context->ProcessKeyDown(k,0);context->ProcessKeyUp(k,0);};
        auto row=[&](const char* name){Rml::ElementList rows;probe->root()->QuerySelectorAll(rows,".setting-row");
            for(auto* r:rows)if(r->GetAttribute<Rml::String>("data-title","")==name)return r;
            throw std::runtime_error(name);};
        auto click=[&](float x,float y,int side=-1){
            if(side>=0){float f=std::clamp((x-20-side*816)/816,0.f,1.f),g=side?1-f:f;float v=g*255;int lo=int(v),hi=std::min(255,lo+1);
                auto a=storybook_art::profile[lo],b=storybook_art::profile[hi];float top=a.top+(b.top-a.top)*(v-lo),bottom=a.bottom+(b.bottom-a.bottom)*(v-lo);
                y=82+top+(y-82)*(bottom-top)/779;const float drop=27*std::pow(g,10.f);y+=drop*(1-(y-82)/779);}
            const auto point=storybook_geometry::fit_book(float(width),float(height)).project({x,y});
            int px=int(point.x),py=int(point.y);
            auto* hit=context->GetElementAtPoint({float(px),float(py)});
            settings_qa_require(hit&&hit->GetTagName()=="storybook-view","visible mesh receives pointer input");
            settings_qa_require(hit->GetComputedValues().focus()!=Rml::Style::Focus::None,"mesh receives click focus");
            context->ProcessMouseMove(px,py,0);context->ProcessMouseButtonDown(0,0);context->ProcessMouseButtonUp(0,0);
        };
        frame("native-picture.ppm");
        settings_qa_require(probe->book()!=nullptr,"native storybook mounted");
        Rml::ElementList tabs;probe->root()->QuerySelectorAll(tabs,"tab");
        settings_qa_require(tabs.size()==kSettingsChapterCount,"platform-specific chapter count");
        auto* first=probe->root()->QuerySelector(".setting-row");
        settings_qa_require(first&&first->GetAttribute<Rml::String>("data-title","")=="Preset","preset is first");
        settings_qa_require(probe->root()->QuerySelector(".current-row")==first,"first row owns cursor");
        width=1280;height=720;frame("native-picture-720p.ppm");
        width=2560;height=1440;dpi=2;frame("native-picture-hidpi.ppm");
        width=1280;height=960;dpi=1;frame("native-picture-4x3.ppm");
        const auto checkFit=[&](){const auto fit=storybook_geometry::fit_book(float(width),float(height));
            const auto corner=fit.project({1672,941});const auto mapped=probe->book()->design_point(corner.x,corner.y);
            settings_qa_require(std::abs(mapped.x-1672)<.01f&&std::abs(mapped.y-941)<.01f,"centered book hit testing matches uniform render projection");
            const auto center=probe->book()->design_point(width/2.f,height/2.f);
            settings_qa_require(std::abs(center.x-836)<.01f&&std::abs(center.y-470.5f)<.01f,"book remains centered across window aspects");};
        checkFit();
        width=1920;height=820;frame("native-picture-ultrawide.ppm");
        checkFit();
        click(555,181,0);frame();settings_qa_require(probe->root()->QuerySelector(".current-row")==first,"ultrawide pointer follows centered book");
        width=1440;height=900;dpi=1;
        probe->set_active_tab(kSettingsTabAudio);probe->focus_content();frame("native-sound.ppm");
        const int standardWidth=width,standardHeight=height;const float standardDpi=dpi;
        // Keep the cache probe on the hidden window's actual aspect ratio. SDL may
        // enforce a platform-specific ratio; changing that policy here leaks into
        // the later portrait/modal navigation checks.
        int cacheWidth=0,cacheHeight=0;
        settings_qa_require(SDL_GetWindowSizeInPixels(static_cast<SDL_Window *>(windowPtr),&cacheWidth,&cacheHeight)
            &&cacheWidth>0&&cacheHeight>0,"storybook cache QA reads the native hidden viewport");
        const int alternateCacheWidth=std::max(1,cacheWidth*3/4);
        const int alternateCacheHeight=std::max(1,cacheHeight*3/4);
        const int originalMasterVolume=cfg().masterVolume,originalSfxVoices=cfg().sfxVoices;
        width=cacheWidth;height=cacheHeight;dpi=1;frame(nullptr,1);
        auto* volume=row("Master Volume");volume->Focus();frame(nullptr,1);
        auto* effectsForCache=row("More Sound Effects");
        const auto steadyGeometry=probe->book()->geometry_build_count();
        const auto steadyText=probe->book()->text_generation_count();
        for(int i=0;i<5;++i)frame(nullptr,1);
        settings_qa_require(probe->book()->geometry_build_count()==steadyGeometry,
            "motion-off storybook geometry builds plateau on stable frames");
        settings_qa_require(probe->book()->text_generation_count()==steadyText,
            "motion-off storybook text generations plateau on stable frames");
        const auto controllerButton=[&](SDL_GamepadButton button){
            SDL_Event event{};event.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            event.gbutton.which=0x7fffffffu;event.gbutton.button=button;
            mp6::ui::handle_event(event);
            event.type=SDL_EVENT_GAMEPAD_BUTTON_UP;mp6::ui::handle_event(event);
        };
        const auto volumeDescription=probe->book()->state().description;
        controllerButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN);frame(nullptr,1);
        settings_qa_require(probe->root()->QuerySelector(".current-row")==effectsForCache,
            "controller D-pad moves semantic focus after idle");
        settings_qa_require(probe->book()->state().selected==1 &&
            probe->book()->state().description!=volumeDescription &&
            probe->book()->geometry_build_count()>steadyGeometry,
            "controller D-pad redraws book cursor and description after idle");
        controllerButton(SDL_GAMEPAD_BUTTON_DPAD_UP);frame(nullptr,1);
        settings_qa_require(probe->book()->state().selected==0 &&
            probe->book()->state().description==volumeDescription,
            "controller D-pad return restores book cursor and description");
        SDL_Event stick{};stick.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;
        stick.gaxis.which=0x7fffffffu;stick.gaxis.axis=SDL_GAMEPAD_AXIS_LEFTY;
        stick.gaxis.value=24000;mp6::ui::handle_event(stick);
        stick.gaxis.value=0;mp6::ui::handle_event(stick);frame(nullptr,1);
        settings_qa_require(probe->book()->state().selected==1,
            "controller stick updates rendered book selection");
        key(Rml::Input::KI_UP);frame(nullptr,1);
        settings_qa_require(probe->book()->state().selected==0 &&
            probe->book()->state().description==volumeDescription,
            "keyboard updates rendered book selection without other invalidation");
        const auto inputGeometry=probe->book()->geometry_build_count();
        const auto inputText=probe->book()->text_generation_count();
        for(int i=0;i<5;++i)frame(nullptr,1);
        settings_qa_require(probe->book()->geometry_build_count()==inputGeometry &&
            probe->book()->text_generation_count()==inputText,
            "controller navigation leaves idle book caches intact");
        const auto selectedGeometry=probe->book()->geometry_build_count();
        const auto selectedValueBefore=volume->GetInnerRML();
        cfg().masterVolume=cfg().masterVolume==100?95:100;
        frame(nullptr,1);
        settings_qa_require(volume->GetInnerRML()!=selectedValueBefore,
            "selected external config value reaches storybook text");
        settings_qa_require(probe->book()->geometry_build_count()>selectedGeometry,
            "selected config change redraws retained storybook geometry");
        const auto nonselectedGeometry=probe->book()->geometry_build_count();
        const auto nonselectedValueBefore=effectsForCache->GetInnerRML();
        cfg().sfxVoices=cfg().sfxVoices==32?16:32;
        frame(nullptr,1);
        settings_qa_require(effectsForCache->GetInnerRML()!=nonselectedValueBefore,
            "nonselected external config value reaches storybook text");
        settings_qa_require(probe->book()->geometry_build_count()>nonselectedGeometry,
            "nonselected config change redraws retained storybook geometry");
        const auto textCacheAtRest=probe->book()->text_cache_count();
        size_t maxTextCache=textCacheAtRest;
        for(int i=0;i<8;++i){
            width=i%2?alternateCacheWidth:cacheWidth;
            height=i%2?alternateCacheHeight:cacheHeight;dpi=i%2?1.f:2.f;
            frame(nullptr,1);maxTextCache=std::max(maxTextCache,probe->book()->text_cache_count());
        }
        width=cacheWidth;height=cacheHeight;dpi=1;frame(nullptr,1);
        settings_qa_require(maxTextCache<=textCacheAtRest+16,
            "storybook text/scene cache stays bounded across repeated resize");
        settings_qa_require(probe->book()->text_cache_count()<=textCacheAtRest+16,
            "storybook text cache remains bounded after returning to the base scale");
        cfg().masterVolume=originalMasterVolume;cfg().sfxVoices=originalSfxVoices;
        width=standardWidth;height=standardHeight;dpi=standardDpi;frame(nullptr,1);
        cfg().masterVolume=100;frame();key(Rml::Input::KI_LEFT);
        settings_qa_require(cfg().masterVolume==95,"native volume keyboard step");
        cfg().masterVolume=0;frame();
        settings_qa_require(!volume->GetAttribute<bool>("data-can-left",true)&&volume->GetAttribute<bool>("data-can-right",false),"volume minimum disables only left arrow");
        key(Rml::Input::KI_LEFT);settings_qa_require(cfg().masterVolume==0,"volume lower bound");
        cfg().masterVolume=100;frame();
        settings_qa_require(volume->GetAttribute<bool>("data-can-left",false)&&!volume->GetAttribute<bool>("data-can-right",true),"volume maximum disables only right arrow");
        const auto soundsBefore=mp6::audio::menu_sounds_played();
        key(Rml::Input::KI_RIGHT);settings_qa_require(cfg().masterVolume==100,"volume upper bound");
        settings_qa_require(mp6::audio::menu_sounds_played()==soundsBefore,"blocked value change is silent");
        frame();click(555,181,0);settings_qa_require(cfg().masterVolume==95,"warped left arrow applies real volume");
        frame();click(756,181,0);settings_qa_require(cfg().masterVolume==100,"warped right arrow applies real volume");
        auto* effects=row("More Sound Effects");effects->Focus();cfg().sfxVoices=32;frame();key(Rml::Input::KI_RETURN);
        settings_qa_require(cfg().sfxVoices==16,"choose toggles real setting");
        volume->Focus();frame();click(350,254,0);settings_qa_require(cfg().sfxVoices==32,"single click focuses and activates row");
        frame("native-sound-selected.ppm");
        SDL_Event pad{};pad.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;pad.gbutton.which=0x7fffffffu;pad.gbutton.button=SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;mp6::ui::handle_event(pad);
        pad.type=SDL_EVENT_GAMEPAD_BUTTON_UP;mp6::ui::handle_event(pad);frame();
        auto* selected=probe->root()->QuerySelector("tab:selected span");
        settings_qa_require(selected&&selected->GetInnerRML()==(kSettingsChapterCount==4?"Game":"Controls"),"controller shoulder changes chapter");
        probe->set_active_tab(kSettingsTabVideo);probe->focus_content();frame();
        click(540,652,0);frame("native-picture-page2.ppm");
        settings_qa_require(probe->root()->QuerySelector(".current-row")->GetAttribute<Rml::String>("data-title","")=="VSync","page navigation focuses second group");
        click(400,652,0);frame();settings_qa_require(probe->root()->QuerySelector(".current-row")==row("Preset"),"page navigation returns to preset");
        row("Ambient Occlusion")->Focus();cfg().ambientOcclusion=0;frame();
        auto* ao=row("Ambient Occlusion");
        settings_qa_require(!ao->GetAttribute<bool>("data-can-left",true),"enum first option disables left arrow");
        key(Rml::Input::KI_LEFT);settings_qa_require(cfg().ambientOcclusion==0,"enum never wraps below first option");
        cfg().ambientOcclusion=2;frame();
        settings_qa_require(!ao->GetAttribute<bool>("data-can-right",true),"enum last option disables right arrow");
        key(Rml::Input::KI_RIGHT);settings_qa_require(cfg().ambientOcclusion==2,"enum never wraps above last option");
        cfg().ambientOcclusion=0;frame();
        const auto choiceBefore=row("Ambient Occlusion")->GetInnerRML();
        key(Rml::Input::KI_RETURN);frame("native-inline-choice.ppm");
        settings_qa_require(!probe->root()->IsClassSet("detail-open")&&probe->visible(),"Confirm never opens a detailed picker");
        settings_qa_require(row("Ambient Occlusion")->GetInnerRML()!=choiceBefore,"Confirm cycles the inline choice");
        key(Rml::Input::KI_LEFT);frame();
        settings_qa_require(row("Ambient Occlusion")->GetInnerRML()==choiceBefore,"left arrow reverses inline choice");
        probe->set_active_tab(kSettingsTabGame);probe->focus_content();frame("native-game.ppm");
        probe->set_active_tab(kSettingsTabAdvanced);probe->focus_content();frame("native-extras.ppm");
        width=900;height=420;frame("native-landscape.ppm");
        width=1440;height=900;cfg().menuMotion=1;frame();
        probe->set_active_tab(kSettingsTabVideo);probe->focus_content();mp6::ui::update();
        // Readback can outlast a 280ms transition on a throttled hidden GPU.
        // Assert its start before waiting for GPU work, not after the capture.
        settings_qa_require(probe->book()->turning(),"native page turn begins");
        frame("native-turn-start.ppm",1);
        SDL_Delay(65);frame("native-turn-middle.ppm",1);SDL_Delay(285);frame("native-turn-landed.ppm");
        settings_qa_require(!probe->book()->turning(),"native page turn settles");
        probe->set_active_tab(kSettingsTabAudio);probe->focus_content();frame();
        width=1280;height=960;frame("native-turn-resize.ppm");SDL_Delay(285);frame("native-turn-forward-landed.ppm");
        settings_qa_require(!probe->book()->turning(),"resizing in flight settles within 300ms");
        probe->set_active_tab(kSettingsTabVideo);probe->focus_content();frame();
        SDL_Delay(285);frame();
        for(int i=0;i<5;++i){controllerButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN);mp6::ui::update();}
        settings_qa_require(probe->book()->state().selected==5,
            "controller navigation updates animated book selection");
        controllerButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN);mp6::ui::update();
        settings_qa_require(probe->book()->state().selected==6&&probe->book()->turning(),
            "controller navigation across row group starts a page turn");
        SDL_Delay(285);frame();
        settings_qa_require(!probe->book()->turning()&&probe->book()->state().selected==6,
            "controller page turn lands on the selected row");
        cfg().menuMotion=0;frame();probe->set_active_tab(kSettingsTabAudio);probe->focus_content();frame();
        settings_qa_require(!probe->book()->turning(),"animation opt-out is immediate");
        click(548,905);frame(); // Footer Choose: volume has no destructive action.
        key(Rml::Input::KI_DOWN);frame();settings_qa_require(probe->root()->QuerySelector(".current-row")==row("More Sound Effects"),"keyboard continues after footer click");
        click(610,770,0);key(Rml::Input::KI_UP);frame();settings_qa_require(probe->root()->QuerySelector(".current-row")==row("Master Volume"),"blank paper preserves keyboard focus");
        probe->hide(false);settings_qa_require(!probe->document()->IsVisible(),"close releases input");probe->show();frame();settings_qa_require(probe->visible(),"rapid reopen");
        cfg().menuMotion=1;const auto exitStart=SDL_GetTicksNS();probe->hide(false);
        settings_qa_require(!probe->visible()&&probe->document()->IsVisible(),"outro releases input but retains presentation");
        const int beforeClose=cfg().masterVolume;key(Rml::Input::KI_LEFT);
        settings_qa_require(cfg().masterVolume==beforeClose,"outro blocks settings changes");
        // Capture against the transition start, not accumulating GPU readback
        // delays between landmarks. These include each spring's overshoot.
        const auto frameAt=[&](Uint64 start,double at,const char* capture){
            const auto elapsed=milliseconds(start);
            if(elapsed<at)SDL_Delay(Uint32(at-elapsed));
            frame(capture,1);
        };
        frameAt(exitStart,120,"native-outro-spring.ppm");
        frameAt(exitStart,200,"native-outro.ppm");
        frameAt(exitStart,540,"native-outro-background.ppm");frameAt(exitStart,820,nullptr);
        settings_qa_require(!probe->document()->IsVisible(),"staggered decorative outro finishes");
        const auto introStart=SDL_GetTicksNS();probe->show();
        frameAt(introStart,24,"native-intro-first-frame.ppm");frameAt(introStart,164,"native-intro-sky.ppm");
        frameAt(introStart,394,"native-intro-book.ppm");frameAt(introStart,650,"native-intro-interactive.ppm");
        settings_qa_require(!probe->book()->entering(),"book interactive within doubled 600ms intro");
        frameAt(introStart,742,"native-intro-brighton-spring.ppm");frameAt(introStart,962,"native-intro-twila-spring.ppm");
        frameAt(introStart,1120,"native-intro-hosts.ppm");frameAt(introStart,1400,"native-intro-landed.ppm");
        probe->hide(false);probe->show();frame();SDL_Delay(245);frame();
        settings_qa_require(probe->visible()&&probe->document()->IsVisible(),"reopen cancels pending outro");
        for(int i=0;i<6;++i){
            SDL_Delay(35);const double before=probe->book()->presentation_progress();probe->hide(false);
            settings_qa_require(std::abs(before-probe->book()->presentation_progress())<.015,"close reverses from current book pose");
            frame(nullptr,1);SDL_Delay(35);const double closing=probe->book()->presentation_progress();probe->show();
            settings_qa_require(std::abs(closing-probe->book()->presentation_progress())<.015,"F10 reopen preserves partially closed pose");
            frame(i==5?"native-reversed-open.ppm":nullptr,1);
        }
        cfg().menuMotion=0;frame();
        probe->hide(false);auto game=std::make_unique<SettingsProbe>(true);probe=game.get();push_document(std::move(game));probe->set_active_tab(kSettingsTabGame);frame("native-save-slots.ppm");
        Rml::ElementList slots;probe->root()->QuerySelectorAll(slots,"pane.settings-list button");int numbered=0;std::set<Rml::String> names;
        for(auto* slot:slots){auto name=slot->GetAttribute<Rml::String>("data-title","");settings_qa_require(names.insert(name).second,"save actions remain unambiguous");if(name.find("Slot ")!=Rml::String::npos)++numbered;}
        settings_qa_require(numbered==10,"five save/load pairs preserved without execution");
        probe->hide(false);
        const std::string linkedRoot=cfg().contentRoot;
        snprintf(cfg().contentRoot,sizeof(cfg().contentRoot),"missing-first-launch-fixture");
        auto launcher=std::make_unique<PrelaunchProbe>();auto* home=launcher.get();push_document(std::move(launcher));
        width=1440;height=900;dpi=1;frame();SDL_Delay(240);frame("launcher-link-dialog.ppm");
        const auto linkDialog=[&](){for(int i=0;i<context->GetNumDocuments();++i){auto* doc=context->GetDocument(i);
            if(doc->IsVisible())if(auto* dialog=doc->QuerySelector("window.content-link[open]"))return dialog;}return static_cast<Rml::Element*>(nullptr);};
        auto* linking=linkDialog();
        settings_qa_require(linking&&!home->visible()&&dynamic_cast<ContentSetup*>(top_document()),"first launch automatically presents disc linking");
        Rml::ElementList choices;linking->QuerySelectorAll(choices,"button");
        settings_qa_require(choices.size()==3&&choices[0]->GetInnerRML().find("Disc Image")!=Rml::String::npos&&
            choices[1]->GetInnerRML().find("Folder")!=Rml::String::npos,"linking offers image folder and dismissal without opening a system picker");
        width=900;height=420;frame("launcher-link-dialog-small.ppm");
        const auto checkDialog=[&](){auto pos=linking->GetAbsoluteOffset();settings_qa_require(pos.x>=0&&pos.y>=0&&
            pos.x+linking->GetOffsetWidth()<=width+1&&pos.y+linking->GetOffsetHeight()<=height+1,"entire first-launch dialog fits the window");};
        checkDialog();
        width=390;height=720;frame("launcher-link-dialog-portrait.ppm");
        checkDialog();
        for(auto* button:choices){auto pos=button->GetAbsoluteOffset();settings_qa_require(pos.x>=0&&pos.y>=0&&
            pos.x+button->GetOffsetWidth()<=width+1&&pos.y+button->GetOffsetHeight()<=height+1,"first-launch actions fit the window");}
        choices[2]->Focus();key(Rml::Input::KI_RETURN);SDL_Delay(240);frame();
        settings_qa_require(home->visible()&&!linkDialog()&&!content_ready(),"dismissal returns to unlinked launcher without reopening dialog");
        width=1440;height=900;frame("launcher-no-disc.ppm");
        Rml::ElementList actions;home->document()->QuerySelectorAll(actions,"#menu-list button");
        settings_qa_require(actions.size()==3&&actions[0]->GetInnerRML().find("Select Game")!=Rml::String::npos,"unlinked launcher retains Select Game");
        click(360,340,0);frame();SDL_Delay(240);frame();
        settings_qa_require(linkDialog()!=nullptr,"Select Game reopens linking dialog");
        key(Rml::Input::KI_ESCAPE);SDL_Delay(240);frame();
        snprintf(cfg().contentRoot,sizeof(cfg().contentRoot),"%s",linkedRoot.c_str());home->show();frame("launcher-linked.ppm");
        settings_qa_require(content_ready()&&!linkDialog()&&actions[0]->GetInnerRML().find("Play")!=Rml::String::npos,"linking refreshes launcher to Play without restart");
        key(Rml::Input::KI_DOWN);frame("launcher-settings-selected.ppm");
        settings_qa_require(context->GetFocusElement()==actions[1],"launcher keyboard moves the single selection");
        cfg().menuMotion=1;click(360,452,0);frame("launcher-settings-no-intro.ppm",1);
        settings_qa_require(!home->visible()&&dynamic_cast<SettingsWindow*>(top_document()),"launcher pointer opens real Settings");
        const auto activeBook=[&](){return dynamic_cast<StorybookView*>(context->GetFocusElement()->GetOwnerDocument()->QuerySelector("storybook-view"));};
        settings_qa_require(activeBook()&&!activeBook()->entering(),"launcher Settings does not replay scenery or book intro with motion enabled");
        settings_qa_require(activeBook()->turning()&&activeBook()->handing_off(),"launcher Settings flips from the actual opening spread");
        auto* handoff=activeBook();const auto handoffStart=SDL_GetTicksNS();
        const int pictureBefore=cfg().widescreen;
        key(Rml::Input::KI_E);key(Rml::Input::KI_RIGHT);frame(nullptr,1);
        settings_qa_require(activeBook()==handoff&&handoff->handing_off()&&cfg().widescreen==pictureBefore,"handoff blocks accidental changes while its page is turning");
        frameAt(handoffStart,55,"launcher-settings-flip-early.ppm");
        frameAt(handoffStart,120,"launcher-settings-flip-middle.ppm");
        frameAt(handoffStart,215,"launcher-settings-flip-late.ppm");
        frameAt(handoffStart,300,"launcher-settings-flip-settled.ppm");
        settings_qa_require(!handoff->turning()&&!handoff->handing_off(),"launcher Settings page flip completes within 300ms");
        frameAt(handoffStart,900,"launcher-settings-chapters-settled.ppm");
        for(int i=0;i<kSettingsChapterCount;++i)settings_qa_require(handoff->chapter_offset(i)==0,"staggered chapter springs settle after the quick page turn");
        key(Rml::Input::KI_ESCAPE);frame("launcher-settings-return.ppm",1);
        settings_qa_require(home->visible(),"closing launcher Settings returns without a book outro");
        actions[1]->Focus();key(Rml::Input::KI_RETURN);frame(nullptr,1);
        settings_qa_require(dynamic_cast<SettingsWindow*>(top_document())&&activeBook()&&!activeBook()->entering(),"keyboard reopening Settings also preserves the open book");
        settings_qa_require(activeBook()->turning(),"keyboard opening also turns the page");
        key(Rml::Input::KI_ESCAPE);frame();settings_qa_require(home->visible(),"Back can cancel a handoff immediately");cfg().menuMotion=0;
        actions[1]->Focus();key(Rml::Input::KI_RETURN);frame();
        settings_qa_require(activeBook()&&!activeBook()->turning()&&activeBook()->chapter_offset(0)==0,"motion disabled skips the page flip and chapter springs");
        key(Rml::Input::KI_ESCAPE);frame();
        width=1280;height=960;frame("launcher-4x3.ppm");
        width=1920;height=820;frame("launcher-ultrawide.ppm");
        width=2560;height=1440;dpi=2;frame("launcher-hidpi.ppm");
        width=900;height=420;dpi=1;frame("launcher-landscape.ppm");
        // Shared surfaces must not fall back to the former blue/cyan UI.
        // Pump RCSS transitions at a real cadence (RmlUi caps large time steps).
        auto settle=[&](){for(int i=0;i<16;++i){SDL_Delay(20);frame(nullptr,1);}};
        auto visibleElement=[&](const char* selector)->Rml::Element*{
            for(int i=0;i<context->GetNumDocuments();++i){auto* d=context->GetDocument(i);
                if(d->IsVisible())if(auto* e=d->QuerySelector(selector))return e;}
            return nullptr;
        };
        auto paperCheck=[&](const char* selector){auto* e=visibleElement(selector);
            settings_qa_require(e&&e->GetOffsetWidth()>0&&e->GetOffsetHeight()>0,"auxiliary UI surface is visible");
            settings_qa_require(e->GetProperty<Rml::Colourb>("background-color")==Rml::Colourb(255,249,231),
                "auxiliary surfaces use opaque storybook paper");
            auto p=e->GetAbsoluteOffset(Rml::BoxArea::Border);
            printf("[SETTINGS-QA] surface %s rect %.1f %.1f %.1f %.1f viewport %d %d\n",selector,p.x,p.y,e->GetOffsetWidth(),e->GetOffsetHeight(),width,height);fflush(stdout);
            settings_qa_require(p.x>=-1&&p.y>=-1&&p.x+e->GetOffsetWidth()<=width+1&&p.y+e->GetOffsetHeight()<=height+1,
                "auxiliary panel stays inside viewport");return e;
        };
        const int oldFps=cfg().showFps,oldCorner=cfg().fpsCorner;
        cfg().showFps=1;cfg().fpsCorner=1;width=1440;height=900;
        frame("ui-fps-badge.ppm");paperCheck("fps");
        cfg().showFps=oldFps;cfg().fpsCorner=oldCorner;
        width=1440;height=900;settle();frame("ui-controller-notice.ppm");
        paperCheck("toast.controller-warning");
        push_toast({"achievement","Party milestone","A new entry for your storybook.",std::chrono::seconds(5)});
        settle();frame("ui-achievement-notice.ppm");paperCheck("toast.achievement");
        get_toasts().clear();settle();
        push_document(std::make_unique<Modal>(Modal::Props{
            .title="Return to launcher?",.bodyRml="Your settings are saved automatically.<br/><span class='tip'>You can return to the party later.</span>",
            .actions={{"Keep playing",[](Modal& m){m.pop();}},{"Return",[](Modal& m){m.pop();}}},.icon="question-mark"}));
        settle();frame("ui-confirm-dialog.ppm");paperCheck("window.modal");
        width=390;height=720;frame("ui-confirm-dialog-portrait.ppm");paperCheck("window.modal");
        const auto logFocus=[&](const char* phase){auto* focused=context->GetFocusElement();
            printf("[SETTINGS-QA] modal %s focus tag=%s id=%s text=%s\n",phase,
                focused?focused->GetTagName().c_str():"<none>",focused?focused->GetId().c_str():"",
                focused?focused->GetInnerRML().c_str():"");fflush(stdout);};
        logFocus("before-right");key(Rml::Input::KI_RIGHT);frame();logFocus("after-right");
        auto* focusedAction=context->GetFocusElement();
        settings_qa_require(focusedAction&&focusedAction->GetInnerRML().find("Return")!=Rml::String::npos,
            "paper dialog preserves keyboard action navigation");
        key(Rml::Input::KI_ESCAPE);settle();frame("ui-confirm-dismissed.ppm");
        settings_qa_require(!visibleElement("window.modal[open]")&&!dynamic_cast<Modal*>(top_document()),
            "paper dialog preserves Cancel and releases input");
        width=1440;height=900;frame();
        // The inherited tuner remains unrouted, but its retained surface must
        // also be legible when used by tooling; these actions change no prefs.
        push_document(std::make_unique<GraphicsTuner>(GraphicsTunerProps{
            .option=GraphicsOption::ShadowResolution,.title="Shadow Quality",
            .helpText="Choose the detail level of shadows.",.valueMin=0,.valueMax=4,.defaultValue=0},true));
        settle();frame("ui-graphics-panel.ppm");paperCheck(".tuner-root");
        key(Rml::Input::KI_ESCAPE);settle();
        home->show();frame();
        auto& bar=push_document(std::make_unique<MenuBar>());settle();
        frame("ui-menu-bar.ppm");paperCheck("popup");
        bar.pop();settle();
        mp6_console_set_available(1); // Only this isolated UI harness.
        auto& console=push_document(std::make_unique<Console>());
        mp6_console_log("Storybook UI verification: console text remains readable.");
        mp6_console_toggle_log();frame("ui-console.ppm");paperCheck("cmdbar");paperCheck("output");
        mp6_console_set_overlay(MP6_CONSOLE_PANEL_UNIT,1);
        mp6_console_set_overlay(MP6_CONSOLE_PANEL_GAME,1);
        frame("ui-stat-panels.ppm");paperCheck("statunit");paperCheck("statpanel");
        mp6_console_clear_overlays();console.pop();mp6_console_set_available(0);frame();home->show();frame();
        width=1440;height=900;cfg().menuMotion=1;actions[0]->Focus();frame();key(Rml::Input::KI_RETURN);
        settings_qa_require(!play_requested()&&home->pending_close(),"Play waits for the book outro");
        frame("launcher-outro.ppm",1);SDL_Delay(830);frame();
        settings_qa_require(play_requested(),"launcher Play hands off after outro");
        mp6::ui::s_playRequested=false;
        printf("[SETTINGS-QA] COMPLETE\n");fflush(stdout);return 1;
    }catch(const std::exception& e){fprintf(stderr,"[SETTINGS-QA] FAIL %s\n",e.what());fflush(stderr);std::exit(9);}
}
