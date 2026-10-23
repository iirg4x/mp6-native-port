#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

/* Anti-Aliasing P2 (aurora-patches/0017): post-process AA applied in the
 * present resample pass over the resolved image. Live -- toggled at runtime
 * via aurora_set_post_aa without a restart (unlike AuroraConfig.msaa/ssaa,
 * which are aurora_initialize-time). */
typedef enum {
  AURORA_POST_AA_NONE,
  AURORA_POST_AA_FXAA,
} AuroraPostAA;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  /* Anti-Aliasing P3 (aurora-patches/0018): SSAA supersampling factor for the
   * content framebuffer (the surface stays native; aurora's present-resample
   * pass downsamples). 0 or <=1 = off/native (byte-identical). Read once at
   * aurora_initialize, so it is restart-pending like msaa. Desktop-only: the
   * scaling is compiled out under __ANDROID__ (the port never sends >1 there
   * either). Clamped to the device maxTextureDimension2D. */
  float ssaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowCpuAdapter;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
/* [MP6] Why the last aurora_begin_frame() returned false (static string). */
const char* aurora_begin_frame_fail_reason();
bool aurora_try_begin_frame();
/* Replay preflight: 1 ready, 0 resource pressure, -1 not presentable.
 * The timeout is surplus before the caller's absolute simulation deadline.
 * Does not reserve a frame, pump input, or start UI/GX recording. */
int aurora_wait_replay_ready(uint64_t maxWaitNs);
void aurora_end_frame();

void aurora_set_log_level(AuroraLogLevel level);
// Live, asynchronous GPU timestamps. Unsupported backends return status 0;
// status 1 is collecting, 2 is ready, 3 indicates a failed readback.
// Times are milliseconds over the last sampleCount completed frames.
// gpuSpan covers the first-to-last timed pass, not CPU/swapchain waits.
#define AURORA_GPU_STAT_ROWS 32
typedef struct {
  char name[64];
  double averageMs;
  double maxMs;
} AuroraGpuStatRow;
typedef struct {
  uint32_t status;
  uint32_t sampleCount;
  uint32_t rowCount;
  uint32_t droppedFrames;
  double spanAverageMs;
  double spanMaxMs;
  double betweenAverageMs;
  double betweenMaxMs;
  AuroraGpuStatRow rows[AURORA_GPU_STAT_ROWS];
} AuroraGpuStats;
void aurora_gpu_stats_set_enabled(bool enabled);
void aurora_gpu_stats_read(AuroraGpuStats* out);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_resampler(AuroraSampler sampler);
/* [MP6] Render scale (compat/aurora/windows/0007): content framebuffer at
 * scale x the window size, upscaled at present. 0.25..1; 1 = native. Call
 * between frames; the framebuffer is rebuilt immediately. */
void aurora_set_render_scale(float scale);
/* [MP6] Hidden frame (compat/aurora/windows/0008): call before the frame's
 * aurora_begin_frame()/aurora_try_begin_frame(). That frame is recorded as
 * usual (texture uploads, GX copies and every pass whose output is read run)
 * but render passes nothing reads are dropped and no surface image is
 * acquired or presented. One-shot; used by Unlocked FPS smooth mode, which
 * shows the real tick frame through the replay presented right after it. */
void aurora_hide_next_frame(void);
/* [MP6] 1 while the frame being recorded is a hidden one (see above). */
int aurora_frame_hidden(void);
/* [MP6] Present log for frame-pacing measurement. Every aurora_end_frame()
 * gets a sequence number (aurora_end_frame_seq() returns the last one). Once
 * aurora_present_log_drain() has been called, the render worker records
 * (seq, steady-clock ns) right after each successful surface present; drain
 * returns how many entries it wrote. Never called = nothing is recorded.
 * Native only: the web build has no surface present call to time, so its log
 * stays empty. */
uint64_t aurora_end_frame_seq(void);
int aurora_present_log_drain(uint64_t* seqs, int64_t* presentNs, int max);
/* Anti-Aliasing P2 (aurora-patches/0017): live post-process AA select (FXAA).
 * Clones aurora_set_resampler -- applies immediately, no restart. */
void aurora_set_post_aa(AuroraPostAA mode);
/* [MP6] The game released [begin, end): a heap block, or a whole heap being
 * set up again. Texture and palette conversions read from there are dropped
 * at the end of the frame. Any thread. */
void aurora_texture_sources_released(const void* begin, const void* end);
/* [MP6] The game wrote [begin, end) for the GPU (a DCFlushRange/DCStoreRange).
 * Texture and palette conversions read from there are checked against the
 * new bytes at their next draw. Any thread. */
void aurora_texture_sources_written(const void* begin, const void* end);

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

#ifdef __cplusplus
}
#endif

#endif
