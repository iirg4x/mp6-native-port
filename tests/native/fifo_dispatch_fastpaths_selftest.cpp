#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

static constexpr u8 GX_NOP = 0x00;
static constexpr u8 GX_DRAW_QUADS = 0x80;
static constexpr u8 GX_DRAW_TRIANGLES = 0x90;
static constexpr u8 GX_DRAW_TRIANGLE_STRIP = 0x98;
static constexpr u8 GX_DRAW_TRIANGLE_FAN = 0xA0;
static constexpr u8 GX_DRAW_LINES = 0xA8;
static constexpr u8 GX_DRAW_LINE_STRIP = 0xB0;
static constexpr u8 GX_DRAW_POINTS = 0xB8;
static constexpr u8 GX_LOAD_BP_REG = 0x61;
static constexpr u8 GX_LOAD_CP_REG = 0x08;
static constexpr u8 GX_LOAD_XF_REG = 0x10;
static constexpr u8 GX_LOAD_INDX_A = 0x20;
static constexpr u8 GX_LOAD_INDX_B = 0x28;
static constexpr u8 GX_LOAD_INDX_C = 0x30;
static constexpr u8 GX_LOAD_INDX_D = 0x38;
static constexpr u8 GX_CMD_CALL_DL = 0x40;
static constexpr u8 GX_CMD_INVL_VC = 0x48;
static constexpr u8 GX_AURORA = 0x50;
static constexpr u8 GX_OPCODE_MASK = 0xF8;
static constexpr u8 GX_VAT_MASK = 0x07;
static constexpr u32 MaxTextures = 8;

static constexpr u8 CP_CMD_NOP = GX_NOP;
static constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
static constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
static constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
static constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
static constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
static constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
static constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
static constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
static constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;
static constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
static constexpr u32 GX_POS_MTX_ARRAY = 0;

struct ProbeFailure final : std::exception {
  std::string message;
  u32 pos;

  ProbeFailure(std::string value, u32 where) : message(std::move(value)), pos(where) {}
  const char* what() const noexcept override { return message.c_str(); }
};

struct Trace {
  std::vector<std::string> events;
};
static Trace* g_trace = nullptr;

static void note(std::string value) {
  if (g_trace != nullptr) g_trace->events.push_back(std::move(value));
}

static void fail(const char* message, u32 pos) {
  throw ProbeFailure{std::string(message), pos};
}

#define ZoneScoped (void)0
#define ZoneScopedN(...) (void)0
#define CHECK(condition, message, ...) do { if (!(condition)) fail(message, pos); } while (0)
#define FATAL(...) do { fail("fatal", pos); } while (0)

namespace fmt {
template <class... Args>
std::string format(const char*, Args&&...) {
  return "formatted";
}
}  // namespace fmt

struct Logger {
  template <class... Args>
  void warn(const char*, Args&&...) { note("warn"); }
  template <class... Args>
  void error(const char*, Args&&...) { note("error"); }
  template <class... Args>
  void debug(const char*, Args&&...) { note("debug"); }
} Log;

static u16 bswap(u16 value) {
  return static_cast<u16>((value >> 8) | (value << 8));
}

static u32 bswap(u32 value) {
  return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
         ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

static u16 read_u16(const u8* ptr, bool bigEndian) {
  u16 value;
  std::memcpy(&value, ptr, sizeof(value));
  return bigEndian ? bswap(value) : value;
}

static u32 read_u32(const u8* ptr, bool bigEndian) {
  u32 value;
  std::memcpy(&value, ptr, sizeof(value));
  return bigEndian ? bswap(value) : value;
}

struct ArrayStub {
  u8* data;
  u32 stride;
};
struct StateStub {
  std::array<ArrayStub, 16> arrays{};
} g_gxState;

static void invalidate_vertex_cache() {
  note("invalidate");
}

static bool copy_xf_data(u16 dst, const u8*, u16 len, bool bigEndian) {
  note("index:" + std::to_string(dst) + ":" + std::to_string(len) + ":" +
       std::to_string(bigEndian));
  return true;
}

static void handle_bp(u32 value, bool bigEndian) {
  note("bp:" + std::to_string(value) + ":" + std::to_string(bigEndian));
}

static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  note("cp:" + std::to_string(addr) + ":" + std::to_string(value) + ":" +
       std::to_string(bigEndian));
}

