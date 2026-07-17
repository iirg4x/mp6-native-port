/* MP6 native port -- first-run content onboarding dialog implementation.
 * See content_setup.hpp for the design story. OUR file; structural idiom
 * follows the ripped modal.cpp / partyboard's DiscVerificationModal (their
 * src/port/ui/prelaunch.cpp) so the ripped window.rcss modal + progress
 * styles apply unchanged. */
#include "content_setup.hpp"

#include "launcher_state.hpp"

#include "lib/window.hpp" /* aurora internal (get_sdl_window), via -I AURORA_ROOT */
#include <SDL3/SDL.h>
#include <fmt/format.h>

#include <algorithm>
#include <mutex>

#ifdef __ANDROID__
extern "C" {
#include "../../android/saf_bridge.h"
/* SDL3 SDL_system.h storage getters, declared by hand exactly like
 * platform/main_native.c does (keeps the include set unchanged). */
const char *SDL_GetAndroidExternalStoragePath(void);
const char *SDL_GetAndroidInternalStoragePath(void);
}
#endif

namespace mp6::ui {
namespace {

    /* SDL dialog callbacks may fire on any thread -- store-only here, the
     * document's update() consumes on the UI thread. One ContentSetup at a
     * time by construction (single modal). */
    std::mutex sPickMutex;
    std::string sPickedPath;
    int sPickEvent = 0; /* 0 none, 1 picked, 2 cancelled, 3 error */

    void dialog_callback(void *userdata, const char *const *filelist, int filter)
    {
        (void)userdata;
        (void)filter;
        std::lock_guard<std::mutex> lock(sPickMutex);
        if (filelist == nullptr) {
            sPickEvent = 3;
            sPickedPath = SDL_GetError();
        } else if (filelist[0] == nullptr) {
            sPickEvent = 2;
        } else {
            sPickEvent = 1;
            sPickedPath = filelist[0];
        }
    }

    int take_pick_event(std::string &pathOut)
    {
        std::lock_guard<std::mutex> lock(sPickMutex);
        int ev = sPickEvent;
        sPickEvent = 0;
        pathOut = sPickedPath;
        sPickedPath.clear();
        return ev;
    }

    Rml::String format_bytes(uint64_t n)
    {
        if (n >= 1024ull * 1024ull * 1024ull) {
            return fmt::format("{:.2f} GB", (double)n / (1024.0 * 1024.0 * 1024.0));
        }
        if (n >= 1024ull * 1024ull) {
            return fmt::format("{:.1f} MB", (double)n / (1024.0 * 1024.0));
        }
        return fmt::format("{} KB", n / 1024);
    }

