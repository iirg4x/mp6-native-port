#pragma once

#include <memory>
#include <utility>

namespace mp6::ao {

// Render-worker only. A lease covers encoding the producer through encoding
// its last consumer, NOT GPU completion. Reuse is valid only on the same
// ordered command/submission stream. Encoded commands retain GPU references;
// this pool never explicitly destroys a texture or reuses uniform buffers.
// If a consumer is delayed or rejected, its outstanding lease prevents reuse.
template <typename T>
class SerialScratchPool {
    std::shared_ptr<T> cached;

public:
    template <typename Matches, typename Create>
    std::shared_ptr<T> acquire(Matches &&matches, Create &&create) {
        if (!cached || cached.use_count() != 1 || !matches(*cached))
            cached = std::forward<Create>(create)();
        return cached;
    }

    // Dropping the cache cannot invalidate an outstanding lease or commands
    // which already retain the GPU objects. Used on fallback and shutdown.
    void reset() { cached.reset(); }

    const std::shared_ptr<T> &retained() const noexcept { return cached; }
};

} // namespace mp6::ao
