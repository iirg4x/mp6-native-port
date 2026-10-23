#include "menu_music.hpp"
#include "pdt_music.hpp"
#include "menu_sfx.hpp"
#include "menu_sfx_bank.hpp"
#include <SDL3/SDL.h>
#include "mp6_audio_out.h"
#include <cstdio>
#include <string>
#include "mp6_host_section.h"

namespace mp6::audio {

struct MenuMusic::Impl {
    SDL_IOStream *file = nullptr;
    SDL_AudioStream *stream = nullptr;
    bool resumed = false;
    std::string root;
    std::vector<MusicTrack> tracks;
    MusicShuffle shuffle{uint32_t(SDL_GetTicksNS()) ^ uint32_t(SDL_GetPerformanceCounter())};
    MusicDecoder decoder;
    uint64_t queued = 0;
    float gain = -1;
    int trackId = -1;
    ~Impl() { close(); }
    void close() {
        if (stream)
            SDL_DestroyAudioStream(stream);
        stream = nullptr;
        resumed = false;
        if (file)
            SDL_CloseIO(file);
        file = nullptr;
        tracks.clear();
        trackId = -1;
    }
    bool read(uint32_t offset, void *dst, size_t bytes) {
        return file && SDL_SeekIO(file, offset, SDL_IO_SEEK_SET) == offset &&
               SDL_ReadIO(file, dst, bytes) == bytes;
    }
    MusicRead reader() {
        return [this](uint32_t at, void *dst, size_t size) { return read(at, dst, size); };
    }
    bool next() {
        const auto &track = tracks[shuffle.next()];
        decoder.start(track);
        trackId = int(track.id);
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(track.rate)};
        if (!SDL_SetAudioStreamFormat(stream, &spec, nullptr))
            return false;
        printf("[MENU-MUSIC] stream %u, %u Hz, %.1fs\n", track.id, track.rate,
               double(decoder.length()) / track.rate);
        return true;
    }
    void open() {
        if (root.empty())
            return;
        file = SDL_IOFromFile((root + "/sound/MP6_Str.pdt").c_str(), "rb");
        if (!file)
            return;
        const auto size = SDL_GetIOSize(file);
        if (size <= 0 || uint64_t(size) > UINT32_MAX) {
            close();
            return;
        }
        tracks = menu_music_catalog(reader(), uint32_t(size));
        if (tracks.empty()) {
            printf("[MENU-MUSIC] no valid idle music tracks; staying silent\n");
            close();
            return;
        }
        if (!mp6_audio_sdl_init()) {
            close();
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
        stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            printf("[MENU-MUSIC] audio device unavailable: %s\n", SDL_GetError());
            close();
            return;
        }
        shuffle.reset(tracks.size());
        gain = -1;
        resumed = false;
        printf("[MENU-MUSIC] linked disc: %zu idle tracks\n", tracks.size());
        if (!next()) {
            close();
            return;
        }
    }
    void pump(float masterGain) {
        if (!stream)
            return;
        masterGain = std::clamp(masterGain, 0.f, 1.f);
        if (gain != masterGain) {
            gain = masterGain;
            if (!SDL_SetAudioStreamGain(stream, gain)) {
                close();
                return;
            }
        }
        std::array<int16_t, MusicDecoder::blockSamples * 2> pcm;
        for (unsigned batch = 0; batch < 8; ++batch) {
            const int pending = SDL_GetAudioStreamQueued(stream);
            if (pending < 0) {
                close();
                return;
            }
            if (pending >= int(decoder.track().rate * 4 / 2))
                break; // half a second of stereo S16
            if (decoder.position() == decoder.length() && !next()) {
                close();
                return;
            }
            const uint32_t position = decoder.position();
            const auto track = decoder.track();
            const int frames = decoder.pull(reader(), pcm.data());
            if (frames <= 0) {
                printf("[MENU-MUSIC] bank read failed; stopping playback\n");
                close();
                return;
            }
            for (int i = 0; i < frames; ++i) {
                const float fade =
                    std::min({1.f, (position + i) / float(track.rate * .15f),
                              (decoder.length() - position - i) / float(track.rate * .20f)});
                const float volume = fade * track.volume / 127.f;
                pcm[i * 2] = int16_t(pcm[i * 2] * volume);
                pcm[i * 2 + 1] = int16_t(pcm[i * 2 + 1] * volume);
            }
            if (!SDL_PutAudioStreamData(stream, pcm.data(), frames * 4)) {
                close();
                return;
            }
            queued += frames;
        }
        if (!resumed) {
            if (!SDL_ResumeAudioStreamDevice(stream)) {
                close();
                return;
            }
            resumed = true;
        }
    }
};
MenuMusic::MenuMusic() : m(std::make_unique<Impl>()) {}
MenuMusic::~MenuMusic() = default;
void MenuMusic::update(const char *root, float gain) {
    const char *nextRoot = root ? root : "";
    if (m->root != nextRoot) {
        m->close();
        m->root = nextRoot;
        m->open();
    }
    m->pump(gain);
}
bool MenuMusic::playing() const { return m->stream != nullptr; }
size_t MenuMusic::track_count() const { return m->tracks.size(); }
int MenuMusic::current_track() const { return m->trackId; }
uint64_t MenuMusic::queued_frames() const { return m->queued; }

