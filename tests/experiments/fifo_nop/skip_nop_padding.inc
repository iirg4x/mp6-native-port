// Called only after the dispatcher has consumed a NOP command. GX ignores
// the low three opcode bits, so all bytes 0x00..0x07 are padding here. Keep
// the first real command for the normal decoder, including all payloads.
static u32 skip_nop_padding(const u8* data, u32 pos, u32 size) {
  while (size - pos >= sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, data + pos, sizeof(word));
    if (word & UINT64_C(0xf8f8f8f8f8f8f8f8)) break;
    pos += sizeof(word);
  }
  while (pos < size && (data[pos] & 0xf8) == 0) ++pos;
  return pos;
}
