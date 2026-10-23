#pragma once

#include <cstddef>
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {

/// Optional timestamps for a custom pass; call only on the render worker.
const wgpu::PassTimestampWrites* gpu_pass_timestamps(const char* label) noexcept;

inline constexpr size_t InlineDrawPayloadSize = 128;

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_draw_type.
using DrawTypeId = uint64_t;
inline constexpr DrawTypeId InvalidDrawType = 0;

struct Range {
  uint32_t offset = 0;
  uint32_t size = 0;

  bool operator==(const Range& rhs) const { return offset == rhs.offset && size == rhs.size; }
  bool operator!=(const Range& rhs) const { return !(*this == rhs); }
};

struct DrawContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::TextureFormat colorFormat;
  wgpu::TextureFormat depthFormat;
  uint32_t sampleCount = 1;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
};

/// Invoked on the render worker thread while replaying the pass the draw was
/// recorded into. The encoder's pipeline/bind-group/viewport/scissor state is
/// restored after the callback returns. Handles in the context are borrowed and
/// valid only for the duration of the call. sampleCount/target dimensions are
/// those of the containing pass (offscreen passes are always single-sample).
using DrawCallback = void (*)(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass,
                              const void* payload, size_t payloadSize, void* userdata);

struct DrawTypeDescriptor {
  const char* label = nullptr;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

wgpu::Device device() noexcept;
wgpu::Queue queue() noexcept;
/// Reuse immutable bindings; entries expire after inactivity or cache reset.
/// A returned handle keeps its resources alive independently of the cache.
wgpu::BindGroup cached_bind_group(const wgpu::BindGroupDescriptor& descriptor);
wgpu::TextureFormat color_format() noexcept;
wgpu::TextureFormat depth_format() noexcept;
uint32_t sample_count() noexcept;
bool uses_reversed_z() noexcept;
/// Port AO recovery policy, fixed by the selected adapter for this device.
bool ao_compatibility_mode() noexcept;

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc);
void unregister_draw_type(DrawTypeId type) noexcept;
/// Records an inline custom draw into the currently open render pass at the
/// current position in the command stream. Payload (<= InlineDrawPayloadSize)
/// is copied. Returns false (with a warning) outside an active render pass.
bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize);

// Custom render pipelines in the shared pipeline cache. A type is a pure
// function from a small POD key (no padding, no pointers) to a pipeline; the
// keys a scene uses are recorded and built ahead of use on later visits like
// GX pipelines, and a missing one is counted as a stall. create may run on a
// pipeline compile thread, so it must only use the key and objects it makes
// thread-safely itself. Bump version when create's output changes for a key.
using PipelineTypeId = uint64_t;
inline constexpr PipelineTypeId InvalidPipelineType = 0;
inline constexpr size_t MaxPipelineKeySize = 96;
using PipelineCreateCallback = wgpu::RenderPipeline (*)(const void* key, size_t keySize);
// Adapts a recorded key (from the bundled seed, recorded on another platform,
// or this device's own recording) to what this device would ask for: target
// formats, sample count, platform variants. Returns false to skip the key.
using PipelineReplayCallback = bool (*)(void* key, size_t keySize);
struct PipelineTypeDescriptor {
  const char* name = nullptr; // stable across runs: identifies saved keys
  uint32_t version = 0;
  PipelineCreateCallback create = nullptr;
  PipelineReplayCallback replay = nullptr; // optional
};
PipelineTypeId register_pipeline_type(const PipelineTypeDescriptor& desc);
// The pipeline for key, built now on this thread if nothing has built it yet.
wgpu::RenderPipeline custom_pipeline(PipelineTypeId type, const void* key, size_t keySize);

/// Generational handle: 0 is never valid, and IDs are not reused after unregister_encoder_task_type.
using EncoderTaskId = uint64_t;
inline constexpr EncoderTaskId InvalidEncoderTask = 0;

struct EncoderTaskContext {
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer storageBuffer;
  // Requested by readsDepth; valid only while encoding this task. Never retain
  // it for a later draw: the loading continuation will write the attachment.
  wgpu::TextureView depth;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
  uint32_t sampleCount = 1;
};

/// Invoked on the render worker thread with the frame's command encoder,
/// positioned between two render passes. The callback may begin/end compute
/// passes and record copies on the encoder; it must leave no pass open when it
/// returns and must not Finish the encoder. Handles in the context are borrowed
/// and valid only for the duration of the call. Data appended to the streaming
/// buffers before the task was pushed is GPU-visible inside it.
using EncoderTaskCallback = void (*)(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                                     const void* payload, size_t payloadSize, void* userdata);