    const char *kChooseMessage =
        "Mario Party 6 game content was not found.<br/><br/>"
        "The port needs the game files from your own <b>Mario Party 6 (USA)</b> disc. "
        "Select a disc image (<b>.iso</b>, <b>.gcm</b>, <b>.rvz</b> and other formats) "
        "or an already-extracted <b>GP6E01</b> folder \xE2\x80\x94 the needed files "
        "(about 400 MB) are imported once and kept on this device.";

} // namespace

ContentSetup::ContentSetup()
    : WindowSmall("modal", "modal-dialog")
{
    auto *header = append(mDialog, "div");
    header->SetClass("modal-header", true);

    mTitle = append(header, "div");
    mTitle->SetClass("modal-title", true);
    mTitle->SetInnerRML("Game Content Setup");

    auto *body = append(mDialog, "div");
    body->SetClass("modal-body", true);

    mMessage = append(body, "div");
    mMessage->SetInnerRML(kChooseMessage);

    mProgressWrap = append(body, "div");
    mProgressWrap->SetClass("verification-progress", true);
    mProgressFile = append(mProgressWrap, "div");
    mProgressFile->SetClass("verification-file", true);
    mProgressBar = append(mProgressWrap, "progress");
    mProgressBar->SetClass("progress-ongoing", true);
    mProgressBar->SetClass("verification-progress-bar", true);
    mProgressBar->SetAttribute("value", 0.f);
    mProgressDetail = append(mProgressWrap, "div");
    mProgressDetail->SetClass("verification-detail", true);
    mProgressWrap->SetProperty(Rml::PropertyId::Display, Rml::Style::Display::None);

    mActions = append(mDialog, "div");
    mActions->SetClass("modal-actions", true);

    set_stage(Stage::Choose);
}

ContentSetup::~ContentSetup()
{
    /* A live native import outlives no one: request cancel and reap. The
     * Java-side tree import (android) is fire-and-forget by design -- it
     * has its own cancel path and cannot dangle into game state. */
    Mp6ImportStatus st {};
    mp6_import_poll(&st);
    if (st.state == MP6_IMPORT_RUNNING) {
        mp6_import_cancel();
        /* mp6_import_reset() joins; the worker exits at the next chunk. */
    }
    if (st.state != MP6_IMPORT_IDLE) {
        mp6_import_reset();
    }
}

void ContentSetup::set_stage(Stage stage)
{
    mStage = stage;
    const bool importing = (stage == Stage::Importing);
    mMessage->SetProperty(Rml::PropertyId::Display,
                          importing ? Rml::Style::Display::None : Rml::Style::Display::Block);
    mProgressWrap->SetProperty(Rml::PropertyId::Display,
                               importing ? Rml::Style::Display::Block : Rml::Style::Display::None);
    switch (stage) {
        case Stage::Choose:
            mTitle->SetInnerRML("Game Content Setup");
            mMessage->SetInnerRML(kChooseMessage);
            break;
        case Stage::PickingImage:
        case Stage::PickingFolder:
            mMessage->SetInnerRML("Waiting for the system file picker...");
            break;
        case Stage::Importing:
            mTitle->SetInnerRML("Importing Game Content");
            break;
        case Stage::Error:
            mTitle->SetInnerRML("Import Failed");
            break;
    }
    rebuild_actions();
    focus();
}

void ContentSetup::rebuild_actions()
{
    /* Destroy the Button components BEFORE their elements (dtor order
     * safety), then rebuild the action row for the stage. */
    mButtons.clear();
    mActions->SetInnerRML("");

    auto add = [this](const Rml::String &label, ButtonCallback cb) -> Button & {
        auto btn = std::make_unique<Button>(mActions, label);
        btn->root()->SetClass("modal-btn", true);
        btn->on_pressed(std::move(cb));
        mButtons.push_back(std::move(btn));
        return *mButtons.back();
    };

    switch (mStage) {
        case Stage::Choose:
            add("Select Disc Image", [this] { begin_image_pick(); });
            add("Select Folder", [this] { begin_folder_pick(); });
            add("Cancel", [this] { pop(); });
            break;
        case Stage::PickingImage:
        case Stage::PickingFolder:
            add("Back", [this] { set_stage(Stage::Choose); }).set_disabled(false);
            break;
        case Stage::Importing:
            add("Cancel", [this] {
                if (!mCancelRequested) {
                    mCancelRequested = true;
#ifdef __ANDROID__
                    if (mUsingSafTree) {
                        mp6_saf_tree_import_cancel();
                    } else
#endif
                        mp6_import_cancel();
                    if (!mButtons.empty()) {
                        mButtons.front()->set_text("Cancelling...");
                        mButtons.front()->set_disabled(true);
                    }
                }
            });
            break;
        case Stage::Error:
            add("Back", [this] { set_stage(Stage::Choose); });
            add("Close", [this] { pop(); });
            break;
    }
}

std::string ContentSetup::dest_disc_root()
{
#ifdef __ANDROID__
    /* The exact base mp6_android_main probes (external preferred, internal
     * fallback), so the imported tree is auto-found on every later boot
     * with zero configuration. */
    const char *ext = SDL_GetAndroidExternalStoragePath();
    const char *inl = SDL_GetAndroidInternalStoragePath();
    const char *base = (ext != nullptr && ext[0] != '\0') ? ext : inl;
    return std::string(base != nullptr ? base : "") + "/mp6/GP6E01";
#else
    /* Windows: an already-configured content root is imported INTO (it is
     * the user's chosen location); otherwise content/GP6E01 next to the
     * exe (the saves/ convention), and the config is pointed at it on
     * success (finish_success). */
    if (cfg().contentRoot[0] != '\0') {
        return cfg().contentRoot;
    }
    const char *base = SDL_GetBasePath();
    return std::string(base != nullptr ? base : "") + "content/GP6E01";
#endif
}

void ContentSetup::begin_image_pick()
{
    static const SDL_DialogFileFilter kFilters[] = {
        { "GameCube disc image", "iso;gcm;rvz;ciso;gcz;wia" },
        { "All files", "*" },
    };
    set_stage(Stage::PickingImage);
    SDL_ShowOpenFileDialog(&dialog_callback, nullptr, aurora::window::get_sdl_window(), kFilters,
                           (int)(sizeof(kFilters) / sizeof(kFilters[0])), nullptr, false);
}

void ContentSetup::begin_folder_pick()
{
#ifdef __ANDROID__
    /* SDL has no folder dialog on Android; Mp6Activity's SAF tree flow
     * (ACTION_OPEN_DOCUMENT_TREE) takes over via the JNI bridge. */
    set_stage(Stage::PickingFolder);
    if (mp6_saf_open_tree_picker() != 0) {
        show_error("Could not open the system folder picker.");
    }
#else
    set_stage(Stage::PickingFolder);
    SDL_ShowOpenFolderDialog(&dialog_callback, nullptr, aurora::window::get_sdl_window(), nullptr, false);
#endif
}

void ContentSetup::begin_import_image(const std::string &source)
{
    mCancelRequested = false;
    mUsingSafTree = false;
    if (mp6_import_start_image(source.c_str(), dest_disc_root().c_str()) != 0) {
        show_error("An import is already running.");
        return;
    }
    set_stage(Stage::Importing);
}

void ContentSetup::begin_import_folder(const std::string &source)
{
    mCancelRequested = false;
    mUsingSafTree = false;
    if (mp6_import_start_folder(source.c_str(), dest_disc_root().c_str()) != 0) {
        show_error("An import is already running.");
        return;
    }
    set_stage(Stage::Importing);
}

void ContentSetup::show_error(const std::string &message)
{
    set_stage(Stage::Error);
    mMessage->SetInnerRML(escape(message));
}

void ContentSetup::finish_success()
{
#ifndef __ANDROID__
    if (cfg().contentRoot[0] == '\0') {
        snprintf(cfg().contentRoot, sizeof(cfg().contentRoot), "%s", dest_disc_root().c_str());
        cfg_save();
    }
#endif
    refresh_content_state();
    printf("[CONTENT] import complete -- content root ready\n");
    fflush(stdout);
    push_toast({
        .type = "content",
        .title = "Content imported",
        .content = "Mario Party 6 game content is ready.",
        .duration = std::chrono::seconds(4),
    });
    pop();
}

void ContentSetup::poll_import()
{
    Mp6ImportStatus st {};
#ifdef __ANDROID__
    if (mUsingSafTree) {
        mp6_saf_tree_import_poll(&st);
    } else
#endif
        mp6_import_poll(&st);

    if (st.state == MP6_IMPORT_RUNNING) {
        if (mProgressFile != nullptr) {
            mProgressFile->SetInnerRML(escape(st.currentFile[0] ? st.currentFile : "Reading source..."));
        }
        if (st.bytesTotal > 0) {
            float fraction =
                std::clamp((float)((double)st.bytesDone / (double)st.bytesTotal), 0.0f, 1.0f);
            if (mProgressBar != nullptr) mProgressBar->SetAttribute("value", fraction);
            if (mProgressDetail != nullptr) {
                mProgressDetail->SetInnerRML(escape(
                    fmt::format("{} / {} ({:.0f}%)  \xE2\x80\xA2  file {} of {}", format_bytes(st.bytesDone),
                                format_bytes(st.bytesTotal), fraction * 100.0, st.filesDone, st.filesTotal)));
            }
        } else if (mProgressDetail != nullptr) {
            mProgressDetail->SetInnerRML("Opening source...");
        }
        return;
    }

    if (st.state == MP6_IMPORT_DONE) {
#ifdef __ANDROID__
        if (!mUsingSafTree)
#endif
            mp6_import_reset();
        finish_success();
        return;
    }
    if (st.state == MP6_IMPORT_FAILED) {
        std::string err = st.error[0] ? st.error : "unknown error";
#ifdef __ANDROID__
        if (!mUsingSafTree)
#endif
            mp6_import_reset();
        show_error("Import failed: " + err);
        return;
    }
    if (st.state == MP6_IMPORT_CANCELLED) {
#ifdef __ANDROID__
        if (!mUsingSafTree)
#endif
            mp6_import_reset();
        set_stage(Stage::Choose);
        return;
    }
}

void ContentSetup::update()
{
    switch (mStage) {
        case Stage::PickingImage: {
            std::string path;
            int ev = take_pick_event(path);
            if (ev == 1) {
                begin_import_image(path);
            } else if (ev == 2) {
                set_stage(Stage::Choose);
            } else if (ev == 3) {
                show_error(path.empty() ? "The system file dialog failed." : ("File dialog failed: " + path));
            }
            break;
        }
        case Stage::PickingFolder: {
#ifdef __ANDROID__
            char uri[1024];
            int r = mp6_saf_poll_tree_pick(uri, sizeof(uri));
            if (r == 1) {
                mCancelRequested = false;
                mUsingSafTree = true;
                if (mp6_saf_tree_import_start(uri, dest_disc_root().c_str()) != 0) {
                    show_error("Could not start the folder import.");
                } else {
                    set_stage(Stage::Importing);
                }
            } else if (r < 0) {
                set_stage(Stage::Choose);
            }
#else
            std::string path;
            int ev = take_pick_event(path);
            if (ev == 1) {
                begin_import_folder(path);
            } else if (ev == 2) {
                set_stage(Stage::Choose);
            } else if (ev == 3) {
                show_error(path.empty() ? "The system folder dialog failed." : ("Folder dialog failed: " + path));
            }
#endif
            break;
        }
        case Stage::Importing:
            poll_import();
            break;
        case Stage::Choose:
        case Stage::Error:
            break;
    }
    Document::update();
}

bool ContentSetup::focus()
{
    if (mButtons.empty()) {
        return false;
    }
    return mButtons.front()->focus();
}

bool ContentSetup::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        if (mStage == Stage::Choose || mStage == Stage::Error) {
            pop();
        }
        /* Importing/picking: swallow ESC (Cancel button is the explicit
         * path; the system dialog owns its own dismissal). */
        event.StopPropagation();
        return true;
    }

    int direction = 0;
    if (cmd == NavCommand::Left) {
        direction = -1;
    } else if (cmd == NavCommand::Right) {
        direction = 1;
    } else {
        return false;
    }

    auto *target = event.GetTargetElement();
    for (int i = 0; i < (int)mButtons.size(); ++i) {
        if (mButtons[i]->contains(target)) {
            const int next = i + direction;
            if (next >= 0 && next < (int)mButtons.size() && mButtons[next]->focus()) {
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace mp6::ui
