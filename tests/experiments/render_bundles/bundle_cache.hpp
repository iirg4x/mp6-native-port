#pragma once
#include <array>
#include <cstdint>
#include <utility>

// Private experiment: keys describe commands, never the live GPU buffer bytes.
namespace mp6_bundle_test {
constexpr unsigned MaxDraws = 32;
struct DrawKey {
  uint64_t pipeline = 0, textures = 0;
  uint32_t uniformOffset = 0, count = 0, instances = 0, firstIndex = 0;
  uint32_t indexed = 0, reserved = 0;
  bool operator==(const DrawKey&) const = default;
};
struct Key {
  uint64_t staticGroup = 0, uniformGroup = 0, indexBuffer = 0, emptyGroup = 0;
  uint32_t colorFormat = 0, depthFormat = 0, samples = 0, readOnly = 0, count = 0;
  std::array<DrawKey, MaxDraws> draws{};
  bool operator==(const Key&) const = default;
  uint64_t hash() const {
    // Explicit fields: no object padding, resource contents, or partial keys.
    uint64_t h = 14695981039346656037ull;
    auto add = [&](uint64_t value) { h ^= value; h *= 1099511628211ull; };
    add(staticGroup); add(uniformGroup); add(indexBuffer); add(emptyGroup);
    add(colorFormat); add(depthFormat); add(samples); add(readOnly); add(count);
    for (unsigned i = 0; i < count; ++i) {
      const auto& d = draws[i];
      add(d.pipeline); add(d.textures); add(d.uniformOffset); add(d.count);
      add(d.instances); add(d.firstIndex); add(d.indexed); add(d.reserved);
    }
    return h;
  }
};

// Four-way bounded cache; Payload owns the bundle and its immutable resources.
// First sightings only remember keys; compile after a confirmed repeat.
template<class Payload, unsigned Sets = 32, unsigned Ways = 4> struct Cache {
  static constexpr uint32_t MaxAge = 16;
  struct Entry {
    Key key{};
    Payload payload{};
    uint64_t hash = 0;
    uint32_t lastFrame = 0;
    bool occupied = false;
  };
  std::array<Entry, Sets * Ways> entries{};
  void clear() { for (auto& e : entries) e = {}; }
  void expire(uint32_t frame) {
    for (auto& e : entries)
      if (e.occupied && uint32_t(frame - e.lastFrame) > MaxAge) e = {};
  }
  std::pair<Entry*, bool> observe(const Key& key, uint32_t frame, uint64_t hash) {
    auto* set = entries.data() + (hash % Sets) * Ways;
    Entry* victim = &set[0];
    for (unsigned i = 0; i < Ways; ++i) {
      auto& e = set[i];
      if (e.occupied && uint32_t(frame - e.lastFrame) <= MaxAge &&
          e.hash == hash && e.key == key) {
        e.lastFrame = frame;
        return {&e, true};
      }
      if (!e.occupied || (victim->occupied &&
          uint32_t(frame - e.lastFrame) > uint32_t(frame - victim->lastFrame))) victim = &e;
    }
    *victim = {};
    victim->key = key;
    victim->hash = hash;
    victim->lastFrame = frame;
    victim->occupied = true;
    return {victim, false};
  }
};
} // namespace mp6_bundle_test
