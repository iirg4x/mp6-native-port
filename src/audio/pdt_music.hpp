// Read-only front-end access to MP6's stream bank. Does not initialize or alter
// the game's MSM mixer/DVD state. Format authority: msm_bridge.c / MP6_Str.pdt.
#pragma once
#include "dspadpcm.h"
#include "msm_safe.h"
#include <algorithm>
#include <array>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

namespace mp6::audio {

using MusicRead = std::function<bool(uint32_t, void *, size_t)>;
struct MusicTrack {
    uint32_t id = 0, rate = 0, adpcmFrames = 0, sampleOffset[2]{};
    unsigned channels = 0, volume = 0;
    MP6AdpcmCoefTable coefficients[2]{};
};
inline uint16_t music_be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t music_be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline std::vector<MusicTrack> music_catalog(const MusicRead &read, uint32_t size) {
    uint8_t header[32];
    std::vector<MusicTrack> tracks;
    if (size < 32 || !read(0, header, sizeof(header)) || music_be16(header) != 1)
        return {};
    const uint32_t count = music_be16(header + 2), rate = music_be32(header + 8);
    const uint32_t list = music_be32(header + 16), coeff = music_be32(header + 20),
                   packs = music_be32(header + 24), samples = music_be32(header + 28);
    if (!count || count > 4096 || rate < 8000 || rate > 96000 || list < 32 || coeff < list ||
        packs <= coeff || samples <= packs || samples > size ||
        uint64_t(count) * 4 > coeff - list || (packs - coeff) % 32 || samples - packs < 32)
        return {};
    for (uint32_t id = 0; id < count; ++id) {
        uint8_t offset[4], pack[32];
        if (!read(list + id * 4, offset, 4))
            return {};
        const uint32_t address = music_be32(offset);
        if (!address)
            continue;
        if (address < packs || uint64_t(address) + 32 > samples || !read(address, pack, 32))
            return {};
        // Bit 1 marks authored looping music. Non-looping jingles/cinematics
        // are excluded here; the menu applies a narrower idle-only policy below.
        if (!(pack[0] & 2))
            continue;
        const uint32_t bytes = (music_be32(pack + 8) >> 1) & ~31u,
                       loop = (music_be32(pack + 12) >> 1) & ~7u;
        MusicTrack track;
        track.id = id;
        track.rate = music_be16(pack + 6);
        if (!track.rate)
            track.rate = rate;
        track.channels = (pack[0] & 1) ? 2 : 1;
        track.volume = pack[1];
        track.adpcmFrames = bytes / 8;
        if ((pack[0] & ~3) || track.volume > 127 || track.rate < 8000 || track.rate > 96000 ||
            !bytes || loop >= bytes ||
            !mp6_msm_decode_budget_valid(uint64_t(track.adpcmFrames) * 14, 4))
            return {};
        for (unsigned channel = 0; channel < track.channels; ++channel) {
            const uint32_t offset = music_be32(pack + 16 + channel * 8),
                           index = music_be16(pack + 20 + channel * 8);
            uint8_t table[32];
            if (offset < samples || !mp6_msm_span_valid_u32(size, offset, bytes) ||
                uint64_t(index) * 32 + 32 > packs - coeff || !read(coeff + index * 32, table, 32))
                return {};
            track.sampleOffset[channel] = offset;
            for (unsigned i = 0; i < 8; ++i)
                for (unsigned j = 0; j < 2; ++j)
                    track.coefficients[channel].coef[i][j] =
                        int16_t(music_be16(table + i * 4 + j * 2));
        }
        tracks.push_back(track);
    }
    return tracks;
}

inline bool idle_music_track(uint32_t id) {
    // GP6E01 stream IDs verified at the game call sites, not the legacy
    // msm_stream.h names (some of those names describe a different track).
    switch (id) {
    case 2:  // fileselDll/filesel.c: file selection.
    case 3:  // mdselDll/mdsel.c: mode selection (MSM_STREAM_MODESEL).
    case 4:  // mdpartyDll/mdparty.c: party setup.
    case 6:  // mdminiDll/mode_fade_in.c: minigame selection, not gameplay.
    case 7:  // optionDll/option.c: options room.
    case 92: // mdbankDll/mdbank.c: Star Bank.
        return true;
    default:
        return false;
    }
}

inline std::vector<MusicTrack> menu_music_catalog(const MusicRead &read, uint32_t size) {
    auto tracks = music_catalog(read, size);
    tracks.erase(
        std::remove_if(tracks.begin(), tracks.end(),
                       [](const MusicTrack &track) { return !idle_music_track(track.id); }),
        tracks.end());
    // No fallback to arbitrary looping music if no approved idle tracks exist.
    return tracks;
}

class MusicDecoder {
public:
    static constexpr uint32_t blockFrames = 256, blockSamples = blockFrames * 14;
    void start(const MusicTrack &track) {
        mTrack = track;
        mPosition = 0;
        mHistory = {};
    }
    uint32_t position() const { return mPosition * 14; }
    uint32_t length() const { return mTrack.adpcmFrames * 14; }
    const MusicTrack &track() const { return mTrack; }
    // Main/UI thread only. Bounded 2 KiB reads per channel; no entire-song
    // allocation and no file I/O or decoding in SDL's device callback.
    int pull(const MusicRead &read, int16_t *stereo) {
        const uint32_t frames = std::min(blockFrames, mTrack.adpcmFrames - mPosition);
        if (!frames)
            return 0;
        std::array<uint8_t, blockFrames * 8> raw;
        std::array<int16_t, blockSamples> mono;
        for (unsigned ch = 0; ch < mTrack.channels; ++ch) {
            if (!read(mTrack.sampleOffset[ch] + mPosition * 8, raw.data(), frames * 8))
                return -1;
            mp6_dspadpcm_decode(raw.data(), frames, &mTrack.coefficients[ch], &mHistory[ch],
                                mono.data());
            for (uint32_t i = 0; i < frames * 14; ++i)
                stereo[i * 2 + ch] = mono[i];
        }
        if (mTrack.channels == 1)
            for (uint32_t i = 0; i < frames * 14; ++i)
                stereo[i * 2 + 1] = stereo[i * 2];
        mPosition += frames;
        return int(frames * 14);
    }

private:
    MusicTrack mTrack{};
    uint32_t mPosition = 0;
    std::array<MP6AdpcmState, 2> mHistory{};
};

class MusicShuffle {
public:
    explicit MusicShuffle(uint32_t seed) : mRandom(seed) {}
    void reset(size_t count) {
        mOrder.resize(count);
        std::iota(mOrder.begin(), mOrder.end(), 0);
        mCursor = count;
        mLast = count;
    }
    size_t next() {
        if (mOrder.empty())
            return 0;
        if (mCursor == mOrder.size()) {
            std::shuffle(mOrder.begin(), mOrder.end(), mRandom);
            if (mOrder.size() > 1 && mOrder.front() == mLast)
                std::swap(mOrder.front(), mOrder[1]);
            mCursor = 0;
        }
        return mLast = mOrder[mCursor++];
    }

private:
    std::mt19937 mRandom;
    std::vector<size_t> mOrder;
    size_t mCursor = 0, mLast = 0;
};
} // namespace mp6::audio
