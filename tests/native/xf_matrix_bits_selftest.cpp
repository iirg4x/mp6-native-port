#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <dolphin/gx/GXEnum.h>
#include <aurora/math.hpp>
#include "selftest_assert.h"

using aurora::Mat3x4;
using aurora::Vec4;
#include "xf_types.inc"
constexpr u32 MaxPnMtx = 10, MaxTexMtx = 10, MaxPTTexMtx = 20;
struct State {
    std::array<PnMtx, MaxPnMtx> pnMtx{};
    std::array<Mat3x4<float>, MaxTexMtx> texMtxs{};
    std::array<Mat3x4<float>, MaxPTTexMtx> ptTexMtxs{};
    std::array<Light, 8> lights{};
    bool stateDirty = false;
};
static Vec4<float> unpack_color(u32 value) {
    return {float((value >> 24) & 255) / 255, float((value >> 16) & 255) / 255,
            float((value >> 8) & 255) / 255, float(value & 255) / 255};
}
#define CHECK(condition, ...) assert(condition)
template<class T> static T unaligned_load(const void* source) {
    T value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}
static u32 bswap(u32 value) { return __builtin_bswap32(value); }
namespace reference {
State g_gxState;
#include "xf_reference.inc"
}
namespace candidate {
State g_gxState;
#include "xf_subject.inc"
}

void equal_state() {
    const auto& a = reference::g_gxState;
    const auto& b = candidate::g_gxState;
    assert(std::memcmp(a.pnMtx.data(), b.pnMtx.data(), sizeof(a.pnMtx)) == 0);
    assert(std::memcmp(a.texMtxs.data(), b.texMtxs.data(), sizeof(a.texMtxs)) == 0);
    assert(std::memcmp(a.ptTexMtxs.data(), b.ptTexMtxs.data(), sizeof(a.ptTexMtxs)) == 0);
    assert(std::memcmp(a.lights.data(), b.lights.data(), sizeof(a.lights)) == 0);
}

// Intentionally unaligned FIFO data, encoded independently of the decoder.
void write(u32 addr, const u32* words, u32 count, bool bigEndian) {
    std::array<u8, 65> bytes{};
    for (u32 i = 0; i < count; ++i)
        for (u32 byte = 0; byte < 4; ++byte)
            bytes[1 + i * 4 + byte] = u8(words[i] >> ((bigEndian ? 3 - byte : byte) * 8));
    const bool a = reference::copy_xf_data(addr, bytes.data() + 1, count, bigEndian);
    const bool b = candidate::copy_xf_data(addr, bytes.data() + 1, count, bigEndian);
    assert(a == b); // Retain existing return behavior, including position writes.
    equal_state();
}

int main(int argc, char** argv) {
    const bool negative = argc > 1;
    std::array<u32, 16> words{0, 0x80000000, 0x7f800000, 0xff800000,
        0x7fc00001, 0x7fc12345, 0x7f800001, 0xff800001,
        0x00000001, 0x80000001, 0x3f800000, 0xbf800000};
    u32 rng = 0x6b203d41;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    unsigned checks = 0;
    for (bool bigEndian : {false, true}) {
        for (u32 kind = 0; kind < 4; ++kind) {
            const u32 limit = kind == 3 ? MaxPTTexMtx : MaxPnMtx;
            for (u32 slot = 0; slot < limit; ++slot) {
                const u32 addr = kind == 0 ? slot * 12 : kind == 1 ? 0x78 + slot * 12
                                 : kind == 2 ? 0x400 + slot * 9 : 0x500 + slot * 12;
                const u32 count = kind == 2 ? 9 : 12;
                for (u32 iteration = 0; iteration < 128; ++iteration) {
                    write(addr, words.data(), count, bigEndian);
                    candidate::g_gxState.stateDirty = false;
                    write(addr, words.data(), count, bigEndian);
                    assert(!candidate::g_gxState.stateDirty);
                    if (negative) return 0;
                    // Never clear an earlier dirty barrier, even for equal bits.
                    candidate::g_gxState.stateDirty = true;
                    write(addr, words.data(), count, bigEndian);
                    assert(candidate::g_gxState.stateDirty);
                    const u32 index = iteration % count;
                    words[index] ^= iteration == 0 ? 0x80000000u : 1u;
                    candidate::g_gxState.stateDirty = false;
                    write(addr, words.data(), count, bigEndian);
                    assert(candidate::g_gxState.stateDirty);
                    // A 2x4 texture upload must preserve the third row exactly.
                    if (kind == 1) {
                        const auto tail = candidate::g_gxState.texMtxs[slot].m2;
                        words[0] ^= 1;
                        write(addr, words.data(), 8, bigEndian);
                        assert(std::memcmp(&tail, &candidate::g_gxState.texMtxs[slot].m2, 16) == 0);
                        candidate::g_gxState.stateDirty = false;
                        write(addr, words.data(), 8, bigEndian);
                        assert(!candidate::g_gxState.stateDirty);
                    }
                    // Simulate state restoration/external matrix writers. The
                    // incoming payload repeats, but live destination has changed.
                    auto savedA = reference::g_gxState;
                    auto savedB = candidate::g_gxState;
                    reference::g_gxState = {};
                    candidate::g_gxState = {};
                    write(addr, words.data(), count, bigEndian);
                    assert(candidate::g_gxState.stateDirty);
                    reference::g_gxState = savedA;
                    candidate::g_gxState = savedB;
                    candidate::g_gxState.stateDirty = false;
                    write(addr, words.data(), count, bigEndian);
                    assert(!candidate::g_gxState.stateDirty);
                    for (auto& word : words) word = next();
                    ++checks;
                }
            }
        }
        // Normal padding is not uploaded, and must not make an equal write dirty.
        for (u32 row = 0; row < 3; ++row) {
            const u32 sentinel = 0x7fc12345;
            auto& a = reference::g_gxState.pnMtx[0].nrm;
            auto& b = candidate::g_gxState.pnMtx[0].nrm;
            auto& rowA = row == 0 ? a.m0 : row == 1 ? a.m1 : a.m2;
            auto& rowB = row == 0 ? b.m0 : row == 1 ? b.m1 : b.m2;
            std::memcpy(&rowA[3], &sentinel, 4);
            std::memcpy(&rowB[3], &sentinel, 4);
        }
        write(0x400, words.data(), 9, bigEndian);
        candidate::g_gxState.stateDirty = false;
        write(0x400, words.data(), 9, bigEndian);
        assert(!candidate::g_gxState.stateDirty);
        // All light partial writes, padding-only writes, and unhandled addresses
        // retain the original decoder behavior; this candidate changes matrices only.
        for (u32 light = 0; light < 8; ++light)
            for (u32 start = 0; start < 16; ++start)
                for (u32 count = 1; count <= 16 - start; ++count) {
                    candidate::g_gxState.stateDirty = false;
                    write(0x600 + light * 16 + start, words.data(), count, bigEndian);
                    assert(candidate::g_gxState.stateDirty);
                }
        for (u32 addr : {0xf0u, 0x3ffu, 0x45au, 0x4ffu, 0x5f0u, 0x680u, 0x1000u}) {
            candidate::g_gxState.stateDirty = false;
            write(addr, words.data(), 1, bigEndian);
            assert(!candidate::g_gxState.stateDirty);
        }
    }
    std::printf("%u matrix sequences: bit-exact state; repeat/dirty/reset/partial/endianness checks passed\n", checks);
}
