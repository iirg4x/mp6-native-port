#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace aurora::gx::compact_instance {
// Word-addressed through the existing read-only attribute storage binding.
// No float conversion: these are the original serialized GPU matrix bits.
constexpr uint32_t RecordWords = 40;
constexpr uint32_t RecordBytes = RecordWords * sizeof(uint32_t);
constexpr uint32_t MaxInstances = 8;
using Record = std::array<uint32_t, RecordWords>;

inline bool compatible_uniforms(std::span<const uint8_t> a,
                                std::span<const uint8_t> b, uint32_t texMatrices) {
  if (texMatrices > 10) return false;
  const size_t normal = 192 + texMatrices * 48;
  if (a.size() != b.size() || a.size() < normal + 48) return false;
  // Vertex address, array bases, model position/normal matrices are per-instance.
  // Padding at 24..31 holds the record address in the instanced variant only.
  const auto equal = [&](size_t start, size_t size) {
    return std::memcmp(a.data() + start, b.data() + start, size) == 0;
  };
  return equal(4, 20) && equal(80, 64) && equal(192, normal - 192) &&
         equal(normal + 48, a.size() - normal - 48);
}

inline Record pack_record(std::span<const uint8_t> u, uint32_t texMatrices) {
  Record record{};
  const size_t normal = 192 + texMatrices * 48;
  if (texMatrices > 10 || u.size() < normal + 48) return record;
  std::memcpy(record.data(), u.data(), 4);                    // vertex byte address
  std::memcpy(record.data() + 1, u.data() + 32, 48);          // 12 array byte addresses
  std::memcpy(record.data() + 16, u.data() + 144, 48);        // model position matrix
  std::memcpy(record.data() + 28, u.data() + normal, 48);      // model normal matrix
  return record;
}
} // namespace aurora::gx::compact_instance
