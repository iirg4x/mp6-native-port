/* MP6 native port -- first-run content onboarding dialog.
 *
 * OUR file (not ripped): a WindowSmall document in the ripped partyboard
 * framework's own idiom (its skeleton/classes mirror modal.cpp and their
 * DiscVerificationModal shape -- docs/PARTYBOARD_PROVENANCE.md notes the
 * pattern reference), driving platform/content/content_import.h:
 *
 *   choose  -> "Select Disc Image" (SDL3 file dialog; on Android the SDL
 *              dialog IS the SAF ACTION_OPEN_DOCUMENT flow and returns a
 *              content:// URI) or "Select Folder" (SDL3 folder dialog on
 *              desktop; Mp6Activity's ACTION_OPEN_DOCUMENT_TREE flow via
 *              platform/android/saf_bridge.c on Android),
 *   import  -> progress bar (the ripped window.rcss verification-progress
 *              styles, used by partyboard for their disc-hash modal) fed
 *              from the import worker, with Cancel,
 *   error   -> precise message (wrong game/region, unreadable image, out
 *              of space), back to choose,
 *   done    -> refresh the launcher's content state and pop; the prelaunch
 *              menu's first button flips to "Play" on its own.
 */
#pragma once

#include "button.hpp"
#include "window.hpp"

#include "../../content/content_import.h"

#include <memory>
#include <string>
#include <vector>

namespace mp6::ui {

class ContentSetup : public WindowSmall {
public:
    ContentSetup();
    ~ContentSetup() override;

    void update() override;
    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

private:
    enum class Stage {
        Choose,
        PickingImage,  /* SDL file dialog open, waiting for its callback */
        PickingFolder, /* desktop: SDL folder dialog; android: SAF tree picker */
        Importing,
        Error,
    };

    void set_stage(Stage stage);
    void rebuild_actions();
    void begin_image_pick();
    void begin_folder_pick();
    void begin_import_image(const std::string &source);
    void begin_import_folder(const std::string &source);
    void poll_import();
    void show_error(const std::string &message);
    void finish_success();
    std::string dest_disc_root();

    Stage mStage = Stage::Choose;
    bool mCancelRequested = false;
    bool mUsingSafTree = false; /* android folder import runs on the Java side */

    Rml::Element *mTitle = nullptr;
    Rml::Element *mMessage = nullptr;
    Rml::Element *mProgressWrap = nullptr;
    Rml::Element *mProgressFile = nullptr;
    Rml::Element *mProgressBar = nullptr;
    Rml::Element *mProgressDetail = nullptr;
    Rml::Element *mActions = nullptr;
    std::vector<std::unique_ptr<Button>> mButtons;
};

} // namespace mp6::ui
