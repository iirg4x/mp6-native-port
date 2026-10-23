#include "fifo.hpp"
#include "command_processor.hpp"
#include "gx.hpp" // aurora_gx_export_vtx_layout (drain-capture consumers seed their walker from live vtx state)
#include "../internal.hpp"

#include <cstdlib>
#include <cstring>

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

// Drain-capture sink (aurora_gx_set_drain_capture below): when set, every
// drained chunk is handed to the sink -- exact bytes, exact chunk framing --
// immediately BEFORE it is processed. Lets an application retain a frame's
// full command stream (all chunks) and replay it later via
// aurora_gx_submit_raw. Null (the default) keeps drain() bit-identical to
// its prior behavior.
static void (*sDrainCaptureFn)(const void* data, uint32_t size, void* user) = nullptr;
static void* sDrainCaptureUser = nullptr;

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void patch_u32(const uint32_t offset, const uint32_t val) {
  ASSERT(!detail::sInDisplayList && offset + sizeof(uint32_t) <= detail::sBufferSize,
         "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void drain() {
  if (detail::sBufferSize == 0) {
    return;
  }
  if (sDrainCaptureFn != nullptr) {
    sDrainCaptureFn(detail::sBufferData, detail::sBufferSize, sDrainCaptureUser);
  }
  process(detail::sBufferData, detail::sBufferSize, true);
  detail::sBufferSize = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() { detail::sBufferSize = 0; }

} // namespace aurora::gx::fifo

// ---------------------------------------------------------------------------
// C-linkage surface for retained-stream capture/replay (MP6 "Unlocked FPS"
// frame interpolation; aurora-patches/0015). Deliberately additive and
// self-contained in this TU: no header change, consumers declare these
// three prototypes themselves.
// ---------------------------------------------------------------------------
extern "C" {

// Register (or clear, fn=nullptr) the drain-capture sink. The sink observes
// every drained chunk verbatim before it is processed; it must not call back
// into the GX fifo.
void aurora_gx_set_drain_capture(void (*fn)(const void* data, uint32_t size, void* user), void* user) {
  aurora::gx::fifo::sDrainCaptureFn = fn;
  aurora::gx::fifo::sDrainCaptureUser = user;
}

// Append raw, already-encoded (big-endian) command-stream bytes to the fifo,
// exactly as if the corresponding GX* calls had written them. They are
// processed by the next drain (typically aurora_end_frame).
void aurora_gx_submit_raw(const void* data, uint32_t size) { aurora::gx::fifo::write_data(data, size); }

// Snapshot the live vertex-layout state (vertex descriptor + per-format
// attribute component types/counts) a command-stream walker needs to compute
// draw-command vertex strides. Covers attrs GX_VA_PNMTXIDX..GX_VA_TEX7 (21
// entries; the ones calculate_last_vtx_size sums). Call between frames --
// i.e. with the fifo drained -- so the snapshot is exactly the state the
// NEXT stream starts from.
void aurora_gx_export_vtx_layout(uint8_t* vtxDescOut /*21*/, uint8_t* vatCntOut /*8x21*/,
                                 uint8_t* vatTypeOut /*8x21*/) {
  using aurora::gx::g_gxState;
  for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
    vtxDescOut[attr] = static_cast<uint8_t>(g_gxState.vtxDesc[attr]);
    for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
      vatCntOut[fmt * 21 + attr] = static_cast<uint8_t>(g_gxState.vtxFmts[fmt].attrs[attr].cnt);
      vatTypeOut[fmt * 21 + attr] = static_cast<uint8_t>(g_gxState.vtxFmts[fmt].attrs[attr].type);
    }
  }
}

} // extern "C"
