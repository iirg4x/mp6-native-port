// Two recording-thread CPU snapshots keep comparisons off write-combined GPU
// upload memory. A growing instance group owns its own immutable template copy.
struct InstanceUniformSnapshot {
  ByteBuffer bytes;
  Range gpuRange;
  uint32_t frame = UINT32_MAX;
};
static std::array<InstanceUniformSnapshot, 2> g_instanceUniforms;
static uint32_t g_instanceUniformSlot = 0;

UniformWriter begin_instance_uniform(uint32_t capacity) {
  if (!check_recording("begin_instance_uniform")) return {};
  g_instanceUniformSlot ^= 1;
  auto& current = g_instanceUniforms[g_instanceUniformSlot];
  current.bytes.clear();
  current.frame = UINT32_MAX;
  return UniformWriter(current.bytes, capacity, 0);
}

Range finish_instance_uniform(Range initialized) {
  auto& current = g_instanceUniforms[g_instanceUniformSlot];
  CHECK(initialized.offset == 0 && initialized.size == current.bytes.size(),
        "Invalid CPU uniform snapshot");
  current.gpuRange = push_uniform(current.bytes.data(), current.bytes.size());
  current.frame = current_frame_packet().frameIndex;
  return current.gpuRange;
}

static std::span<const uint8_t> instance_uniform_bytes(Range range) {
  // Prefer the newest snapshot if a renderer restart reused frame/range numbers.
  for (uint32_t age = 0; age < 2; ++age) {
    const auto& snapshot = g_instanceUniforms[g_instanceUniformSlot ^ age];
    if (snapshot.frame == current_frame_packet().frameIndex &&
        snapshot.gpuRange.offset == range.offset && snapshot.gpuRange.size == range.size)
      return {snapshot.bytes.data(), snapshot.bytes.size()};
  }
  return {};
}
