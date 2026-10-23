/* Adapted from mariopartyrd/partyboard (unlicensed upstream; see docs/PARTYBOARD_PROVENANCE.md).
 * Credit: TwilitRealm. */
// Credits: TwilitRealm

#pragma once
#include "window.hpp"
#include "launcher_state.hpp"

namespace mp6::ui {

/* Touch Controls is Android-only. Save states remain in Game. */
inline constexpr int kSettingsTabVideo = 0;
inline constexpr int kSettingsTabAudio = 1;
#if defined(__ANDROID__) || defined(MP6_SETTINGS_TOUCH_QA)
inline constexpr int kSettingsTabControls = 2;
inline constexpr int kSettingsTabGame = 3;
inline constexpr int kSettingsTabMods = 4;
inline constexpr int kSettingsChapterCount = 5;
#else
inline constexpr int kSettingsTabGame = 2;
inline constexpr int kSettingsTabMods = 3;
inline constexpr int kSettingsChapterCount = 4;
#endif
inline constexpr int kSettingsTabAdvanced = kSettingsTabMods;
class StorybookView;

class SettingsWindow : public Window {
public:
    /* inGame adds save-state actions, enables live game-only settings and
     * hides instead of destroying the persistent menu on Back. */
    SettingsWindow(bool prelaunch = false, int initialTab = 0, bool inGame = false);
    static void preload_resources();
    void turn_from_launcher(const StorybookView& source);

    void update() override;
    void show() override;
    void hide(bool close) override;
    bool visible() const override;

    bool focus_content(); /* [MP6] focus the first content row (prelaunch content-root jump) */

protected:
    // Everything that can change a rendered story row lives here. Keep this
    // source snapshot in lockstep with row getters/disabled predicates so a
    // retained book can stay idle without losing external config changes.
    struct StorybookSource {
        int aa = 0;
        int ambientOcclusion = 0;
        int fpsCorner = 0;
        int heapScale = 0;
        int masterVolume = 0;
        int menuMotion = 0;
        int sfxVoices = 0;
        int shadowQuality = 0;
        int showFps = 0;
        int unlockedFps = 0;
        int smoothFrames = 0;
        int vsync = 0;
        int widescreen = 0;
        int windowMode = 0;
        int touchEnabled = 0;
        int touchSize = 0;
        int touchOpacity = 0;
        int touchFloating = 0;
        unsigned int touchVisible = 0;
        int freecam = 0;
        int unlockAll = 0;
        int quickPlay = 0;
        int microphone = 0;
        unsigned int disabledFlags = 0;
        float windowScale = 0;
        double tickHz = 0;
        char backend[24]{};
        char display[64]{};

        bool operator==(const StorybookSource &other) const;
    };

    void build_shell();
    void prepare_book(const char *title, const char *subtitle);
    void update_storybook();
    void storybook_pointer(Rml::Event &event, bool click);
    void build_picture(Rml::Element *content);
    void build_sound(Rml::Element *content);
    void build_controls(Rml::Element *content);
    void build_game(Rml::Element *content);
    void build_extras(Rml::Element *content);
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;
    std::unique_ptr<Button> mBackButton;
    std::unique_ptr<Button> mHelpButton;
    std::unique_ptr<Button> mChooseButton;
    std::unique_ptr<Button> mPreviousChapter;
    std::unique_ptr<Button> mNextChapter;
    Rml::Element *mChapterTitle = nullptr;
    Rml::Element *mChapterSubtitle = nullptr;
    Rml::Element *mController = nullptr;
    Rml::Element *mGuideCopy = nullptr;
    StorybookView *mStorybook = nullptr;
    std::vector<Rml::Element *> mStoryRows;
    std::vector<Rml::Element *> mStoryValues;
    std::vector<Rml::String> mStoryLabels;
    std::vector<Rml::String> mStoryValueLabels;
    std::vector<unsigned char> mStoryDisabled;
    std::vector<unsigned char> mStoryCanLeft;
    std::vector<unsigned char> mStoryCanRight;
    Rml::String mStoryChapter;
    Rml::Element *mStoryFocusRow = nullptr;
    StorybookSource mStorySource{};
    bool mStorySourceValid = false;
    bool mStoryRowsDirty = true;
    bool mStoryStateDirty = true;
    // TouchLayoutPreview consumes coordinates that are not printed in the
    // story rows. Keep an exact external snapshot so live layout edits wake
    // only the component tree, not the story text/state cache.
    Mp6TouchConfig mTouchLayoutSource{};
    bool mTouchLayoutSourceValid = false;
    int mControlViewportWidth = -1;
    int mControlViewportHeight = -1;
    float mControlViewportDensity = -1;
    int mChapterIndex = 0;
    int mDetailFocusPending = 0;
    float mFontPixels = -1;
    bool consume_close_request() override; /* [MP6] inGame: hide, don't close */
    bool mPrelaunch;
    bool mInGame;
    bool mClosing = false;
    unsigned int mSeenSavestateGen = 0; /* [MP6] Save States tab refresh latch */
};

} // namespace mp6::ui
