#pragma once

#include "../internal.hpp"

namespace aurora::gx::fifo {

// Called at renderer lifecycle and direct test state resets.
void invalidate_pipeline_config() noexcept;

// Process a buffer of GX FIFO commands
void process(const uint8_t* data, uint32_t size, bool bigEndian);

// Already-processed FIFO only; use gfx::efb_depth_write_serial to drain first.
uint64_t efb_depth_write_serial() noexcept;

// [MP6] Retained replay (compat/aurora/windows/0006): build_uniform reports
// every position/normal matrix it copies into a draw's uniforms while a replay
// stream is being recorded, so a later replay can patch just those bytes.
extern bool g_retainRecording;
void retained_note_matrix(size_t uniformOffset, uint32_t slot, bool normal) noexcept;

} // namespace aurora::gx::fifo
