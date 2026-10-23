#pragma once
#include "pdt_music.hpp"

namespace mp6::audio {

struct MenuCue {
    uint32_t rate = 0;
    std::vector<int16_t> pcm;
};

// The four common UI cues are simple, non-looping MuSyX macros. Read only
// those from the user's GSND bank; this is deliberately not another macro VM.
inline MenuCue menu_cue(const MusicRead &read, uint32_t bytes, unsigned cue) {
    MenuCue result;
    const auto fits = [](uint64_t at, uint64_t n, uint64_t size) {
        return at <= size && n <= size - at;
    };
    std::array<uint8_t, 96> h{};
    if (cue > 3 || bytes < h.size() || !read(0, h.data(), h.size()) ||
        music_be32(h.data()) != 0x47534e44 || music_be32(h.data() + 4) != 2)
        return {};
    const auto header = [&](int p) { return music_be32(h.data() + p); };
    for (int p : {32, 48, 56, 64})
        if (!fits(header(p), header(p + 4), bytes))
            return {};
    if (!fits(cue * 16, 16, header(52)) || header(36) > 1024 * 32 || header(36) % 32)
        return {};
    std::array<uint8_t, 16> se{};
    if (!read(header(48) + cue * 16, se.data(), se.size()))
        return {};
    const auto group = music_be16(se.data()), fx = music_be16(se.data() + 2);
    if (se[4] > 127 || se[5] != 64 || music_be16(se.data() + 6) != 0)
        return {};
    std::array<uint8_t, 32> info{};
    bool found = false;
    for (uint32_t at = 0; at < header(36); at += 32) {
        if (!read(header(32) + at, info.data(), info.size()))
            return {};
        if (music_be16(info.data()) == group && music_be32(info.data() + 8) > 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return {};
    const uint32_t dataOffset = music_be32(info.data() + 4), dataSize = music_be32(info.data() + 8);
    const uint32_t sampleOffset = music_be32(info.data() + 12),
                   sampleSize = music_be32(info.data() + 16);
    if (dataSize < 16 || dataSize > 4 * 1024 * 1024 || !fits(dataOffset, dataSize, header(60)) ||
        !fits(sampleOffset, sampleSize, header(68)))
        return {};
    std::vector<uint8_t> data(dataSize);
    if (!read(header(56) + dataOffset, data.data(), data.size()))
        return {};
    const auto has = [&](uint64_t at, uint64_t n) { return fits(at, n, data.size()); };
    const auto u32 = [&](uint64_t p) { return music_be32(data.data() + p); };
    const auto u16 = [&](uint64_t p) { return music_be16(data.data() + p); };
    const uint64_t pool = u32(0), proj = u32(4), sdir = u32(8);
    if (!has(pool, 16) || !has(proj, 40) || !has(sdir, 32))
        return {};
    uint64_t node = proj, table = 0;
    found = false;
    for (unsigned i = 0; i < 1024 && has(node, 40); ++i) {
        if (u16(node + 4) == group && u16(node + 6) == 1) {
            table = proj + u32(node + 28);
            found = true;
            break;
        }
        if (u32(node) == 0xffffffff)
            break;
        node = proj + u32(node);
    }
    if (!found || !has(table, 4) || !has(table + 4, uint64_t(u16(table)) * 10))
        return {};
    uint16_t object = 0;
    found = false;
    uint8_t volume = 0;
    for (unsigned i = 0; i < u16(table); ++i) {
        const auto e = table + 4 + i * 10;
        if (u16(e) != fx)
            continue;
        object = u16(e + 2);
        volume = data[e + 6];
        if (volume > 127 || data[e + 7] != 64 || data[e + 8] != 60)
            return {};
        found = true;
        break;
    }
    if (!found)
        return {};
    node = pool + u32(pool);
    found = false;
    for (unsigned i = 0; i < 4096 && has(node, 8); ++i) {
        if (u16(node + 4) == object) {
            found = true;
            break;
        }
        const auto next = u32(node);
        if (next == 0xffffffff || next < 8)
            break;
        node += next;
    }
    if (!found || !has(node + 8, 40))
        return {};
    const auto macro = node + 8;
    // SetKeyGroup(0), StartSample, Wait-for-sample, StopSample, End. Reject
    // unexpected pitch, envelopes, branches or loops rather than misplaying.
    if (u32(macro) != 0x59 || u32(macro + 4) != 0 || (u32(macro + 8) & 0xff0000ff) != 0x10 ||
        u32(macro + 12) != 0 || u32(macro + 16) != 0x01000107 || u32(macro + 20) != 0xffff0000 ||
        u32(macro + 24) != 0x11 || u32(macro + 28) != 0 || u32(macro + 32) != 0 ||
        u32(macro + 36) != 0)
        return {};
    const uint16_t sample = (u32(macro + 8) >> 8) & 0xffff;
    found = false;
    uint64_t entry = sdir;
    for (unsigned i = 0; i < 4096 && has(entry, 32); ++i, entry += 32) {
        if (u16(entry) == 0xffff)
            break;
        if (u16(entry) == sample) {
            found = true;
            break;
        }
    }
    if (!found)
        return {};
    const auto rate = u32(entry + 12) & 0xffff, length = u32(entry + 16),
               rawOffset = u32(entry + 4);
    const uint64_t coeff = sdir + u32(entry + 28);
    if (rate < 8000 || rate > 48000 || !length || length > rate * 2 || u32(entry + 24) != 0 ||
        !has(coeff, 40) || u16(coeff) != 8)
        return {};
    const uint32_t frames = (length + 13) / 14;
    if (!fits(rawOffset, frames * 8, sampleSize))
        return {};
    std::vector<uint8_t> raw(frames * 8);
    if (!read(header(64) + sampleOffset + rawOffset, raw.data(), raw.size()))
        return {};
    for (uint32_t i = 0; i < frames; ++i)
        if ((raw[i * 8] >> 4) > 7)
            return {};
    MP6AdpcmCoefTable coefs{};
    MP6AdpcmState history{};
    for (int i = 0; i < 16; ++i)
        coefs.coef[i / 2][i % 2] = int16_t(u16(coeff + 8 + i * 2));
    result.pcm.resize(frames * 14);
    result.rate = rate;
    mp6_dspadpcm_decode(raw.data(), frames, &coefs, &history, result.pcm.data());
    result.pcm.resize(length);
    const float gain = float(se[4]) * volume / (127.f * 127.f);
    for (auto &v : result.pcm)
        v = int16_t(v * gain);
    return result;
}
} // namespace mp6::audio
