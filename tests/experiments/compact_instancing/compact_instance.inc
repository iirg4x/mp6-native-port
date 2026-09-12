// Included in gfx/common.cpp after the mapped frame storage helpers.
// Only the immediately preceding GX command is eligible. Viewport/scissor,
// custom draws, EFB copies, pass changes and clears remain ordering barriers.
bool try_compact_instance(gx::DrawData& current, const gx::PipelineConfig& config,
                          const gx::ShaderInfo& info) {
  using namespace gx::compact_instance;
  if (!info.compactMatrices || info.lineMode || !current.mergeable || current.instanceCount != 1)
    return false;
  auto* previous = get_last_draw_command<gx::DrawData>();
  if (!previous) return false;

  // Recording-thread-only, valid only for the last command of this exact frame.
  // Compare the pointer; never dereference retained pointers from earlier passes.
  struct LastGroup {
    uint32_t frame = UINT32_MAX;
    gx::DrawData* draw = nullptr;
    PipelineRef basePipeline = 0;
    PipelineRef instancePipeline = 0;
    Range records;
    std::vector<uint8_t> templateUniform;
  };
  static LastGroup group;
  auto& frame = current_frame_packet();
  const bool extending = group.frame == frame.frameIndex && group.draw == previous &&
                         group.instancePipeline == previous->pipeline && !previous->mergeable &&
                         previous->instanceCount > 1 && previous->instanceCount < MaxInstances;
  if (!extending && (!previous->mergeable || previous->instanceCount != 1)) return false;
  if ((extending ? group.basePipeline : previous->pipeline) != current.pipeline ||
      previous->vtxFmt != current.vtxFmt || previous->dstAlpha != current.dstAlpha ||
      previous->bindGroups.textureBindGroup != current.bindGroups.textureBindGroup ||
      previous->vtxCount != current.vtxCount || previous->indexCount != current.indexCount ||
      previous->idxRange.size != current.idxRange.size) return false;

  const auto checked_span = [](const ByteBuffer& buffer, Range range) {
    CHECK(range.offset <= buffer.size() && range.size <= buffer.size() - range.offset,
          "Instance source range exceeds the recording buffer");
    return std::span<const uint8_t>(buffer.data() + range.offset, range.size);
  };
  const auto first = extending ? std::span<const uint8_t>(group.templateUniform) :
                                instance_uniform_bytes(previous->uniformRange);
  const auto next = instance_uniform_bytes(current.uniformRange);
  if (!compatible_uniforms(first, next, info.usesTexMtx.count())) return false;
  // Same topology, not necessarily the same geometry: each instance retains its
  // own direct-vertex and indexed-attribute addresses. No hashing / deduplication.
  const auto firstIndices = checked_span(g_instanceIndexShadow, previous->idxRange);
  const auto nextIndices = checked_span(g_instanceIndexShadow, current.idxRange);
  if (firstIndices.size() && std::memcmp(firstIndices.data(), nextIndices.data(), firstIndices.size()))
    return false;

  const auto nextRecord = pack_record(next, info.usesTexMtx.count());
  if (!extending) {
    auto instanceConfig = config;
    instanceConfig.shaderConfig.meshInstancing = true;
    gx::ShaderInfo instanceInfo{};
    const auto pipeline = find_gx_pipeline(instanceConfig, instanceInfo);
    wgpu::RenderPipeline ready;
    if (!get_pipeline(pipeline, ready)) return false; // Keep ordinary draws while compiling.
    CHECK(instanceInfo.uniformSize == info.uniformSize && instanceInfo.compactMatrices,
          "Instancing changed the uniform layout");
    std::array<Record, MaxInstances> records{};
    records[0] = pack_record(first, info.usesTexMtx.count());
    records[1] = nextRecord;
    const auto range = push_storage(reinterpret_cast<const uint8_t*>(records.data()), sizeof(records));
    group.frame = frame.frameIndex;
    group.draw = previous;
    group.basePipeline = current.pipeline;
    group.instancePipeline = pipeline;
    group.records = range;
    group.templateUniform.assign(first.begin(), first.end());
    // Mutate only the unused header padding; ordinary state comparison ignores it.
    std::memcpy(frame.uniforms.data() + previous->uniformRange.offset + 24, &range.offset, 4);
    previous->pipeline = pipeline;
    previous->instanceCount = 2;
    previous->mergeable = false; // Never append ordinary triangle indices to this group.
  } else {
    CHECK(group.records.size == MaxInstances * RecordBytes, "Invalid instance reservation");
    const auto offset = group.records.offset + previous->instanceCount * RecordBytes;
    CHECK(offset <= frame.storage.size() && RecordBytes <= frame.storage.size() - offset,
          "Instance destination exceeds the recording buffer");
    std::memcpy(frame.storage.data() + offset, nextRecord.data(), RecordBytes);
    ++previous->instanceCount;
  }
  ++g_mergedDrawCallCount;
  return true;
}
