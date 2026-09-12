#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Recording-thread owned. Only publish a result after the authoritative cache
// confirms compilation completed. Clear together with that cache at shutdown.
// Hash identity has the same semantics as the existing pipeline map; this does
// not introduce a second configuration hash or keep pointers into the map.
template <class Info, std::size_t Slots = 64>
class ReadyPipelineMemo {
  static_assert(Slots && (Slots & (Slots - 1)) == 0);
  struct Entry {
    std::uint64_t hash{};
    std::uint32_t alignment{};
    std::uint32_t firstFrame{};
    Info info{};
    bool valid{};
  };
  std::array<Entry, Slots> entries{};

public:
  bool get(std::uint64_t hash, std::uint32_t alignment,
           std::uint32_t frame, Info& output) const {
    const auto& entry = entries[hash & (Slots - 1)];
    // A request earlier than the remembered observation must visit the real
    // cache, where firstFrameUsed persistence is updated normally.
    if (!entry.valid || entry.hash != hash || entry.alignment != alignment ||
        frame < entry.firstFrame) return false;
    output = entry.info;
    return true;
  }

  void remember(std::uint64_t hash, std::uint32_t alignment,
                std::uint32_t frame, const Info& info) {
    auto& entry = entries[hash & (Slots - 1)];
    entry = Entry{hash, alignment, frame, info, true};
  }

  void clear() {
    for (auto& entry : entries) entry.valid = false;
  }
};
