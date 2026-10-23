#pragma once

#include <cstddef>
#include <string>
#include <filesystem>

#include <SDL3/SDL_iostream.h>

#ifdef __ANDROID__
#include <cstdio> // mp6 patch 0011 (fileOpen below): fopen pre-flight
#endif

namespace aurora::card {

class FileIO {
  std::string m_path;
  bool m_ready = false;
  // mp6: a truncating open no longer empties the file on disk; the next
  // write replaces it whole (atomically), so a crash in between leaves the
  // old file rather than an empty one.
  bool m_truncatePending = false;

  bool isReady() const { return m_ready && !m_path.empty(); };

public:
  FileIO() = default;
  explicit FileIO(const std::filesystem::path& filename, bool truncate = false);
  ~FileIO() = default;

  FileIO(FileIO&& other) noexcept;
  FileIO& operator=(FileIO&& other) noexcept;
  FileIO(const FileIO& other) = delete;
  FileIO& operator=(const FileIO& other) = delete;

#ifdef __ANDROID__
  // mp6 patch 0011: never let a card open reach SDL's Android asset fallback.
  // SDL_IOFromFile falls back to a JNI AssetManager lookup whenever plain
  // fopen fails (e.g. a .gci that simply does not exist yet) -- but games
  // call the CARD API from coroutine contexts whose stacks live outside the
  // ART-registered thread stack, and ART's JNI-entry stack-bounds check then
  // throws a spurious java.lang.StackOverflowError that aborts the app at
  // the next JNI call (measured on device: CARDOpen of an absent save from a
  // game coroutine -> pending StackOverflowError -> CheckJNI abort in
  // SDL_UpdateJoysticks). Card paths are always real filesystem paths, never
  // APK assets, so the fallback is meaningless here: pre-flight with plain
  // fopen and only hand SDL a path its own internal fopen will succeed on.
  static SDL_IOStream* fileOpen(const std::string& path, const char* mode) {
    if (path.empty()) {
      return nullptr;
    }
    FILE* probe = fopen(path.c_str(), mode);
    if (probe == nullptr) {
      return nullptr;
    }
    fclose(probe);
    return SDL_IOFromFile(path.c_str(), mode);
  }
#else
  static SDL_IOStream* fileOpen(const std::string& path, const char* mode) {return path.empty() ? nullptr : SDL_IOFromFile(path.c_str(), mode);}
#endif
  bool fileRead(void* buf, size_t length, off_t offset);
  bool fileWrite(const void* buf, size_t length, off_t offset);
  size_t fileSize() const;
  bool deleteFile();
  explicit operator bool() const;
};

} // namespace aurora::card
