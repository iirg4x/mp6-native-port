"""QA-only inline AO GPU timestamp source transforms.

The transforms in this module are applied only to the isolated inline-AO
benchmark output.  They deliberately operate on source text with strict
anchors so drift in the pinned Aurora source fails closed instead of silently
changing a production or a different QA binary.
"""


INLINE_AO_FEATURE = "ChromiumExperimentalTimestampQueryInsidePasses"


def replace_once(text, old, new):
    """Replace one source anchor and reject missing or drifting anchors."""

    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected one source anchor: {old[:100]!r}, got {count}")
    return text.replace(old, new, 1)


def instrument_gpu(text):
    """Request inline-pass timestamps only when the adapter exposes them.

    Dawn's regular TimestampQuery feature covers begin/end pass writes,
    while WriteTimestamp inside an already-open render pass additionally
    needs ChromiumExperimentalTimestampQueryInsidePasses.  The adapter's
    supported-feature enumeration is the source of truth: unsupported
    adapters do not receive the optional request and therefore keep the
    original device-creation path.
    """

    if "[MP6-INLINE-AO] feature=" in text:
        raise ValueError("GPU source is already inline-AO instrumented")

    # Dawn caches adapter features before RequestDevice. Experimental features
    # are filtered by the instance/adapter toggles, so the existing device-only
    # allow_unsafe_apis entry is too late for this supported-feature census.
    instance_anchor = "    g_instance = wgpu::CreateInstance(&instanceDescriptor);"
    instance_replacement = '''    const char *inlineAoInstanceToggleNames[] = {"allow_unsafe_apis"};
    wgpu::DawnTogglesDescriptor inlineAoInstanceToggles(wgpu::DawnTogglesDescriptor::Init{
        .nextInChain = nullptr,
        .enabledToggleCount = 1,
        .enabledToggles = inlineAoInstanceToggleNames,
    });
    inlineAoInstanceToggles.nextInChain = instanceDescriptor.nextInChain;
    instanceDescriptor.nextInChain = &inlineAoInstanceToggles;
    g_instance = wgpu::CreateInstance(&instanceDescriptor);'''
    text = replace_once(text, instance_anchor, instance_replacement)
    text = '''#ifdef TRACY_ENABLE
#error "Inline AO profiling requires the non-Tracy query ring"
#endif
''' + text

    feature_anchor = """      if (feature == wgpu::FeatureName::TimestampQuery) {
        requiredFeatures.push_back(feature);
      }
"""
    feature_replacement = """      if (feature == wgpu::FeatureName::TimestampQuery) {
        requiredFeatures.push_back(feature);
      }
      if (feature == wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses) {
        requiredFeatures.push_back(feature);
      }
"""
    text = replace_once(text, feature_anchor, feature_replacement)

    # Keep the report immediately after the adapter feature census and before
    # any optional feature/device descriptor work.  The feature was appended
    # only from the adapter's supported list above; no unsupported feature is
    # forced into requiredFeatures.
    feature_census_end = """      if (feature == wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses) {
        requiredFeatures.push_back(feature);
      }
    }
#ifdef WEBGPU_DAWN
"""
    feature_census_replacement = """      if (feature == wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses) {
        requiredFeatures.push_back(feature);
      }
    }
    const bool inlineAoTimestampsSupported = std::any_of(
        requiredFeatures.begin(), requiredFeatures.end(), [](wgpu::FeatureName feature) {
          return feature == wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses;
        });
    Log.info("[MP6-INLINE-AO] feature={}",
             inlineAoTimestampsSupported ? "supported" : "unsupported");
#ifdef WEBGPU_DAWN
"""
    text = replace_once(text, feature_census_end, feature_census_replacement)
    return text


INLINE_SPAN = r'''struct InlineAoTimestampSpan {
    const wgpu::RenderPassEncoder &pass;
    wgpu::PassTimestampWrites writes{};
    bool active = false;

    InlineAoTimestampSpan(const wgpu::Device &device,
                          const wgpu::RenderPassEncoder &pass,
                          const char *label)
        : pass(pass) {
        const bool supported =
            device.HasFeature(wgpu::FeatureName::ChromiumExperimentalTimestampQueryInsidePasses);
        static std::atomic_bool logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[MP6-INLINE-AO] scope=nested-efb timestamps=%s\n",
                         supported ? "supported" : "unsupported");
        }
        if (!supported) return;
        const wgpu::PassTimestampWrites *allocated = gfx::gpu_pass_timestamps(label);
        if (!allocated) return;
        // gpu_pass_timestamps returns a pointer to a shared static descriptor;
        // copy it before any later helper call can overwrite that storage.
        writes = *allocated;
        pass.WriteTimestamp(writes.querySet, writes.beginningOfPassWriteIndex);
        active = true;
    }

    ~InlineAoTimestampSpan() {
        if (active)
            pass.WriteTimestamp(writes.querySet, writes.endOfPassWriteIndex);
    }
};
'''


def instrument_ao(text):
    """Bracket the two nested-EFB fullscreen draws with optional timestamps."""

    if "InlineAoTimestampSpan" in text:
        raise ValueError("AO source is already inline-AO instrumented")

    helper_anchor = """uint64_t g_upstreamEncoded = 0;
uint64_t g_foliageSeeded=0;

void warn(const char *reason) {
"""
    helper_replacement = """uint64_t g_upstreamEncoded = 0;
uint64_t g_foliageSeeded=0;

""" + INLINE_SPAN + """
void warn(const char *reason) {
"""
    text = replace_once(text, helper_anchor, helper_replacement)

    composite_anchor = """    pass.SetBindGroup(0, bindings);
    pass.Draw(3);
"""
    composite_replacement = """    pass.SetBindGroup(0, bindings);
    {
        InlineAoTimestampSpan inlineAoSpan(ctx.device, pass, "AO composite inline");
        pass.Draw(3);
    }
"""
    text = replace_once(text, composite_anchor, composite_replacement)

    foliage_anchor = """    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings); pass.Draw(3);
"""
    foliage_replacement = """    pass.SetPipeline(found->pipeline); pass.SetBindGroup(0,bindings);
    {
        InlineAoTimestampSpan inlineAoSpan(ctx.device, pass, "AO foliage seed inline");
        pass.Draw(3);
    }
"""
    text = replace_once(text, foliage_anchor, foliage_replacement)
    return text


__all__ = ["INLINE_AO_FEATURE", "INLINE_SPAN", "instrument_ao", "instrument_gpu", "replace_once"]
