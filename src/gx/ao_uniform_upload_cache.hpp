#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

namespace mp6::ao {

/*
 * A cache is owned by exactly one GPU uniform buffer.  The validity bit is
 * deliberately separate from the byte snapshot: an all-zero value must still
 * be uploaded the first time a newly-created buffer is used.
 */
template <typename T>
class UniformUploadCache {
    static_assert(std::is_trivially_copyable_v<T>,
                  "uniform upload values must have a stable byte representation");

    std::array<std::byte, sizeof(T)> snapshot{};
    bool has_snapshot = false;

public:
    void reset() noexcept { has_snapshot = false; }

    bool valid() const noexcept { return has_snapshot; }

    template <typename Upload>
    bool upload_if_changed(const T &value, Upload &&upload) {
        if (has_snapshot && std::memcmp(snapshot.data(), &value, sizeof(value)) == 0)
            return false;
        upload(&value, sizeof(value));
        std::memcpy(snapshot.data(), &value, sizeof(value));
        has_snapshot = true;
        return true;
    }
};

} // namespace mp6::ao
