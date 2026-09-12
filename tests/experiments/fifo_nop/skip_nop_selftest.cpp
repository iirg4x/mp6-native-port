#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
using u8 = uint8_t;
using u32 = uint32_t;
#include "skip_nop_padding.inc"
static uint64_t cases = 0;
static void check(const u8* data, u32 pos, u32 size) {
  u32 expected = pos;
  while (expected < size && (data[expected] & 0xf8) == 0) ++expected;
  assert(skip_nop_padding(data, pos, size) == expected);
  ++cases;
}
int main() {
  u8 data[8192];
  for (u32 i = 0; i < sizeof(data); ++i) data[i] = i & 7;
  for (u32 offset = 0; offset < 32; ++offset) {
    for (u32 len = 0; len < 257; ++len) {
      for (u32 next = 8; next <= 255; ++next) {
        data[offset + len] = next;
        check(data + offset, 0, len + 1);
        data[offset + len] = (offset + len) & 7;
      }
      check(data + offset, 0, len);
    }
  }
  for (u32 size = 0; size <= 4096; ++size) {
    check(data, 0, size);
    check(data, size, size);
    check(data, size / 2, size);
  }
  // Guarded final page: zero-length, short and word reads must never cross size.
#ifdef _WIN32
  SYSTEM_INFO info; GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  auto* pages = static_cast<u8*>(VirtualAlloc(nullptr, 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  assert(pages);
  DWORD oldProtect;
  assert(VirtualProtect(pages + page, page, PAGE_NOACCESS, &oldProtect));
#else
  const size_t page = sysconf(_SC_PAGESIZE);
  auto* pages = static_cast<u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(pages != MAP_FAILED);
  assert(mprotect(pages + page, page, PROT_NONE) == 0);
#endif
  std::memset(pages, 0, page);
  for (u32 size = 0; size <= 128; ++size) check(pages + page - size, 0, size);
#ifdef _WIN32
  assert(VirtualFree(pages, 0, MEM_RELEASE));
#else
  assert(munmap(pages, 2 * page) == 0);
#endif
  std::printf("PASS: %llu NOP byte-oracle and guarded-boundary cases\n", (unsigned long long)cases);
}
