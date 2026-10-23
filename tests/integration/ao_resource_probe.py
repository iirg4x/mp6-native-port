"""Source-bound, opt-in AO resource census for board QA.

The transform is deliberately diagnostic-only.  Version 2 estimates requested
texture texel storage for per-job legacy/clean-depth resources plus unique
shared XeGTAO ``Scratch`` identities leased by the pipeline and 64 AO ``Job``
slots; it does not query driver allocations and excludes caches, views,
borrowed snapshots, pipelines, and the shared XeGTAO LUT.  Version 1 parsing
remains available for historical job-owned records.  Production source files
are never written.
"""

from __future__ import annotations

import json
import re
from typing import Any


MARKER = "MP6_QA_AO_RESOURCES"
RESOURCE_ENV = "MP6_QA_AO_RESOURCES"
V1_ESTIMATE_SCOPE = (
    "requested_texture_texel_bytes_only;excludes_caches/views/borrowed_snapshots/"
    "pipelines/shared_lut"
)
ESTIMATE_SCOPE = (
    "requested_texture_texel_bytes_only;shared_scratch_identity_dedup;"
    "per_job_clean_depth_legacy;excludes_caches/views/borrowed_snapshots/"
    "pipelines/shared_lut"
)
V2_PROVENANCE = "shared_scratch_identity_dedup;pipeline_retained_plus_job_leases"
MAX_CHANGE_LOGS = 64
JOB_SLOTS = 64
MAX_SCRATCH_REFERENCES = JOB_SLOTS + 1

JOB_ARRAY_ANCHOR = "std::array<Job, 64> g_jobs;"
FRAME_MEMBER_ANCHOR = "    uint64_t frame = UINT64_MAX; /* game-thread only */"
FRAME_ASSIGN_ANCHOR = "    job->frame = g_frame;"
ENCODE_END_ANCHOR = "    ++g_encoded;\n}"
SHUTDOWN_SYNC_ANCHOR = "    if (initialized) gfx::synchronize();"
PIPELINE_DECL_ANCHOR = "mp6::xegtao::Pipeline g_xegtao;"