static void handle_xf(const u8*, u32& pos, u32 size, bool bigEndian) {
  const u32 before = pos;
  if (size - pos < 4) fail("stub XF read overrun", pos);
  pos += 4;
  note("xf:" + std::to_string(before) + ":" + std::to_string(pos) + ":" +
       std::to_string(bigEndian));
}

static void handle_draw(u8 cmd, const u8*, u32& pos, u32 size, bool bigEndian) {
  const u32 before = pos;
  if (pos < size) ++pos;
  note("draw:" + std::to_string(cmd) + ":" + std::to_string(before) + ":" +
       std::to_string(pos) + ":" + std::to_string(bigEndian));
}

static void handle_aurora(const u8*, u32& pos, u32 size, bool bigEndian) {
  const u32 before = pos;
  if (size - pos < 2) fail("stub Aurora read overrun", pos);
  pos += 2;
  note("aurora:" + std::to_string(before) + ":" + std::to_string(pos) + ":" +
       std::to_string(bigEndian));
}

// Retained Unlocked FPS replay recording (third_party/aurora) is never
// active in this fixture; the dispatcher only consults the flag.
static bool g_retainRecording = false;
static void retain_note_indexed(u32) {}

// The Python unittest writes these two includes from Aurora's current source
// (third_party/aurora) and the frozen pre-fast-path reference.  Keeping the
// fixture generic makes the test prove the production extraction rather than
// a second hand-written dispatcher.
#include "dispatch.inc"
#include "bp.inc"

using ProcessFn = void (*)(const u8*, u32, bool);

struct Outcome {
  bool ok = true;
  std::string error;
  u32 error_pos = 0;
  std::vector<std::string> events;
};

static Outcome run_one(ProcessFn fn, const u8* data, u32 size, bool bigEndian) {
  Trace trace;
  g_trace = &trace;
  Outcome result;
  try {
    fn(data, size, bigEndian);
  } catch (const ProbeFailure& error) {
    result.ok = false;
    result.error = error.message;
    result.error_pos = error.pos;
  } catch (const std::exception& error) {
    result.ok = false;
    result.error = std::string("unexpected:") + error.what();
  }
  g_trace = nullptr;
  result.events = std::move(trace.events);
  return result;
}

static bool same(const Outcome& lhs, const Outcome& rhs) {
  return lhs.ok == rhs.ok && lhs.error == rhs.error &&
         lhs.error_pos == rhs.error_pos && lhs.events == rhs.events;
}

static std::uint64_t process_cases = 0;

static void pair_case(const char* label, const std::vector<u8>& bytes, bool bigEndian) {
  const Outcome original = run_one(process_original, bytes.data(),
                                   static_cast<u32>(bytes.size()), bigEndian);
  const Outcome candidate = run_one(process_candidate, bytes.data(),
                                    static_cast<u32>(bytes.size()), bigEndian);
  ++process_cases;
  if (!same(original, candidate)) {
    std::printf("FAIL %s size=%u endian=%u\n", label,
                static_cast<unsigned>(bytes.size()), static_cast<unsigned>(bigEndian));
    std::printf("original ok=%u error=%s pos=%u events=%zu\n", original.ok,
                original.error.c_str(), original.error_pos, original.events.size());
    std::printf("candidate ok=%u error=%s pos=%u events=%zu\n", candidate.ok,
                candidate.error.c_str(), candidate.error_pos, candidate.events.size());
    std::exit(1);
  }
}

