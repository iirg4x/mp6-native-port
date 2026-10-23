// Isolated, muted native UI throughput; no real user config or game actions.
int mp6_settings_qa_run(void *windowPtr) {
    using namespace mp6::ui;
    const auto ms=[](Uint64 start){return double(SDL_GetTicksNS()-start)/1e6;};
    struct Counters {
        size_t geometryBuilds=0,geometryReuses=0,textGenerations=0,textCache=0,renderCalls=0;
    };
    const auto counters=[&](SettingsProbe* menu){
        const auto* book=menu->book();
        return Counters{book->geometry_build_count(),book->geometry_reuse_count(),
            book->text_generation_count(),book->text_cache_count(),book->render_call_count()};
    };
    cfg().menuMotion=1;cfg().vsync=0;
    auto menu=std::make_unique<SettingsProbe>();auto* probe=menu.get();push_document(std::move(menu));
    auto* context=probe->document()->GetContext();
    auto* renderer=static_cast<aurora::rmlui::WebGPURenderInterface*>(Rml::GetRenderInterface());
    for(const auto size:std::array<Rml::Vector2i,2>{{{1280,720},{2560,1440}}}) {
        SDL_SetWindowSize(static_cast<SDL_Window*>(windowPtr),size.x,size.y);
        aurora::rmlui::set_ui_scale(1.f);
        auto target=aurora::webgpu::create_render_texture(size.x,size.y,false);
        // Keep the original animated/turning cases intact for comparable
        // render_ms baselines, then run a longer no-motion case to prove that
        // retained geometry/text are reused rather than rebuilt every frame.
        const std::array<const char*,3> modes{{"animated","turning","static"}};
        for(const auto mode:modes) {
            const bool staticCase=mode[0]=='s';
            const bool turningCase=mode[0]=='t';
            cfg().menuMotion=staticCase?0:1;
            probe->set_active_tab(kSettingsTabAudio);probe->focus_content();
            if(!staticCase)SDL_Delay(1450); // Let the entrance settle before measuring steady animation.
            double update=0,render=0,total=0;size_t batches=0,maxBatches=0;
            constexpr int warm=40,frames=360,staticFrames=720;
            const int measuredFrames=staticCase?staticFrames:frames;
            Counters afterWarm{};
            for(int i=0;i<warm+measuredFrames;++i) {
                // The static case is deliberately bounded but substantially
                // longer than the comparable samples, providing a bounded
                // retained-resource soak without an unbounded launcher run.
                if(staticCase&&i==warm)afterWarm=counters(probe);
                const auto start=SDL_GetTicksNS();aurora_update();
                if(!aurora_begin_frame())return 9;
                context->SetDimensions(size);context->SetDensityIndependentPixelRatio(1.f);
                if(turningCase&&i%24==0){probe->set_active_tab((i/24)%kSettingsChapterCount);probe->focus_content();}
                auto begin=SDL_GetTicksNS();mp6::ui::update();context->Update();const double u=ms(begin);
                renderer->SetWindowSize(size);
                renderer->BeginFrame(target,aurora::webgpu::present_source(),aurora::rmlui::BaseLayerContent::Transparent);
                begin=SDL_GetTicksNS();context->Render();const double r=ms(begin);
                const size_t draws=probe->book()->render_batch_count();
                maxBatches=std::max(maxBatches,draws);
                renderer->EndFrame();
                // This closes the real Aurora frame and records another Rml
                // frame for presentation. Keep it for lifecycle correctness,
                // but frame_ms is explicitly a double-Rml-render metric;
                // render_ms above remains the single explicit offscreen pass.
                aurora_end_frame();
                if(i>=warm&&i<warm+measuredFrames){update+=u;render+=r;total+=ms(start);batches+=draws;}
            }
            const auto after=counters(probe);
            if(staticCase){
                settings_qa_require(after.geometryBuilds==afterWarm.geometryBuilds,
                    "static storybook geometry builds plateau after warmup");
                settings_qa_require(after.textGenerations==afterWarm.textGenerations,
                    "static storybook text generations plateau after warmup");
                settings_qa_require(after.geometryReuses>afterWarm.geometryReuses,
                    "static storybook retains and reuses compiled geometry");
                settings_qa_require(after.textCache==afterWarm.textCache,
                    "static storybook text cache cardinality plateaus");
            }
            printf("[STORYBOOK-PERF] %dx%d %s update_ms=%.4f render_ms=%.4f frame_ms=%.4f batches=%.1f "
                   "render_scope=explicit_offscreen frame_scope=explicit+aurora_end_frame rml_renders=2 "
                   "geometry_builds=%zu geometry_reuses=%zu text_generations=%zu text_cache=%zu render_calls=%zu\n",
                size.x,size.y,mode,update/measuredFrames,render/measuredFrames,total/measuredFrames,
                double(batches)/measuredFrames,after.geometryBuilds,after.geometryReuses,
                after.textGenerations,after.textCache,after.renderCalls);
            fflush(stdout);
            settings_qa_require(maxBatches>0&&maxBatches<100,"storybook stays below 100 ordered draws");
        }
        settings_qa_capture(target,size.x==1280?"perf-720p.ppm":"perf-1440p.ppm");
    }
    return 1;
}
