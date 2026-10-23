#pragma once

// Internal recording-thread writer; include after common.hpp. Complete the
// write before publishing a pass or appending anything else to this buffer.
namespace aurora::gfx {
class UniformWriter {
public:
  UniformWriter() noexcept = default;
  UniformWriter(ByteBuffer& target, uint32_t capacity, uint32_t alignment) noexcept
  : m_target(&target), m_capacity(capacity) {
    if (alignment != 0 && (alignment & (alignment - 1)) != 0) abort();
    const size_t padding = alignment == 0 ? 0 : (0 - target.size()) & (alignment - 1);
    if (target.size() > UINT32_MAX || padding > UINT32_MAX - target.size() ||
        capacity > UINT32_MAX - target.size() - padding) abort();
    target.append_uninitialized(padding);
    m_offset = target.size();
    target.reserve_extra(capacity);
    m_data = target.data() ? target.data() + m_offset : nullptr;
    if (capacity && !m_data) abort();
  }
  UniformWriter(const UniformWriter&) = delete;
  UniformWriter& operator=(const UniformWriter&) = delete;
  explicit operator bool() const noexcept { return m_target != nullptr; }
  // [MP6] Absolute buffer offset of the next appended byte (retained replay).
  size_t offset() const noexcept { return m_offset + m_size; }

  template <typename T>
  void append(const T& value) noexcept {
    // Aurora vectors have custom assignment operators. Copy their object bytes
    // into byte storage exactly as ByteBuffer::append does, not via assignment.
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_destructible_v<T>);
    if (sizeof(T) > m_capacity - m_size) abort();
    std::memcpy(m_data + m_size, &value, sizeof(T));
    m_size += sizeof(T);
  }
  void append_zeroes(size_t size) noexcept {
    if (size > m_capacity - m_size) abort();
    if (size) std::memset(m_data + m_size, 0, size);
    m_size += size;
  }
  Range finish() noexcept {
    if (!m_target || m_target->size() != m_offset ||
        (m_target->data() ? m_target->data() + m_offset : nullptr) != m_data) abort();
    // Publish only initialized payload bytes, not ShaderInfo's rounded capacity.
    m_target->append_uninitialized(m_size);
    m_target = nullptr;
    m_capacity = m_size; // A completed writer must never modify published bytes.
    return {static_cast<uint32_t>(m_offset), static_cast<uint32_t>(m_size)};
  }
private:
  ByteBuffer* m_target = nullptr;
  uint8_t* m_data = nullptr;
  size_t m_offset = 0;
  size_t m_size = 0;
  size_t m_capacity = 0;
};

UniformWriter begin_uniform(uint32_t capacity);
} // namespace aurora::gfx