static void pair_case_ptr(const char* label, const u8* data, u32 size, bool bigEndian) {
  std::vector<u8> bytes(data, data + size);
  pair_case(label, bytes, bigEndian);
}

static void append_tail(std::vector<u8>& bytes, u8 cmd) {
  bytes.push_back(cmd);
  switch (cmd & CP_OPCODE_MASK) {
  case CP_CMD_LOAD_BP_REG:
    bytes.insert(bytes.end(), {0, 1, 2, 3});
    break;
  case CP_CMD_LOAD_CP_REG:
    bytes.insert(bytes.end(), {0x50, 4, 5, 6, 7});
    break;
  case CP_CMD_LOAD_XF_REG:
    bytes.insert(bytes.end(), {8, 9, 10, 11});
    break;
  case CP_CMD_LOAD_INDX_A:
    bytes.insert(bytes.end(), {0, 0, 0, 0});
    break;
  case CP_CMD_CALL_DL:
    bytes.insert(bytes.end(), 8, 0);
    break;
  case GX_AURORA:
    bytes.insert(bytes.end(), {0, 1});
    break;
  case GX_DRAW_TRIANGLES:
    bytes.push_back(0);
    break;
  default:
    if (cmd >= 0x80) bytes.push_back(0);
    break;
  }
}

static void run_process_tests() {
  std::array<u8, 256> storage{};
  for (auto& array : g_gxState.arrays) {
    array.data = storage.data();
    array.stride = 1;
  }

  const std::array<u8, 8> nop_values{0, 1, 2, 3, 4, 5, 6, 7};
  const std::array<u8, 8> tails{GX_LOAD_BP_REG, GX_LOAD_CP_REG, GX_LOAD_XF_REG,
                                GX_LOAD_INDX_A, GX_CMD_CALL_DL, GX_CMD_INVL_VC,
                                GX_AURORA, GX_DRAW_TRIANGLES};
  for (u8 nop : nop_values) {
    for (unsigned count = 0; count <= 96; ++count) {
      for (u8 tail : tails) {
        std::vector<u8> bytes(count, nop);
        append_tail(bytes, tail);
        pair_case("nop-lead", bytes, false);
        pair_case("nop-lead-be", bytes, true);
      }
    }
  }

  for (unsigned count = 0; count <= 4096; ++count) {
    std::vector<u8> bytes;
    bytes.reserve(count);
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<u8>(i & 7));
    pair_case("all-nop", bytes, false);
  }

  const std::vector<u8> mixed{7, 2, GX_LOAD_BP_REG, 0, 1, 2, 3, 6, GX_LOAD_CP_REG,
                              0x50, 7, 6, 5, 4, 1, GX_CMD_CALL_DL, 0, 1, 2, 3,
                              4, 5, 6, 7, 3, GX_CMD_INVL_VC, 5, GX_AURORA, 0, 1,
                              0, GX_DRAW_TRIANGLES, 0};
  pair_case("mixed", mixed, false);
  pair_case("mixed-be", mixed, true);

  const std::vector<u8> truncation{3, GX_LOAD_BP_REG, 0, 1, 2, 3, 4, GX_LOAD_CP_REG,
                                   0x50, 5, 6, 7, 0, GX_CMD_CALL_DL, 0, 1, 2, 3,
                                   4, 5, 6, 7};
  for (unsigned size = 0; size <= truncation.size(); ++size) {
    std::vector<u8> bytes(truncation.begin(), truncation.begin() + size);
    pair_case("truncated", bytes, false);
    pair_case("truncated-be", bytes, true);
  }

  const std::vector<u8> no_nop{GX_LOAD_BP_REG, 0, 1, 2, 3, GX_LOAD_CP_REG,
                               0x50, 9, 8, 7, 6, GX_CMD_INVL_VC};
  pair_case("no-nop", no_nop, false);
  pair_case("no-nop-be", no_nop, true);

  std::vector<u8> aligned(512, 0);
  for (unsigned offset = 0; offset < 32; ++offset) {
    for (unsigned size = 0; size <= 160; ++size) {
      for (unsigned i = 0; i < size; ++i) aligned[offset + i] = static_cast<u8>(i & 7);
      if (size > 4) aligned[offset + size - 4] = GX_LOAD_BP_REG;
      pair_case_ptr("unaligned", aligned.data() + offset, size, (offset & 1) != 0);
    }
  }