RESOURCE_HEADER = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace mp6_ao_resource_probe {
inline constexpr uint64_t kJobSlots = 64;
inline constexpr uint64_t kFrameBuckets = 128;
inline constexpr uint64_t kMaxChangeLogs = 64;
inline constexpr uint64_t kMaxScratchReferences = 65;
inline constexpr const char *kEstimateScope =
    "requested_texture_texel_bytes_only;shared_scratch_identity_dedup;per_job_clean_depth_legacy;excludes_caches/views/borrowed_snapshots/pipelines/shared_lut";
inline constexpr const char *kProvenance =
    "shared_scratch_identity_dedup;pipeline_retained_plus_job_leases";

struct Census {
    uint64_t legacyTextures = 0;
    uint64_t cleanDepth = 0;
    uint64_t upstreamRawDepth = 0;
    uint64_t upstreamDepthMips = 0;
    uint64_t upstreamNormals = 0;
    uint64_t upstreamAo0 = 0;
    uint64_t legacySlots = 0;
    uint64_t upstreamBundles = 0;
    uint64_t upstreamBundleRefs = 0;
    uint64_t upstreamLeaseSlots = 0;
    uint64_t cleanSlots = 0;
    uint64_t retainedSlots = 0;
    uint64_t legacyTextureBytes = 0;
    uint64_t cleanDepthBytes = 0;
    uint64_t upstreamTextureBytes = 0;
    uint64_t retainedTextureBytes = 0;
    uint64_t uniformBytes = 0;
    // Dimensions are the actual AO attachment/working extents observed by
    // this encode, not the commanded window size.  They remain in the last
    // census so the shutdown row is still interpretable after the callback.
    uint64_t targetWidth = 0;
    uint64_t targetHeight = 0;
    uint64_t jobWidth = 0;
    uint64_t jobHeight = 0;
    uint64_t fullWidth = 0;
    uint64_t fullHeight = 0;
    uint64_t upstreamWidth = 0;
    uint64_t upstreamHeight = 0;
    std::array<const void *, kMaxScratchReferences> scratchIdentities{};
    uint64_t scratchIdentityCount = 0;
};

struct FrameCount {
    uint64_t frame = UINT64_MAX;
    uint64_t count = 0;
};

struct State {
    bool havePrevious = false;
    bool finalEmitted = false;
    Census previous{};
    uint64_t currentFrame = UINT64_MAX;
    uint64_t encodesInFrame = 0;
    uint64_t peakSlots = 0;
    uint64_t peakRetainedTextureBytes = 0;
    uint64_t maxEncodesPerFrame = 0;
    uint64_t changeLogs = 0;
    std::array<FrameCount, kFrameBuckets> frameCounts{};
};

inline State state{};

// Read the opt-in once.  A later environment mutation cannot turn profiling
// on or off while render-worker callbacks are in flight.
inline bool enabled() {
    static const bool latched = [] {
        const char *value = std::getenv("MP6_QA_AO_RESOURCES");
        return value && std::strcmp(value, "1") == 0;
    }();
    return latched;
}

inline uint64_t mip_extent(uint32_t dimension, uint32_t level) {
    const uint64_t shifted = uint64_t(dimension) >> level;
    return shifted ? shifted : 1;
}

inline uint64_t requested_texture_bytes(uint32_t width, uint32_t height,
                                        uint32_t levels, uint32_t bytesPerTexel) {
    uint64_t total = 0;
    for (uint32_t level = 0; level < levels; ++level)
        total += mip_extent(width, level) * mip_extent(height, level) * bytesPerTexel;
    return total;
}

inline void add_texture(bool present, uint32_t width, uint32_t height,
                        uint32_t levels, uint32_t bytesPerTexel,
                        uint64_t &count, uint64_t &bytes) {
    if (!present) return;
    ++count;
    bytes += requested_texture_bytes(width, height, levels, bytesPerTexel);
}

inline void observe_job(Census &census,
                        bool legacy0, bool legacy1, uint32_t legacyWidth, uint32_t legacyHeight,
                        bool clean, uint32_t cleanWidth, uint32_t cleanHeight,
                        bool uniform, uint64_t uniformBytes,
                        bool constants, uint64_t constantsBytes,
                        bool adapter, uint64_t adapterBytes,
                        bool scratch) {
    add_texture(legacy0, legacyWidth, legacyHeight, 1, 8,
                census.legacyTextures, census.legacyTextureBytes);
    add_texture(legacy1, legacyWidth, legacyHeight, 1, 8,
                census.legacyTextures, census.legacyTextureBytes);
    add_texture(clean, cleanWidth, cleanHeight, 1, 4,
                census.cleanDepth, census.cleanDepthBytes);

    const bool hasLegacy = legacy0 || legacy1;
    const bool hasClean = clean;
    if (hasLegacy) ++census.legacySlots;
    if (scratch) ++census.upstreamLeaseSlots;
    if (hasClean) ++census.cleanSlots;
    if (hasLegacy || scratch || hasClean) ++census.retainedSlots;
    if (uniform) census.uniformBytes += uniformBytes;
    if (constants) census.uniformBytes += constantsBytes;
    if (adapter) census.uniformBytes += adapterBytes;
}

inline void observe_scratch(Census &census, const void *identity,
                            uint32_t width, uint32_t height, bool depthHalf,
                            bool rawDepth, bool depthMips, bool normals, bool ao0) {
    if (!identity) return;
    ++census.upstreamBundleRefs;
    for (uint64_t i = 0; i < census.scratchIdentityCount; ++i)
        if (census.scratchIdentities[i] == identity) return;
    if (census.scratchIdentityCount >= kMaxScratchReferences) return;
    census.scratchIdentities[census.scratchIdentityCount++] = identity;
    ++census.upstreamBundles;
    const uint32_t depthBytes = depthHalf ? 2u : 4u;
    add_texture(rawDepth, width, height, 1, 4,
                census.upstreamRawDepth, census.upstreamTextureBytes);
    add_texture(depthMips, width, height, 5, depthBytes,
                census.upstreamDepthMips, census.upstreamTextureBytes);
    add_texture(normals, width, height, 1, 4,
                census.upstreamNormals, census.upstreamTextureBytes);
    add_texture(ao0, width, height, 1, 4,
                census.upstreamAo0, census.upstreamTextureBytes);
}

inline bool same_census(const Census &left, const Census &right) {
    return left.legacyTextures == right.legacyTextures &&
           left.cleanDepth == right.cleanDepth &&
           left.upstreamRawDepth == right.upstreamRawDepth &&
           left.upstreamDepthMips == right.upstreamDepthMips &&
           left.upstreamNormals == right.upstreamNormals &&
           left.upstreamAo0 == right.upstreamAo0 &&
           left.legacySlots == right.legacySlots &&
           left.upstreamBundles == right.upstreamBundles &&
           left.upstreamBundleRefs == right.upstreamBundleRefs &&
           left.upstreamLeaseSlots == right.upstreamLeaseSlots &&
           left.cleanSlots == right.cleanSlots &&
           left.retainedSlots == right.retainedSlots &&
           left.legacyTextureBytes == right.legacyTextureBytes &&
           left.cleanDepthBytes == right.cleanDepthBytes &&
           left.upstreamTextureBytes == right.upstreamTextureBytes &&
           left.retainedTextureBytes == right.retainedTextureBytes &&
           left.uniformBytes == right.uniformBytes &&
           left.targetWidth == right.targetWidth && left.targetHeight == right.targetHeight &&
           left.jobWidth == right.jobWidth && left.jobHeight == right.jobHeight &&
           left.fullWidth == right.fullWidth && left.fullHeight == right.fullHeight &&
           left.upstreamWidth == right.upstreamWidth && left.upstreamHeight == right.upstreamHeight &&
           left.scratchIdentityCount == right.scratchIdentityCount &&
           std::equal(left.scratchIdentities.begin(), left.scratchIdentities.begin() + left.scratchIdentityCount,
                      right.scratchIdentities.begin());
}

inline void note_frame(uint64_t frame) {
    auto &entry = state.frameCounts[frame % kFrameBuckets];
    if (entry.frame != frame) entry = {frame, 0};
    ++entry.count;
    state.currentFrame = frame;
    state.encodesInFrame = entry.count;
    if (entry.count > state.maxEncodesPerFrame) state.maxEncodesPerFrame = entry.count;
}

inline void emit_record(const char *marker, const char *phase,
                        const Census &census, uint64_t frame,
                        uint64_t encodesInFrame, uint64_t changeLogs) {
    std::fprintf(stderr,
        "%s {\"version\":2,\"provenance\":\"%s\",\"phase\":\"%s\",\"job_slots\":%llu,\"frame\":%llu,"
        "\"legacy_textures\":%llu,\"clean_depth\":%llu,\"upstream_raw_depth\":%llu,"
        "\"upstream_depth_mips\":%llu,\"upstream_normals\":%llu,\"upstream_ao0\":%llu,"
        "\"legacy_slots\":%llu,\"upstream_bundles\":%llu,\"upstream_bundle_refs\":%llu,"
        "\"upstream_lease_slots\":%llu,\"clean_slots\":%llu,"
        "\"retained_slots\":%llu,\"legacy_texture_bytes\":%llu,"
        "\"clean_depth_bytes\":%llu,\"upstream_texture_bytes\":%llu,"
        "\"retained_texture_bytes\":%llu,\"uniform_bytes\":%llu,"
        "\"target_width\":%llu,\"target_height\":%llu,"
        "\"job_width\":%llu,\"job_height\":%llu,"
        "\"full_width\":%llu,\"full_height\":%llu,"
        "\"upstream_width\":%llu,\"upstream_height\":%llu,"
        "\"encodes_in_frame\":%llu,\"peak_slots\":%llu,"
        "\"peak_retained_texture_bytes\":%llu,\"max_encodes_per_frame\":%llu,"
        "\"estimate_scope\":\"%s\",\"change_logs\":%llu}\n",
        marker, kProvenance, phase,
        (unsigned long long)kJobSlots, (unsigned long long)frame,
        (unsigned long long)census.legacyTextures, (unsigned long long)census.cleanDepth,
        (unsigned long long)census.upstreamRawDepth, (unsigned long long)census.upstreamDepthMips,
        (unsigned long long)census.upstreamNormals, (unsigned long long)census.upstreamAo0,
        (unsigned long long)census.legacySlots, (unsigned long long)census.upstreamBundles,
        (unsigned long long)census.upstreamBundleRefs, (unsigned long long)census.upstreamLeaseSlots,
        (unsigned long long)census.cleanSlots, (unsigned long long)census.retainedSlots,
        (unsigned long long)census.legacyTextureBytes, (unsigned long long)census.cleanDepthBytes,
        (unsigned long long)census.upstreamTextureBytes, (unsigned long long)census.retainedTextureBytes,
        (unsigned long long)census.uniformBytes,
        (unsigned long long)census.targetWidth, (unsigned long long)census.targetHeight,
        (unsigned long long)census.jobWidth, (unsigned long long)census.jobHeight,
        (unsigned long long)census.fullWidth, (unsigned long long)census.fullHeight,
        (unsigned long long)census.upstreamWidth, (unsigned long long)census.upstreamHeight,
        (unsigned long long)encodesInFrame,
        (unsigned long long)state.peakSlots, (unsigned long long)state.peakRetainedTextureBytes,
        (unsigned long long)state.maxEncodesPerFrame, kEstimateScope,
        (unsigned long long)changeLogs);
}

inline void observe(uint64_t frame, Census census) {
    if (!enabled()) return;
    census.retainedTextureBytes = census.legacyTextureBytes + census.cleanDepthBytes +
                                  census.upstreamTextureBytes;
    note_frame(frame);
    const uint64_t oldPeakSlots = state.peakSlots;
    const uint64_t oldPeakBytes = state.peakRetainedTextureBytes;
    if (census.retainedSlots > state.peakSlots) state.peakSlots = census.retainedSlots;
    if (census.retainedTextureBytes > state.peakRetainedTextureBytes)
        state.peakRetainedTextureBytes = census.retainedTextureBytes;
    const bool peakChanged = oldPeakSlots != state.peakSlots || oldPeakBytes != state.peakRetainedTextureBytes;
    if (!state.havePrevious || peakChanged || !same_census(census, state.previous)) {
        if (state.changeLogs < kMaxChangeLogs) {
            ++state.changeLogs;
            emit_record("[AO-RESOURCES-CHANGE]", "encode", census, frame,
                        state.encodesInFrame, state.changeLogs);
        }
    }
    state.previous = census;
    state.havePrevious = true;
}

inline void shutdown() {
    if (!enabled() || state.finalEmitted) return;
    state.finalEmitted = true;
    const Census census = state.havePrevious ? state.previous : Census{};
    emit_record("[AO-RESOURCES]", "shutdown_after_synchronize", census,
                state.currentFrame, state.encodesInFrame, state.changeLogs);
    std::fflush(stderr);
}
} // namespace mp6_ao_resource_probe
'''
RESOURCE_BODY = RESOURCE_HEADER[RESOURCE_HEADER.index("namespace mp6_ao_resource_probe {"):]


def replace_once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected exactly one source anchor, found {count}: {old!r}")
    return text.replace(old, new, 1)


def requested_texture_bytes(width: int, height: int, levels: int, bytes_per_texel: int) -> int:
    """Return the requested texel-byte estimate used by the C++ probe."""
    for name, value in (("width", width), ("height", height), ("levels", levels),
                        ("bytes_per_texel", bytes_per_texel)):
        if type(value) is not int:
            raise TypeError(f"{name} must be an integer")
        if value < 0 or (name in ("levels", "bytes_per_texel") and value == 0):
            raise ValueError(f"invalid {name}")
    return sum(max(1, width >> level) * max(1, height >> level) * bytes_per_texel
               for level in range(levels))


def instrument_ao_resources(text: str) -> str:
    """Inject the opt-in AO census into the exact current AO source anchors."""
    if MARKER in text:
        raise ValueError("source is already AO resource probe instrumented")
    # Validate the slot topology before adding any instrumentation.  This is a
    # test-only transform, so a changed ownership model must fail closed.
    text = replace_once(text, JOB_ARRAY_ANCHOR, JOB_ARRAY_ANCHOR)
    text = replace_once(text, PIPELINE_DECL_ANCHOR, PIPELINE_DECL_ANCHOR)
    text = replace_once(text, FRAME_MEMBER_ANCHOR,
                        FRAME_MEMBER_ANCHOR + "\n    uint64_t qaResourceFrame = UINT64_MAX;")
    text = replace_once(text, FRAME_ASSIGN_ANCHOR,
                        FRAME_ASSIGN_ANCHOR + "\n    job->qaResourceFrame = g_frame;")
    text = replace_once(text, ENCODE_END_ANCHOR, '''    ++g_encoded;
    if (mp6_ao_resource_probe::enabled()) {
        mp6_ao_resource_probe::Census census{};
        for (const auto &slot : g_jobs) {
            mp6_ao_resource_probe::observe_job(census,
                bool(slot.textures[0]), bool(slot.textures[1]), slot.width, slot.height,
                bool(slot.cleanDepth), slot.fullWidth, slot.fullHeight,
                bool(slot.uniform), sizeof(Params),
                bool(slot.upstream.constants), sizeof(XeGTAO::GTAOConstants),
                bool(slot.upstream.adapter), sizeof(mp6::xegtao::AdapterParams),
                bool(slot.upstream.scratch));
        }
        // The pipeline's current bundle and all job leases can overlap.  Keep
        // only raw identities in the census; never retain a shared_ptr here.
        const auto &retainedScratch = g_xegtao.retained_scratch();
        if (retainedScratch) {
            mp6_ao_resource_probe::observe_scratch(census, retainedScratch.get(),
                retainedScratch->width, retainedScratch->height,
                retainedScratch->depthStorage == mp6::xegtao::DepthStorage::Float16,
                bool(retainedScratch->rawDepth), bool(retainedScratch->depthMips),
                bool(retainedScratch->normals), bool(retainedScratch->ao[0]));
        }
        for (const auto &slot : g_jobs) {
            const auto &scratch = slot.upstream.scratch;
            if (!scratch) continue;
            mp6_ao_resource_probe::observe_scratch(census, scratch.get(),
                scratch->width, scratch->height,
                scratch->depthStorage == mp6::xegtao::DepthStorage::Float16,
                bool(scratch->rawDepth), bool(scratch->depthMips),
                bool(scratch->normals), bool(scratch->ao[0]));
        }
        // Keep the real attachment/working extents alongside the aggregate
        // census.  ctx.target* is the encoded target, while job/full/upstream
        // dimensions are this callback's retained AO resource extents.
        census.targetWidth = ctx.targetWidth;
        census.targetHeight = ctx.targetHeight;
        census.jobWidth = job.width;
        census.jobHeight = job.height;
        census.fullWidth = job.fullWidth;
        census.fullHeight = job.fullHeight;
        if (job.upstream.scratch) {
            census.upstreamWidth = job.upstream.scratch->width;
            census.upstreamHeight = job.upstream.scratch->height;
        }
        mp6_ao_resource_probe::observe(job.qaResourceFrame, census);
    }
}''')
    text = replace_once(text, SHUTDOWN_SYNC_ANCHOR,
                        SHUTDOWN_SYNC_ANCHOR + "\n    mp6_ao_resource_probe::shutdown();")
    return replace_once(
        text,
        PIPELINE_DECL_ANCHOR,
        PIPELINE_DECL_ANCHOR + "\n/* " + MARKER + ": local diagnostic only; never a release asset. */\n" + RESOURCE_BODY,
    )


V1_FIELDS = frozenset({
    "version", "phase", "job_slots", "frame",
    "legacy_textures", "clean_depth", "upstream_raw_depth", "upstream_depth_mips",
    "upstream_normals", "upstream_ao0", "legacy_slots", "upstream_slots", "clean_slots",
    "retained_slots", "legacy_texture_bytes", "clean_depth_bytes", "upstream_texture_bytes",
    "retained_texture_bytes", "uniform_bytes", "target_width", "target_height",
    "job_width", "job_height", "full_width", "full_height", "upstream_width",
    "upstream_height", "encodes_in_frame", "peak_slots",
    "peak_retained_texture_bytes", "max_encodes_per_frame", "estimate_scope", "change_logs",
})
V1_COUNT_FIELDS = frozenset({
    "job_slots", "legacy_textures", "clean_depth", "upstream_raw_depth", "upstream_depth_mips",
    "upstream_normals", "upstream_ao0", "legacy_slots", "upstream_slots", "clean_slots",
    "retained_slots", "legacy_texture_bytes", "clean_depth_bytes", "upstream_texture_bytes",
    "retained_texture_bytes", "uniform_bytes", "target_width", "target_height",
    "job_width", "job_height", "full_width", "full_height", "upstream_width",
    "upstream_height", "encodes_in_frame", "peak_slots",
    "peak_retained_texture_bytes", "max_encodes_per_frame", "change_logs",
})
V2_FIELDS = frozenset({
    "version", "provenance", "phase", "job_slots", "frame",
    "legacy_textures", "clean_depth", "upstream_raw_depth", "upstream_depth_mips",
    "upstream_normals", "upstream_ao0", "legacy_slots", "upstream_bundles",
    "upstream_bundle_refs", "upstream_lease_slots", "clean_slots", "retained_slots",
    "legacy_texture_bytes", "clean_depth_bytes", "upstream_texture_bytes",
    "retained_texture_bytes", "uniform_bytes", "target_width", "target_height",
    "job_width", "job_height", "full_width", "full_height", "upstream_width",
    "upstream_height", "encodes_in_frame", "peak_slots", "peak_retained_texture_bytes",
    "max_encodes_per_frame", "estimate_scope", "change_logs",
})
V2_COUNT_FIELDS = frozenset({
    "job_slots", "legacy_textures", "clean_depth", "upstream_raw_depth", "upstream_depth_mips",
    "upstream_normals", "upstream_ao0", "legacy_slots", "upstream_bundles",
    "upstream_bundle_refs", "upstream_lease_slots", "clean_slots", "retained_slots",
    "legacy_texture_bytes", "clean_depth_bytes", "upstream_texture_bytes",
    "retained_texture_bytes", "uniform_bytes", "target_width", "target_height",
    "job_width", "job_height", "full_width", "full_height", "upstream_width",
    "upstream_height", "encodes_in_frame", "peak_slots", "peak_retained_texture_bytes",
    "max_encodes_per_frame", "change_logs",
})


def _strict_json(payload: str, marker: str) -> dict[str, Any]:
    def reject_constant(value: str) -> None:
        raise ValueError(f"invalid JSON constant in {marker}: {value}")

    def reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON field in {marker}: {key}")
            result[key] = value
        return result

    try:
        result = json.loads(payload, object_pairs_hook=reject_duplicate_pairs,
                            parse_constant=reject_constant)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid JSON payload in {marker}") from exc
    if not isinstance(result, dict):
        raise ValueError(f"{marker} payload is not an object")
    return result


def _validate_record(record: dict[str, Any], phase: str) -> dict[str, Any]:
    version = record.get("version")
    if type(version) is not int:
        raise ValueError("unsupported AO resource record version")
    if version == 1:
        fields, count_fields = V1_FIELDS, V1_COUNT_FIELDS
        scope = V1_ESTIMATE_SCOPE
    elif version == 2:
        fields, count_fields = V2_FIELDS, V2_COUNT_FIELDS
        scope = ESTIMATE_SCOPE
    else:
        raise ValueError("unsupported AO resource record version")
    if set(record) != fields:
        raise ValueError("AO resource record has missing or unexpected fields")
    if record["phase"] != phase or record["estimate_scope"] != scope:
        raise ValueError("unsupported AO resource record phase or estimate scope")
    if version == 2 and record["provenance"] != V2_PROVENANCE:
        raise ValueError("AO resource record has incorrect shared-scratch provenance")
    for name in count_fields:
        value = record[name]
        if type(value) is not int or value < 0:
            raise ValueError(f"invalid AO resource count: {name}")
    if type(record["frame"]) is not int or record["frame"] < 0:
        raise ValueError("invalid AO resource frame")
    if record["job_slots"] != JOB_SLOTS:
        raise ValueError("AO resource census does not cover all 64 Job slots")
    if record["legacy_textures"] > 2 * JOB_SLOTS:
        raise ValueError("legacy texture count exceeds Job capacity")
    if version == 1:
        slot_fields = ("legacy_slots", "upstream_slots", "clean_slots", "retained_slots", "peak_slots")
        retained_categories = ("legacy_slots", "upstream_slots", "clean_slots")
    else:
        slot_fields = ("legacy_slots", "upstream_lease_slots", "clean_slots", "retained_slots", "peak_slots")
        retained_categories = ("legacy_slots", "upstream_lease_slots", "clean_slots")
        if record["upstream_bundles"] > MAX_SCRATCH_REFERENCES:
            raise ValueError("AO scratch bundle count exceeds the 65-reference bound")
        if record["upstream_bundle_refs"] > MAX_SCRATCH_REFERENCES:
            raise ValueError("AO scratch reference count exceeds the 65-reference bound")
        if record["upstream_bundles"] > record["upstream_bundle_refs"]:
            raise ValueError("unique AO scratch bundles exceed observed references")
        if record["upstream_bundles"] == 0 and record["upstream_bundle_refs"] != 0:
            raise ValueError("AO scratch references exist without a unique bundle")
        for name in ("upstream_raw_depth", "upstream_depth_mips", "upstream_normals", "upstream_ao0"):
            if record[name] > MAX_SCRATCH_REFERENCES:
                raise ValueError(f"AO scratch texture count exceeds the 65-reference bound: {name}")
    for name in slot_fields:
        if record[name] > JOB_SLOTS:
            raise ValueError(f"AO slot count exceeds capacity: {name}")
    if record["retained_slots"] < max(record[name] for name in retained_categories):
        raise ValueError("retained slot count is below a category count")
    if record["retained_texture_bytes"] != (
            record["legacy_texture_bytes"] + record["clean_depth_bytes"] +
            record["upstream_texture_bytes"]):
        raise ValueError("AO retained texture bytes do not equal the category sum")
    if record["peak_slots"] < record["retained_slots"]:
        raise ValueError("AO peak slots are below the current census")
    if record["peak_retained_texture_bytes"] < record["retained_texture_bytes"]:
        raise ValueError("AO peak texture bytes are below the current census")
    if record["max_encodes_per_frame"] < record["encodes_in_frame"]:
        raise ValueError("AO maximum encodes are below the current frame")
    if record["max_encodes_per_frame"] == 0:
        raise ValueError("AO resource census contains no encoded callback")
    if record["change_logs"] > MAX_CHANGE_LOGS:
        raise ValueError("AO resource change log exceeded its bound")
    return record


def parse_resource_log(log: str) -> dict[str, Any]:
    """Strictly parse bounded change rows and the final post-synchronize row."""
    if not isinstance(log, str):
        raise TypeError("log must be text")
    lines = log.splitlines()
    for line in lines:
        if line.startswith("[AO-RESOURCES") and not (
                line.startswith("[AO-RESOURCES]") or
                line.startswith("[AO-RESOURCES-CHANGE]")):
            raise ValueError("unrecognized AO resource marker")
    final_lines = [line for line in lines if line.startswith("[AO-RESOURCES]")]
    if len(final_lines) != 1:
        raise ValueError("expected exactly one final [AO-RESOURCES] record")
    final_index = next(index for index, line in enumerate(lines)
                       if line.startswith("[AO-RESOURCES]"))
    final_payload = final_lines[0][len("[AO-RESOURCES]"):].strip()
    final = _validate_record(_strict_json(final_payload, "[AO-RESOURCES]"),
                             "shutdown_after_synchronize")

    change_lines = [line for line in lines if line.startswith("[AO-RESOURCES-CHANGE]")]
    if len(change_lines) > MAX_CHANGE_LOGS:
        raise ValueError("too many AO resource change records")
    if any(index > final_index for index, line in enumerate(lines)
           if line.startswith("[AO-RESOURCES-CHANGE]")):
        raise ValueError("AO resource change row follows final shutdown row")
    changes = []
    previous_change_index = 0
    for line in change_lines:
        payload = line[len("[AO-RESOURCES-CHANGE]"):].strip()
        record = _validate_record(_strict_json(payload, "[AO-RESOURCES-CHANGE]"), "encode")
        if record["version"] != final["version"]:
            raise ValueError("AO resource change/final records mix schema versions")
        if record["change_logs"] <= previous_change_index:
            raise ValueError("AO resource change log index is not increasing")
        previous_change_index = record["change_logs"]
        changes.append(record)
    if final["change_logs"] != len(changes):
        raise ValueError("final AO resource change count disagrees with change rows")
    if final["max_encodes_per_frame"] > 0 and not changes:
        raise ValueError("final AO resource record has no encode change row")
    version = final["version"]
    return {
        "record": final,
        "changes": changes,
        "schema_version": version,
        "provenance": (
            "legacy_job_owned_draw_snapshot"
            if version == 1 else V2_PROVENANCE
        ),
        "scope": (
            "requested texture texel-byte estimate only; v1 counts Job-owned "
            "textures, while v2 deduplicates shared Scratch identities across "
            "the pipeline-retained bundle and Job leases; uniform bytes remain "
            "per-job logical buffer sizes; excludes physical VRAM, caches, views, "
            "borrowed snapshots, pipelines, and the shared LUT"
        ),
    }


# A descriptive alias for callers that name the feature rather than the log.
parse_ao_resources = parse_resource_log
