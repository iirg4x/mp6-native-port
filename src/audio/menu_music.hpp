#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mp6::audio {
// Owned by the launcher loop or the visible in-game menu. Destruction stops and
// closes this private SDL stream. Nothing is added to game savestate state.
class MenuMusic {
public:
    MenuMusic();
    ~MenuMusic();
    MenuMusic(const MenuMusic&)=delete;
    MenuMusic& operator=(const MenuMusic&)=delete;
    void update(const char* validatedFilesRoot,float masterGain);
    bool playing() const;
    size_t track_count() const;
    int current_track() const;
    uint64_t queued_frames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
