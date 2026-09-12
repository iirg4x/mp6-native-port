// Included inside aurora::gx::fifo, after the ordinary scalar writers.
// A failed reservation changes nothing. Fall back to the original field writes
// so short display-list buffers retain their existing partial-write semantics.
template<uint32_t Length>
inline uint8_t* reserve_packet() {
  if (!detail::sInDisplayList) LIKELY {
    if (detail::sBufferSize <= detail::sBufferCapacity &&
        Length <= detail::sBufferCapacity - detail::sBufferSize) LIKELY {
      auto* dst = detail::sBufferData + detail::sBufferSize;
      detail::sBufferSize += Length;
      return dst;
    }
  } else if (detail::sDlWritePos <= detail::sDlSize &&
             Length <= detail::sDlSize - detail::sDlWritePos) {
    auto* dst = detail::sDlBuffer + detail::sDlWritePos;
    detail::sDlWritePos += Length;
    return dst;
  }
  return nullptr;
}

inline void write_bp_packet(uint32_t value) {
  if (auto* dst = reserve_packet<5>()) {
    const auto out = bswap(value);
    dst[0] = 0x61;
    std::memcpy(dst + 1, &out, sizeof(out));
    return;
  }
  write_u8(0x61);
  write_u32(value);
}

inline void write_cp_packet(uint8_t opcode, uint8_t addr, uint32_t value) {
  if (auto* dst = reserve_packet<6>()) {
    const auto out = bswap(value);
    dst[0] = opcode;
    dst[1] = addr;
    std::memcpy(dst + 2, &out, sizeof(out));
    return;
  }
  write_u8(opcode);
  write_u8(addr);
  write_u32(value);
}

inline void write_xf_packet(uint32_t addr, uint32_t value) {
  if (auto* dst = reserve_packet<9>()) {
    const auto outAddr = bswap(uint32_t(0x1000 + addr));
    const auto outValue = bswap(value);
    dst[0] = 0x10;
    std::memcpy(dst + 1, &outAddr, sizeof(outAddr));
    std::memcpy(dst + 5, &outValue, sizeof(outValue));
    return;
  }
  write_u8(0x10);
  write_u32(0x1000 + addr);
  write_u32(value);
}