struct EncoderTaskDescriptor {
  const char* label = nullptr;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  // Seal/store EFB depth and expose it to this ordered task, without a copy.
  // The task must consume depth before returning; this is not a snapshot API.
  bool readsDepth = false;
};

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc);
void unregister_encoder_task_type(EncoderTaskId type) noexcept;
/// Seals the current EFB pass, records an encoder task to execute on the frame
/// encoder at this point, and resumes rendering on a pass that loads the
/// existing contents. Payload semantics match push_custom_draw. Returns false
/// (with a warning) outside an active render pass or while an offscreen pass
/// is open.
bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize);

/// Append transient data to the shared per-frame streaming buffers. Returned
/// ranges are valid for the current frame only. Returns an empty Range (with a
/// warning) outside an active recording frame.
Range push_verts(const uint8_t* data, size_t length, size_t alignment);
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
Range push_uniform(const uint8_t* data, size_t length);
Range push_storage(const uint8_t* data, size_t length);

struct ResolveDesc {
  bool color = true;
  bool depth = false;
};

struct ResolvedTargets {
  wgpu::TextureView color;  // single-sample snapshot; null if not requested
  wgpu::TextureView depth;  // single-sample R32Float depth snapshot; null if not requested
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Snapshots the current pass targets into pooled textures (valid for the
/// current frame), then: on the EFB, continues rendering on a fresh EFB pass
/// (GXCopyTex semantics); in an offscreen pass created by create_pass, ends it
/// and restores the suspended EFB pass (GXRestoreFrameBuffer semantics).
/// Requesting neither color nor depth is a plain pass break (or offscreen
/// close, discarding its output). Depth is left null when unsupported by the
/// device. Returns false (with a warning) outside an active render pass.
bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out);

/// Game-thread serial of potential GX depth writes to the EFB (not offscreen
/// targets). Drains pending FIFO state without breaking the pass or waiting for
/// the GPU. Compare for inequality; merged draws also advance this serial.
/// Clears expressed as GX draws count too; callers must mark their clear boundary.
uint64_t efb_depth_write_serial();

/// Opens an offscreen render pass (GXCreateFrameBuffer semantics): cleared
/// single-sample color+depth at (width, height) with full-target
/// viewport/scissor. Subsequent draws target it until resolve_pass restores the
/// EFB. Nesting is unsupported: returns false (with a warning) outside an
/// active render pass or while any offscreen pass is already open.
/// Color is frame-owned and exported without a copy when this pass is resolved
/// directly. GXCreateFrameBuffer retains its separate reusable-target behavior.
bool create_pass(uint32_t width, uint32_t height);

struct BorrowedPassDepth {
  wgpu::TextureView view;
  uint32_t width = 0, height = 0, sampleCount = 0;
};

/// Seal EFB writes and open a same-size, cleared single-sample offscreen pass.
/// Its draws may sample the borrowed EFB depth attachment until resolve_pass
/// closes this offscreen pass. This is NOT a snapshot: never use the view in
/// a later EFB pass or retain it across frames. MSAA consumers must explicitly
/// select a sample. No EFB depth writes occur during this offscreen bracket.
/// Returns false with an empty output without opening a pass on invalid input.
bool create_pass_from_efb_depth(BorrowedPassDepth& out);

/// Seal EFB writes and open a same-size color-only coverage target using the
/// existing single-sample EFB depth attachment read-only. All draws MUST have
/// depth/stencil writes disabled. resolve_pass closes this bracket before EFB
/// writes resume. No depth copy or private depth allocation is needed.
/// MSAA/nested/inactive passes return false, with zero output dimensions.
bool create_pass_with_readonly_efb_depth(uint32_t& width, uint32_t& height);

/// True while an offscreen pass (create_pass or GXCreateFrameBuffer) is open.
bool is_offscreen() noexcept;

/// Blocks until the render worker has drained its queue. After this returns,
/// no draw callback is executing or queued to execute; used before unloading
/// code that registered draw types. Callable from the game thread only.
void synchronize();

/// Release a texture the caller is done with. In a browser it is destroyed a
/// few recorded frames later (a dropped GPUTexture otherwise keeps its memory
/// until the page's garbage collector runs, which Safari does late); elsewhere
/// the reference is just dropped. Safe for textures used by recorded frames.
void retire_texture(wgpu::Texture texture) noexcept;

/// Converted GX textures not drawn for this many seconds are dropped (0 or
/// less: the MP6_TEXTURE_IDLE_SECONDS default, 3). A host under memory
/// pressure can shorten it; it never lengthens the configured time.
void set_texture_idle_seconds(int seconds) noexcept;

} // namespace aurora::gfx
