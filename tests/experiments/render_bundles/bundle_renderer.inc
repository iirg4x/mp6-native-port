// Private, worker-owned cache. Clear only after the worker has been joined.
static mp6_bundle_test::Cache<wgpu::RenderBundle> g_gxBundles;
static uint32_t g_bundleFrame = UINT32_MAX;
static uint64_t g_bundleFrames = 0, g_bundleHits = 0, g_bundleBuilds = 0;
static uint64_t g_bundleMisses = 0, g_bundleRejected = 0, g_bundleAllDraws = 0;

static void clear_gx_bundles() {
  g_gxBundles.clear();
  g_bundleFrame = UINT32_MAX;
  g_bundleFrames = g_bundleHits = g_bundleBuilds = g_bundleMisses = 0;
  g_bundleRejected = g_bundleAllDraws = 0;
}

static void bundle_frame(uint32_t frame) {
  if (g_bundleFrame == frame) return;
  if (g_bundleFrames && g_bundleFrames % 120 == 0) {
    Log.info("[GX-BUNDLES] frames={} draws={} hit_draws={} build_draws={} miss_draws={} rejected_draws={}",
             g_bundleFrames, g_bundleAllDraws, g_bundleHits, g_bundleBuilds,
             g_bundleMisses, g_bundleRejected);
  }
  ++g_bundleFrames;
  g_bundleFrame = frame;
  g_gxBundles.expire(frame);
}

static bool bundle_draw(const Command& cmd) {
  return cmd.type == CommandType::Draw && cmd.data.draw.type == ShaderType::GX &&
         cmd.data.draw.gx.dstAlpha == UINT32_MAX;
}

struct BundleResult { size_t count; bool executed; };
static BundleResult try_gx_bundle(const wgpu::RenderPassEncoder& pass, FramePacket& frame,
                           const RenderPass& passInfo, size_t begin) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  return {}; // Preserve per-command debug group semantics without folding them.
#else
  if (passInfo.bundleColorFormat == wgpu::TextureFormat::Undefined ||
      !bundle_draw(passInfo.commands[begin])) return {};
  size_t count = 0;
  while (begin + count < passInfo.commands.size() && count < mp6_bundle_test::MaxDraws &&
         bundle_draw(passInfo.commands[begin + count])) ++count;
  if (count < 4) return {};
  mp6_bundle_test::Key key{};
  key.staticGroup = reinterpret_cast<uintptr_t>(g_staticBindGroup.Get());
  key.uniformGroup = reinterpret_cast<uintptr_t>(g_uniformBindGroup.Get());
  key.indexBuffer = reinterpret_cast<uintptr_t>(g_indexBuffer.Get());
  key.emptyGroup = reinterpret_cast<uintptr_t>(gx::g_emptyTextureBindGroup.Get());
  key.colorFormat = static_cast<uint32_t>(passInfo.bundleColorFormat);
  key.depthFormat = static_cast<uint32_t>(passInfo.bundleDepthFormat);
  key.samples = passInfo.msaaSamples;
  key.readOnly = passInfo.depthReadOnly;
  key.count = static_cast<uint32_t>(count);
  for (size_t i = 0; i < count; ++i) {
    const auto& data = passInfo.commands[begin+i].data.draw.gx;
    key.draws[i] = {
      data.pipeline, data.bindGroups.textureBindGroup, data.uniformRange.offset,
      data.indexCount ? data.indexCount : data.vtxCount, data.instanceCount,
      data.indexCount ? data.idxRange.offset / uint32_t(sizeof(uint16_t)) : 0,
      data.indexCount ? 1u : 0u, 0u};
  }
  auto [entry, seen] = g_gxBundles.observe(key, frame.frameIndex, key.hash());
  if (!seen) { g_bundleMisses += count; return {count, false}; }
  if (!entry->payload) {
    // Resolve all pipelines first: an asynchronous miss must not produce a
    // partial bundle that later suppresses a now-ready draw.
    std::array<wgpu::RenderPipeline, mp6_bundle_test::MaxDraws> pipelines{};
    for (size_t i = 0; i < count; ++i) {
      if (!get_pipeline(key.draws[i].pipeline, pipelines[i])) {
        g_bundleRejected += count;
        return {count, false};
      }
    }
    const wgpu::RenderBundleEncoderDescriptor desc{
      .label = "MP6 private GX bundle", .colorFormatCount = 1,
      .colorFormats = &passInfo.bundleColorFormat,
      .depthStencilFormat = passInfo.bundleDepthFormat,
      .sampleCount = passInfo.msaaSamples, .depthReadOnly = passInfo.depthReadOnly,
      .stencilReadOnly = passInfo.depthReadOnly};
    auto encoder = g_device.CreateRenderBundleEncoder(&desc);
    encoder.SetBindGroup(0, g_staticBindGroup);
    encoder.SetBindGroup(2, gx::g_emptyTextureBindGroup);
    PipelineRef pipeline = UINTPTR_MAX;
    BindGroupRef textures = 0;
    bool indexed = false;
    for (size_t i = 0; i < count; ++i) {
      const auto& d = key.draws[i];
      if (d.pipeline != pipeline) { encoder.SetPipeline(pipelines[i]); pipeline = d.pipeline; }
      encoder.SetBindGroup(1, g_uniformBindGroup, 1, &d.uniformOffset);
      if (d.textures && d.textures != textures) {
        encoder.SetBindGroup(2, find_bind_group(d.textures));
        textures = d.textures;
      }
      if (d.indexed) {
        if (!indexed) {
          encoder.SetIndexBuffer(g_indexBuffer, wgpu::IndexFormat::Uint16, 0, IndexBufferSize);
          indexed = true;
        }
        encoder.DrawIndexed(d.count, d.instances, d.firstIndex);
      } else encoder.Draw(d.count, d.instances);
    }
    entry->payload = encoder.Finish();
    g_bundleBuilds += count;
  } else g_bundleHits += count;
  pass.ExecuteBundles(1, &entry->payload);
  return {count, true};
#endif
}
