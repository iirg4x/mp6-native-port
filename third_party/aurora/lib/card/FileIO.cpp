#include "FileIO.hpp"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../fs_helper.hpp"

namespace aurora::card {

FileIO::FileIO(const std::filesystem::path& filename, bool truncate) {
  m_path = fs_path_to_string(filename);;
  SDL_IOStream* stream = nullptr;

  if (truncate) {
    // Created (or emptied) by the first write -- see m_truncatePending.
    m_ready = !m_path.empty();
    m_truncatePending = true;
    return;
  }
  stream = fileOpen(m_path, "r+b");

  if (stream != nullptr) {
    SDL_FlushIO(stream);
    SDL_CloseIO(stream);
    m_ready = true;
  }
}

FileIO::FileIO(FileIO&& other) noexcept {
  m_path = std::move(other.m_path);
  m_ready = std::exchange(other.m_ready, false);
  m_truncatePending = std::exchange(other.m_truncatePending, false);
}

FileIO& FileIO::operator=(FileIO&& other) noexcept {
  if (this != &other) {
    m_path = std::move(other.m_path);
    m_ready = std::exchange(other.m_ready, false);
    m_truncatePending = std::exchange(other.m_truncatePending, false);
  }
  return *this;
}

bool FileIO::fileRead(void* buf, size_t length, off_t offset) {
  if (!isReady() || m_truncatePending) {
    return false;
  }

  SDL_IOStream* stream = fileOpen(m_path, "rb");
  if (stream == nullptr) {
    return false;
  }

  if (SDL_SeekIO(stream, offset, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(stream);
    return false;
  }

  size_t total = 0;
  auto* dst = static_cast<uint8_t*>(buf);
  while (total < length) {
    const size_t read = SDL_ReadIO(stream, dst + total, length - total);
    if (read == 0) {
      SDL_CloseIO(stream);
      return false;
    }
    total += read;
  }

  SDL_CloseIO(stream);
  return true;
}

// mp6: a save write replaces the whole file atomically. Writing in place
// (open, seek, write, close) left a torn .gci whenever the process died
// mid-write -- Android kills a backgrounded app without warning, and power
// can go at any time -- and the game then refuses the save. The file is
// small (MP6's save is 40 KB), so each write reads it, applies the change in
// memory, writes the result to <name>.tmp, makes it durable, and renames it
// over the original: a reader only ever sees the old file or the new one.
namespace {

#ifdef _WIN32
bool widen(const std::string& utf8, std::wstring& out) {
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
  if (n <= 0) {
    return false;
  }
  out.resize(static_cast<size_t>(n));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, out.data(), n) == n;
}

bool replaceFileAtomically(const std::string& path, const uint8_t* data, size_t size) {
  std::wstring wpath, wtmp;
  if (!widen(path, wpath) || !widen(path + ".tmp", wtmp)) {
    return false;
  }
  HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  size_t done = 0;
  bool ok = true;
  while (ok && done < size) {
    DWORD chunk = static_cast<DWORD>(size - done > 0x40000000u ? 0x40000000u : size - done);
    DWORD written = 0;
    ok = WriteFile(h, data + done, chunk, &written, nullptr) && written == chunk;
    done += written;
  }
  ok = ok && FlushFileBuffers(h);
  CloseHandle(h);
  // MOVEFILE_REPLACE_EXISTING replaces the old save in one step;
  // WRITE_THROUGH returns only once the rename is on disk.
  if (!ok || !MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(wtmp.c_str());
    return false;
  }
  return true;
}
#else
bool replaceFileAtomically(const std::string& path, const uint8_t* data, size_t size) {
  const std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  size_t done = 0;
  bool ok = true;
  while (ok && done < size) {
    const ssize_t n = write(fd, data + done, size - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    ok = n > 0;
    if (ok) {
      done += static_cast<size_t>(n);
    }
  }
  // fsync before the rename, so a crash cannot leave the new name pointing at
  // unwritten data. (The web build's in-memory and IndexedDB file systems
  // accept fsync as a no-op; IndexedDB persists on the page's next sync.)
  ok = ok && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  // And the directory, so the rename itself survives a power loss.
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
  const int dfd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dfd >= 0) {
    fsync(dfd);
    close(dfd);
  }
  return true;
}
#endif

} // namespace

bool FileIO::fileWrite(const void* buf, size_t length, off_t offset) {
  if (!isReady() || offset < 0) {
    return false;
  }
  std::vector<uint8_t> contents(m_truncatePending ? 0 : fileSize());
  if (!contents.empty() && !fileRead(contents.data(), contents.size(), 0)) {
    return false;
  }
  const size_t end = static_cast<size_t>(offset) + length;
  if (contents.size() < end) {
    contents.resize(end);
  }
  if (length != 0) {
    std::memcpy(contents.data() + offset, buf, length);
  }
  if (!replaceFileAtomically(m_path, contents.data(), contents.size())) {
    return false;
  }
  m_truncatePending = false;
  return true;
}

size_t FileIO::fileSize() const {
  if (m_truncatePending) {
    return 0;
  }
  SDL_PathInfo info;
  const auto tr = fs_path_to_string(m_path);
  if (SDL_GetPathInfo(m_path.c_str(), &info)) {
    return info.size;
  }
  return 0;
}

bool FileIO::deleteFile() {
  if (SDL_RemovePath(m_path.c_str())) {
    m_ready = false;
    m_path.clear();
    return true;
  }
  return false;
}

FileIO::operator bool() const { return isReady(); }

} // namespace aurora::card
