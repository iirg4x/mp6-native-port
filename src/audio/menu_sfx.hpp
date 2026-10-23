#pragma once
#include <cstdint>
namespace mp6::audio {
// MSM_SE_CMN_01..04 from the linked disc; never bundled or added to game state.
enum class MenuSound { Move, Confirm, Accept, Cancel };
void update_menu_sounds(const char* validatedFilesRoot,float masterGain);
void play_menu_sound(MenuSound cue);
void set_menu_sound_gain(float masterGain);
void shutdown_menu_sounds();
bool menu_sounds_ready();
uint64_t menu_sounds_played();
}