namespace {

struct MenuSounds {
    std::string root;
    std::array<MenuCue, 4> cues;
    std::array<SDL_AudioStream *, 4> streams{};
    uint64_t played = 0;
    float gain = -1;
    ~MenuSounds() { close(); }
    void close() {
        for (auto *&stream : streams) {
            if (stream)
                SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        for (auto &cue : cues)
            cue = {};
        gain = -1;
    }
    void open() {
        if (root.empty())
            return;
        auto *file = SDL_IOFromFile((root + "/sound/MP6_SND.msm").c_str(), "rb");
        if (!file)
            return;
        const auto size = SDL_GetIOSize(file);
        const MusicRead read = [&](uint32_t at, void *out, size_t n) {
            return SDL_SeekIO(file, at, SDL_IO_SEEK_SET) == at && SDL_ReadIO(file, out, n) == n;
        };
        if (size > 0 && uint64_t(size) <= UINT32_MAX)
            for (unsigned i = 0; i < cues.size(); ++i)
                cues[i] = menu_cue(read, uint32_t(size), i);
        SDL_CloseIO(file);
        for (const auto &cue : cues)
            if (cue.pcm.empty()) {
                close();
                return;
            }
        if (!mp6_audio_sdl_init()) {
            close();
            return;
        }
        for (unsigned i = 0; i < cues.size(); ++i) {
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, int(cues[i].rate)};
            streams[i] = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                   nullptr, nullptr);
            if (!streams[i]) {
                close();
                return;
            }
        }
        // The newly opened streams have not received the cached configuration.
        gain = -1;
        printf("[MENU-SFX] preloaded four linked-disc UI cues\n");
    }
};
std::unique_ptr<MenuSounds> menuSounds;

} // namespace
void update_menu_sounds(const char *root, float gain) {
    if (!menuSounds)
        menuSounds = std::make_unique<MenuSounds>();
    auto &s = *menuSounds;
    const char *next = root ? root : "";
    if (s.root != next) {
        s.close();
        s.root = next;
        s.open();
    }
    const float nextGain = std::clamp(gain, 0.f, 1.f);
    if (s.gain != nextGain)
        set_menu_sound_gain(nextGain);
}
void set_menu_sound_gain(float gain) {
    if (!menuSounds)
        return;
    auto &s = *menuSounds;
    const float next = std::clamp(gain, 0.f, 1.f);
    if (s.gain == next)
        return;
    s.gain = next;
    for (auto *stream : s.streams)
        if (stream)
            SDL_SetAudioStreamGain(stream, s.gain);
}
void play_menu_sound(MenuSound cue) {
    const unsigned i = unsigned(cue);
    if (!menuSounds || i >= 4)
        return;
    auto &s = *menuSounds;
    auto *stream = s.streams[i];
    if (!stream || s.gain <= 0)
        return;
    // One bounded voice per cue. Rapid cursor repeats restart, never queue a backlog.
    SDL_ClearAudioStream(stream);
    if (SDL_PutAudioStreamData(stream, s.cues[i].pcm.data(),
                               int(s.cues[i].pcm.size() * sizeof(int16_t))) &&
        SDL_FlushAudioStream(stream) && SDL_ResumeAudioStreamDevice(stream))
        ++s.played;
}
void shutdown_menu_sounds() { menuSounds.reset(); }
bool menu_sounds_ready() { return menuSounds && menuSounds->streams[0]; }
uint64_t menu_sounds_played() { return menuSounds ? menuSounds->played : 0; }

} // namespace mp6::audio