#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const SIZE_T page = info.dwPageSize;
  auto* pages = static_cast<u8*>(VirtualAlloc(nullptr, page * 2,
                                               MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE));
  if (pages == nullptr) {
    std::printf("FAIL VirtualAlloc\n");
    std::exit(1);
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(pages + page, page, PAGE_NOACCESS, &old_protect)) {
    std::printf("FAIL VirtualProtect\n");
    std::exit(1);
  }
  std::memset(pages, 0, page);
  for (unsigned size = 0; size <= 128; ++size) {
    pair_case_ptr("guarded-end", pages + page - size, size, false);
  }
  if (!VirtualFree(pages, 0, MEM_RELEASE)) {
    std::printf("FAIL VirtualFree\n");
    std::exit(1);
  }
#else
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* pages = static_cast<u8*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(pages != MAP_FAILED);
  assert(mprotect(pages + page, page, PROT_NONE) == 0);
  std::memset(pages, 0, page);
  for (unsigned size = 0; size <= 128; ++size) {
    pair_case_ptr("guarded-end", pages + page - size, size, false);
  }
  assert(munmap(pages, page * 2) == 0);
#endif
}

static bool same_mapping(const std::optional<TexBpRegMapping>& lhs,
                         const std::optional<TexBpRegMapping>& rhs) {
  if (lhs.has_value() != rhs.has_value()) return false;
  return !lhs.has_value() ||
         (lhs->texMapId == rhs->texMapId && lhs->kind == rhs->kind);
}

static void run_bp_tests() {
  std::uint64_t compared = 0;
  std::uint64_t valid = 0;
  auto check_one = [&](u32 reg_id, bool count_valid) {
    const auto original = decode_original(reg_id);
    const auto candidate = decode_candidate(reg_id);
    ++compared;
    if (count_valid && original.has_value()) ++valid;
    if (!same_mapping(original, candidate)) {
      std::printf("FAIL BP reg=0x%08X original=%u candidate=%u\n", reg_id,
                  original.has_value() ? 1u : 0u, candidate.has_value() ? 1u : 0u);
      std::exit(1);
    }
  };

  for (u32 reg_id = 0; reg_id <= 0xFFFFu; ++reg_id) check_one(reg_id, true);
  constexpr std::array<u32, 22> boundaries{
      0u,          1u,       0x7Eu, 0x7Fu, 0x80u, 0x83u,
      0x84u,       0x9Au,   0x9Bu, 0x9Cu, 0x9Fu, 0xA0u,
      0xA3u,       0xA4u,   0xBAu, 0xBBu, 0xBCu, 0xFFu,
      0x100u,      0xFFFFu, 0x10000u, std::numeric_limits<u32>::max()};
  for (u32 reg_id : boundaries) check_one(reg_id, false);
  if (valid != 56u) {
    std::printf("FAIL BP valid_count=%llu expected=56\n",
                static_cast<unsigned long long>(valid));
    std::exit(1);
  }
  if (compared != 65558u) {
    std::printf("FAIL BP compared=%llu expected=65558\n",
                static_cast<unsigned long long>(compared));
    std::exit(1);
  }
}

int main() {
  run_process_tests();
  run_bp_tests();
  std::printf("PASS FIFO process cases=%llu BP mapping cases=65558 valid=56\n",
              static_cast<unsigned long long>(process_cases));
  if (process_cases != 21844u) return 1;
  return 0;
}
