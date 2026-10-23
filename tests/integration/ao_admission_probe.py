"""QA-only AO admission accounting for real and interpolated frames.

This module transforms generated QA copies of the AO worker and FI source.
It records bounded counters and one shutdown JSON summary; it never changes a
render decision or emits per-frame logs.
"""

from __future__ import annotations

import json
from typing import Any


ENV_NAME = "MP6_QA_AO_ADMISSION"
MARKER = "MP6_QA_AO_ADMISSION"
SUMMARY_MARKER = "[AO-ADMISSION]"
FI_MARKER = "MP6_QA_AO_ADMISSION_FI"

_NAMESPACE = "namespace {\nusing namespace aurora;\n"
_JOB = "    bool canonicalDepth = false; /* Full XeGTAO experiment; never legacy/Half */\n"
_SEED = "    uint32_t sourceSamples=0; /* zero: R32 snapshot; otherwise borrowed attachment */\n"
_PREP_BEGIN = "    // AO_UPSTREAM_PREPARATION_BEGIN\n"
_APPLY = "extern \"C\" void mp6_ao_apply(const Mp6AoView *view) {\n"
_SEED_FRAME = "    job->frame=g_frame; job->depth=targets.depth;\n"
_JOB_FRAME = "    job->frame = g_frame;\n"
_SHUTDOWN_HEAD = "extern \"C\" void mp6_ao_shutdown(void) {\n"
_FOLIAGE_BEGIN_HEAD = "extern \"C\" int mp6_ao_foliage_begin(int camera) {\n"
_FOLIAGE_END_HEAD = "extern \"C\" void mp6_ao_foliage_end(int camera) {\n"
_FI_REPLAY = "static int s_inReplay;"


ADMISSION_HELPER = r'''
/* MP6_QA_AO_ADMISSION: bounded QA counters only. */
extern "C" int qa_ao_admission_is_replay(void);
extern "C" long mp6_tick_count;
enum { QA_REAL = 0, QA_REPLAY = 1 };
enum { QA_APPLY_CALLS, QA_DISABLED, QA_INVALID, QA_VALID, QA_POOL,
       QA_TASK_REJECTED, QA_TASK_ACCEPTED, QA_COMPOSITE_REJECTED,
       QA_COMPOSITE_ACCEPTED, QA_APPLY_N };
enum { QA_F_INVALID, QA_F_NESTED, QA_F_DIRECT, QA_F_POOL, QA_F_RESOLVE,
       QA_F_CREATE, QA_F_DRAW_REJECTED, QA_F_SEED, QA_F_N };
enum { QA_W_MASK, QA_W_DIM_DROP, QA_W_ENCODED, QA_W_COMPOSITED, QA_W_N };
struct QaAoAdmissionFailure {
    uint64_t frame, tick;
    int camera, replay;
    const char *reason;
};
static std::array<std::array<std::atomic<uint64_t>, QA_APPLY_N>, 2> g_qaApply{};
static std::array<std::array<std::atomic<uint64_t>, QA_F_N>, 2> g_qaFoliage{};
static std::array<std::array<std::atomic<uint64_t>, QA_W_N>, 2> g_qaWorker{};
static std::array<std::atomic<uint64_t>, 2> g_qaEndResolved{};
static std::array<std::atomic<uint64_t>, 2> g_qaEndFailed{};
static std::array<QaAoAdmissionFailure, 32> g_qaFailures{};
static std::atomic<unsigned> g_qaFailureCount{0};

static bool qa_ao_admission_enabled(void)
{
    static const bool enabled = [] {
        const char *value = std::getenv("MP6_QA_AO_ADMISSION");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
static int qa_ao_admission_replay(void)
{
    return qa_ao_admission_enabled() && qa_ao_admission_is_replay() ? QA_REPLAY : QA_REAL;
}
static void qa_ao_admission_add(std::array<std::array<std::atomic<uint64_t>, QA_APPLY_N>, 2> &table,
                                int bucket, int index)
{ if (qa_ao_admission_enabled()) table[bucket][index].fetch_add(1, std::memory_order_relaxed); }
static void qa_ao_admission_failure(uint64_t frame, uint64_t tick, int camera,
                                    int replay, const char *reason)
{
    if (!qa_ao_admission_enabled()) return;
    const unsigned index = g_qaFailureCount.fetch_add(1, std::memory_order_relaxed);
    if (index < g_qaFailures.size()) g_qaFailures[index] = {frame, tick, camera, replay, reason};
}
static void qa_ao_foliage_add(int bucket, int index)
{ if (qa_ao_admission_enabled()) g_qaFoliage[bucket][index].fetch_add(1, std::memory_order_relaxed); }
static void qa_ao_worker_add(int bucket, int index)
{ if (qa_ao_admission_enabled()) g_qaWorker[bucket][index].fetch_add(1, std::memory_order_relaxed); }
static void qa_ao_end_add(int bucket, bool resolved)
{
    if (!qa_ao_admission_enabled()) return;
    (resolved ? g_qaEndResolved : g_qaEndFailed)[bucket].fetch_add(1, std::memory_order_relaxed);
}
static uint64_t qa_ao_end_total(const std::array<std::atomic<uint64_t>, 2> &values)
{
    return values[QA_REAL].load(std::memory_order_relaxed) +
           values[QA_REPLAY].load(std::memory_order_relaxed);
}

template<size_t N>
static void qa_ao_print_counts(const char *const (&names)[N],
                               const std::array<std::atomic<uint64_t>, N> &values)
{
    std::fputc('{', stderr);
    for (size_t i = 0; i < N; ++i) {
        if (i) std::fputc(',', stderr);
        std::fprintf(stderr, "\"%s\":%llu", names[i],
                     (unsigned long long)values[i].load(std::memory_order_relaxed));
    }
    std::fputc('}', stderr);
}
static void qa_ao_emit_admission(void)
{
    static bool emitted = false;
    if (!qa_ao_admission_enabled() || emitted) return;
    emitted = true;
    static const char *const applyNames[] = {"applyCalls","disabledOrMissingView","invalidView",
        "validApply","poolExhausted","taskRejected","taskAccepted","compositeRejected","compositeAccepted"};
    static const char *const foliageNames[] = {"invalidCamera","nested","directAccepted",
        "seedPoolExhausted","resolveFailed","createFailed","seedDrawRejected","seedAccepted"};
    static const char *const workerNames[] = {"maskPresent","maskDimensionDropped","encoded","composited"};
    std::fprintf(stderr, "[AO-ADMISSION] {\"version\":1,\"schema_version\":1,\"enabled\":1,\"apply\":{\"real\":");
    qa_ao_print_counts(applyNames, g_qaApply[QA_REAL]);
    std::fprintf(stderr, ",\"replay\":"); qa_ao_print_counts(applyNames, g_qaApply[QA_REPLAY]);
    std::fprintf(stderr, "},\"foliage\":{\"real\":"); qa_ao_print_counts(foliageNames, g_qaFoliage[QA_REAL]);
    std::fprintf(stderr, ",\"replay\":"); qa_ao_print_counts(foliageNames, g_qaFoliage[QA_REPLAY]);
    std::fprintf(stderr, ",\"endResolved\":%llu,\"endFailed\":%llu},\"worker\":{\"real\":",
        (unsigned long long)qa_ao_end_total(g_qaEndResolved),
        (unsigned long long)qa_ao_end_total(g_qaEndFailed));
    qa_ao_print_counts(workerNames, g_qaWorker[QA_REAL]);
    std::fprintf(stderr, ",\"replay\":"); qa_ao_print_counts(workerNames, g_qaWorker[QA_REPLAY]);
    const unsigned count = std::min<unsigned>(g_qaFailureCount.load(std::memory_order_relaxed), 32);
    std::fprintf(stderr, "},\"failures\":[");
    for (unsigned i = 0; i < count; ++i) {
        if (i) std::fputc(',', stderr);
        const auto &failure = g_qaFailures[i];
        std::fprintf(stderr, "{\"frame\":%llu,\"tick\":%llu,\"camera\":%d,\"replay\":%d,\"reason\":\"%s\"}",
                     (unsigned long long)failure.frame, (unsigned long long)failure.tick,
                     failure.camera, failure.replay, failure.reason);
    }
    std::fprintf(stderr, "]}\n"); std::fflush(stderr);
}
'''


def _replace_once(source: str, anchor: str, replacement: str) -> str:
    count = source.count(anchor)
    if count != 1:
        raise ValueError(f"AO admission anchor changed: {anchor!r} count={count}")
    return source.replace(anchor, replacement, 1)


def instrument_fi_admission(source: str) -> str:
    """Expose the FI replay bit to the generated AO QA translation unit."""

    if not isinstance(source, str):
        raise TypeError("source must be text")
    if FI_MARKER in source:
        raise ValueError("FI admission probe is already instrumented")
    if source.count(_FI_REPLAY) != 1:
        raise ValueError(f"FI replay anchor changed count={source.count(_FI_REPLAY)}")
    function = '''
/* MP6_QA_AO_ADMISSION_FI */
extern int qa_ao_admission_is_replay(void) { return s_inReplay; }
'''
    return _replace_once(source, _FI_REPLAY, _FI_REPLAY + function)


def instrument_ao_admission(source: str) -> str:
    """Instrument current AO admission/worker boundaries with strict anchors."""

    if not isinstance(source, str):
        raise TypeError("source must be text")
    if MARKER in source:
        raise ValueError("AO admission probe is already instrumented")
    anchors = (_NAMESPACE, _JOB, _SEED, _PREP_BEGIN, _APPLY, _SEED_FRAME,
               _JOB_FRAME, _SHUTDOWN_HEAD, _FOLIAGE_BEGIN_HEAD, _FOLIAGE_END_HEAD)
    for anchor in anchors:
        if source.count(anchor) != 1:
            raise ValueError(f"AO admission anchor changed: {anchor!r} count={source.count(anchor)}")
    transformed = _replace_once(source, _NAMESPACE, _NAMESPACE + ADMISSION_HELPER + "\n")
    transformed = _replace_once(
        transformed, _JOB,
        _JOB + "    uint64_t qaAdmissionFrame=0, qaAdmissionTick=0; int qaAdmissionCamera=-1, qaAdmissionReplay=0;\n",
    )
    transformed = _replace_once(
        transformed, _SEED,
        _SEED + "    uint64_t qaAdmissionFrame=0, qaAdmissionTick=0; int qaAdmissionCamera=-1, qaAdmissionReplay=0;\n",
    )
    transformed = _replace_once(
        transformed, _APPLY,
        _APPLY + "    const int qaReplay = qa_ao_admission_replay();\n    qa_ao_admission_add(g_qaApply, qaReplay, QA_APPLY_CALLS);\n",
    )
    transformed = _replace_once(
        transformed,
        "    const int level = mp6_enh_ambient_occlusion();\n    if (!level || !view) return;\n",
        "    const int level = mp6_enh_ambient_occlusion();\n"
        "    if (!level || !view) { qa_ao_admission_add(g_qaApply, qaReplay, QA_DISABLED); "
        "qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, view ? view->camera : -1, qaReplay, "
        "level ? \"missing_view\" : \"disabled\"); return; }\n",
    )
    transformed = _replace_once(
        transformed,
        "    for (float value : view->projection) if (!std::isfinite(value) || value <= 0) return;\n"
        "    if (view->projection[3] <= view->projection[2] ||\n        view->viewport[2] <= 0 || view->viewport[3] <= 0 ||\n        view->depthRange[1] <= view->depthRange[0]) return;\n",
        "    for (float value : view->projection) if (!std::isfinite(value) || value <= 0) { "
        "qa_ao_admission_add(g_qaApply, qaReplay, QA_INVALID); qa_ao_admission_failure(g_frame, "
        "(uint64_t)mp6_tick_count, view->camera, qaReplay, \"invalid_projection\"); return; }\n"
        "    if (view->projection[3] <= view->projection[2] || view->viewport[2] <= 0 || "
        "view->viewport[3] <= 0 || view->depthRange[1] <= view->depthRange[0]) { "
        "qa_ao_admission_add(g_qaApply, qaReplay, QA_INVALID); qa_ao_admission_failure(g_frame, "
        "(uint64_t)mp6_tick_count, view->camera, qaReplay, \"invalid_view\"); return; }\n"
        "    qa_ao_admission_add(g_qaApply, qaReplay, QA_VALID);\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!job) { warn(\"GPU frame slot capacity exceeded\"); return; }\n",
        "    if (!job) { qa_ao_admission_add(g_qaApply, qaReplay, QA_POOL); "
        "qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, view->camera, qaReplay, "
        "\"pool_exhausted\"); warn(\"GPU frame slot capacity exceeded\"); return; }\n",
    )
    transformed = _replace_once(
        transformed, _JOB_FRAME,
        _JOB_FRAME + "    job->qaAdmissionFrame=g_frame; job->qaAdmissionTick=(uint64_t)mp6_tick_count; "
        "job->qaAdmissionCamera=view->camera; job->qaAdmissionReplay=qaReplay;\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {\n        job->available.store(true, std::memory_order_release);\n        warn(\"scene encoder task rejected\");\n        return;\n    }\n",
        "    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {\n"
        "        qa_ao_admission_add(g_qaApply, qaReplay, QA_TASK_REJECTED);\n"
        "        qa_ao_admission_failure(job->qaAdmissionFrame, job->qaAdmissionTick, job->qaAdmissionCamera, qaReplay, \"task_rejected\");\n"
        "        job->available.store(true, std::memory_order_release);\n"
        "        warn(\"scene encoder task rejected\");\n        return;\n    }\n"
        "    qa_ao_admission_add(g_qaApply, qaReplay, QA_TASK_ACCEPTED);\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!gfx::push_custom_draw(g_drawType, &job, sizeof(job))) warn(\"scene composite rejected\");\n",
        "    if (!gfx::push_custom_draw(g_drawType, &job, sizeof(job))) {\n"
        "        qa_ao_admission_add(g_qaApply, qaReplay, QA_COMPOSITE_REJECTED);\n"
        "        qa_ao_admission_failure(job->qaAdmissionFrame, job->qaAdmissionTick, job->qaAdmissionCamera, qaReplay, \"composite_rejected\");\n"
        "        warn(\"scene composite rejected\");\n    } else {\n"
        "        qa_ao_admission_add(g_qaApply, qaReplay, QA_COMPOSITE_ACCEPTED);\n    }\n",
    )

    # The foliage receiver has its own slot pool and its own main-thread
    # begin/end bracket.  Keep the replay/camera/tick snapshot on that slot so
    # worker callbacks never consult mutable global frame state.
    transformed = _replace_once(
        transformed, _FOLIAGE_BEGIN_HEAD,
        _FOLIAGE_BEGIN_HEAD + "    const int qaReplay = qa_ao_admission_replay();\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (camera<0 || camera>=int(g_foliage.size()) || g_foliageCamera!=-1) return 0;\n",
        "    if (camera<0 || camera>=int(g_foliage.size())) {\n"
        "        qa_ao_foliage_add(qaReplay, QA_F_INVALID);\n"
        "        qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"invalid_camera\");\n"
        "        return 0;\n    }\n"
        "    if (g_foliageCamera!=-1) {\n"
        "        qa_ao_foliage_add(qaReplay, QA_F_NESTED);\n"
        "        qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"nested_begin\");\n"
        "        return 0;\n    }\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!gfx::ao_compatibility_mode() && gfx::create_pass_with_readonly_efb_depth(width,height)) {\n",
        "    if (!gfx::ao_compatibility_mode() && gfx::create_pass_with_readonly_efb_depth(width,height)) {\n"
        "        qa_ao_foliage_add(qaReplay, QA_F_DIRECT);\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!job) return 0;\n",
        "    if (!job) {\n"
        "        qa_ao_foliage_add(qaReplay, QA_F_POOL);\n"
        "        qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"seed_pool_exhausted\");\n"
        "        return 0;\n    }\n",
    )
    transformed = _replace_once(
        transformed,
        "        if (!gfx::resolve_pass({.color=false,.depth=true},targets) || !targets.depth || !targets.width || !targets.height) return 0;\n",
        "        if (!gfx::resolve_pass({.color=false,.depth=true},targets) || !targets.depth || !targets.width || !targets.height) {\n"
        "            qa_ao_foliage_add(qaReplay, QA_F_RESOLVE);\n"
        "            qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"seed_resolve_failed\");\n"
        "            return 0;\n        }\n",
    )
    transformed = _replace_once(
        transformed,
        "        if (!gfx::create_pass(targets.width,targets.height)) return 0;\n",
        "        if (!gfx::create_pass(targets.width,targets.height)) {\n"
        "            qa_ao_foliage_add(qaReplay, QA_F_CREATE);\n"
        "            qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"seed_create_failed\");\n"
        "            return 0;\n        }\n",
    )
    transformed = _replace_once(
        transformed, _SEED_FRAME,
        _SEED_FRAME + "    job->qaAdmissionFrame=g_frame; job->qaAdmissionTick=(uint64_t)mp6_tick_count; "
        "job->qaAdmissionCamera=camera; job->qaAdmissionReplay=qaReplay;\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (!gfx::push_custom_draw(g_foliageSeedType,&job,sizeof(job))) {\n"
        "        gfx::ResolvedTargets discarded; gfx::resolve_pass({.color=false,.depth=false},discarded);\n"
        "        job->available.store(true,std::memory_order_release); return 0;\n    }\n",
        "    if (!gfx::push_custom_draw(g_foliageSeedType,&job,sizeof(job))) {\n"
        "        qa_ao_foliage_add(qaReplay, QA_F_DRAW_REJECTED);\n"
        "        qa_ao_admission_failure(job->qaAdmissionFrame, job->qaAdmissionTick, job->qaAdmissionCamera, qaReplay, \"seed_draw_rejected\");\n"
        "        gfx::ResolvedTargets discarded; gfx::resolve_pass({.color=false,.depth=false},discarded);\n"
        "        job->available.store(true,std::memory_order_release); return 0;\n    }\n"
        "    qa_ao_foliage_add(qaReplay, QA_F_SEED);\n",
    )
    transformed = _replace_once(
        transformed, _FOLIAGE_END_HEAD,
        _FOLIAGE_END_HEAD + "    const int qaReplay = qa_ao_admission_replay();\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (camera!=g_foliageCamera) return;\n",
        "    if (camera!=g_foliageCamera) {\n"
        "        qa_ao_end_add(qaReplay, false);\n"
        "        qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"foliage_end_mismatch\");\n"
        "        return;\n    }\n",
    )
    transformed = _replace_once(
        transformed,
        "    if (gfx::resolve_pass({.color=true,.depth=false},targets) && targets.color)\n"
        "        g_foliage[camera]={targets.color,targets.width,targets.height};\n",
        "    if (gfx::resolve_pass({.color=true,.depth=false},targets) && targets.color) {\n"
        "        g_foliage[camera]={targets.color,targets.width,targets.height};\n"
        "        qa_ao_end_add(qaReplay, true);\n"
        "    } else {\n"
        "        qa_ao_end_add(qaReplay, false);\n"
        "        qa_ao_admission_failure(g_frame, (uint64_t)mp6_tick_count, camera, qaReplay, \"foliage_end_resolve_failed\");\n"
        "    }\n",
    )

    # Worker callbacks use only the immutable reservation snapshot.  A mask
    # may be present but discarded when its dimensions do not match the
    # admitted target; count both facts without changing that decision.
    transformed = _replace_once(
        transformed,
        "    if (job.foliageWidth != ctx.targetWidth || job.foliageHeight != ctx.targetHeight) job.foliage = nullptr;\n",
        "    if (job.foliage) qa_ao_worker_add(job.qaAdmissionReplay, QA_W_MASK);\n"
        "    if (job.foliageWidth != ctx.targetWidth || job.foliageHeight != ctx.targetHeight) {\n"
        "        if (job.foliage) qa_ao_worker_add(job.qaAdmissionReplay, QA_W_DIM_DROP);\n"
        "        job.foliage = nullptr;\n    }\n",
    )
    transformed = _replace_once(
        transformed, "    ++g_encoded;\n",
        "    ++g_encoded;\n    qa_ao_worker_add(job.qaAdmissionReplay, QA_W_ENCODED);\n",
    )
    transformed = _replace_once(
        transformed, "    ++g_composited;\n",
        "    ++g_composited;\n    qa_ao_worker_add(job.qaAdmissionReplay, QA_W_COMPOSITED);\n",
    )

    # Synchronize exactly once before emitting the bounded summary.  The
    # non-QA branch retains the production ordering and the summary is still
    # emitted when no draw type was admitted, which makes empty/failure runs
    # visible to the parser instead of looking like a silent pass.
    initialized = ("    const bool initialized = g_drawType != gfx::InvalidDrawType || "
                   "g_foliageSeedType != gfx::InvalidDrawType;\n")
    transformed = _replace_once(
        transformed,
        _SHUTDOWN_HEAD + initialized + "    if (initialized) gfx::synchronize();\n",
        _SHUTDOWN_HEAD + initialized +
        "    const bool qaAdmission = qa_ao_admission_enabled();\n"
        "    if (qaAdmission) {\n"
        "        gfx::synchronize();\n"
        "        qa_ao_emit_admission();\n"
        "    }\n"
        "    if (initialized && !qaAdmission) gfx::synchronize();\n",
    )
    return transformed


def _strict_object(payload: str) -> dict[str, Any]:
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate admission field: {key}")
            result[key] = value
        return result
    try:
        value = json.loads(payload, object_pairs_hook=unique)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError("malformed AO admission JSON") from exc
    if not isinstance(value, dict):
        raise ValueError("AO admission summary must be an object")
    return value


def _counter(value: Any, name: str) -> int:
    if type(value) is not int or value < 0:
        raise ValueError(f"invalid AO admission counter: {name}")
    return value


def _bucket(summary: dict[str, Any], section: str, bucket: str, names: tuple[str, ...]) -> dict[str, int]:
    section_value = summary.get(section)
    if not isinstance(section_value, dict):
        raise ValueError(f"missing AO admission section: {section}")
    value = section_value.get(bucket)
    if not isinstance(value, dict):
        raise ValueError(f"missing AO admission bucket: {section}.{bucket}")
    if set(value) != set(names):
        raise ValueError(f"unexpected AO admission fields: {section}.{bucket}")
    return {name: _counter(value.get(name), f"{section}.{bucket}.{name}") for name in names}


def parse_admission_log(log: str) -> dict[str, Any]:
    """Parse and validate one complete, healthy AO admission summary."""

    if not isinstance(log, str):
        raise TypeError("log must be text")
    lines = [line[len(SUMMARY_MARKER):].strip() for line in log.splitlines()
             if line.startswith(SUMMARY_MARKER + " ")]
    if len(lines) != 1:
        raise ValueError("AO admission log must contain exactly one summary")
    summary = _strict_object(lines[0])
    required = {"version", "schema_version", "enabled", "apply", "foliage", "worker", "failures"}
    if set(summary) != required:
        raise ValueError("AO admission summary has missing or unexpected fields")
    if (type(summary.get("version")) is not int or
            type(summary.get("schema_version")) is not int or
            type(summary.get("enabled")) is not int or
            summary["version"] != 1 or summary["schema_version"] != 1 or
            summary["enabled"] != 1):
        raise ValueError("unsupported or disabled AO admission summary")
    apply_names = ("applyCalls","disabledOrMissingView","invalidView","validApply","poolExhausted",
                   "taskRejected","taskAccepted","compositeRejected","compositeAccepted")
    foliage_names = ("invalidCamera","nested","directAccepted","seedPoolExhausted","resolveFailed",
                     "createFailed","seedDrawRejected","seedAccepted")
    worker_names = ("maskPresent","maskDimensionDropped","encoded","composited")
    real = _bucket(summary, "apply", "real", apply_names)
    replay = _bucket(summary, "apply", "replay", apply_names)
    _bucket(summary, "foliage", "real", foliage_names)
    _bucket(summary, "foliage", "replay", foliage_names)
    worker_real = _bucket(summary, "worker", "real", worker_names)
    worker_replay = _bucket(summary, "worker", "replay", worker_names)
    foliage_section = summary["foliage"]
    if not isinstance(foliage_section, dict):
        raise ValueError("malformed AO foliage summary")
    if set(foliage_section) != {"real", "replay", "endResolved", "endFailed"}:
        raise ValueError("AO foliage summary has missing or unexpected fields")
    end_resolved = _counter(summary.get("foliage", {}).get("endResolved"), "foliage.endResolved")
    end_failed = _counter(summary.get("foliage", {}).get("endFailed"), "foliage.endFailed")
    failures = summary.get("failures")
    if not isinstance(failures, list) or len(failures) > 32:
        raise ValueError("invalid bounded AO admission failures")
    for failure in failures:
        if not isinstance(failure, dict) or set(failure) != {"frame", "tick", "camera", "replay", "reason"}:
            raise ValueError("malformed AO admission failure sample")
        if not isinstance(failure.get("reason"), str) or not failure["reason"]:
            raise ValueError("malformed AO admission failure reason")
        for name in ("frame", "tick", "replay"):
            _counter(failure.get(name), f"failure.{name}")
        camera = failure.get("camera")
        if type(camera) is not int or camera < -1:
            raise ValueError("invalid AO admission failure camera")
    if failures:
        raise ValueError("AO admission contains failure samples")
    totals = {name: real[name] + replay[name] for name in apply_names}
    if totals["validApply"] <= 0:
        raise ValueError("AO admission contains no accepted AO apply")
    if totals["invalidView"] or totals["poolExhausted"] or totals["taskRejected"] or totals["compositeRejected"]:
        raise ValueError("AO admission contains apply failures")
    for bucket_name, values in (("real", real), ("replay", replay)):
        if values["applyCalls"] != values["disabledOrMissingView"] + values["invalidView"] + values["validApply"]:
            raise ValueError(f"AO admission apply-return conservation mismatch: {bucket_name}")
    if totals["validApply"] != totals["taskAccepted"] or totals["taskAccepted"] != totals["compositeAccepted"]:
        raise ValueError("AO admission apply conservation mismatch")
    foliage = [_bucket(summary, "foliage", bucket, foliage_names) for bucket in ("real", "replay")]
    for values in foliage:
        if any(values[name] for name in ("invalidCamera","nested","seedPoolExhausted","resolveFailed",
                                         "createFailed","seedDrawRejected")):
            raise ValueError("AO foliage admission contains failures")
    total_seed = sum(values["seedAccepted"] for values in foliage)
    total_direct = sum(values["directAccepted"] for values in foliage)
    total_begin = sum(sum(values.values()) for values in foliage)
    if total_begin != total_seed + total_direct or end_resolved != total_seed + total_direct:
        raise ValueError("AO foliage begin conservation mismatch")
    if (end_failed or worker_real["maskDimensionDropped"] or
            worker_replay["maskDimensionDropped"] or
            worker_real["maskDimensionDropped"] > worker_real["maskPresent"] or
            worker_replay["maskDimensionDropped"] > worker_replay["maskPresent"]):
        raise ValueError("AO foliage/worker admission contains failures")
    if worker_real["encoded"] != real["taskAccepted"] or worker_replay["encoded"] != replay["taskAccepted"]:
        raise ValueError("AO worker encode conservation mismatch")
    if worker_real["composited"] != real["compositeAccepted"] or worker_replay["composited"] != replay["compositeAccepted"]:
        raise ValueError("AO worker composite conservation mismatch")
    summary["totals"] = {name: totals[name] for name in apply_names}
    summary["totals"]["endResolved"] = end_resolved
    summary["totals"]["endFailed"] = end_failed
    return summary


__all__ = ["ADMISSION_HELPER", "ENV_NAME", "FI_MARKER", "MARKER", "SUMMARY_MARKER",
           "instrument_ao_admission", "instrument_fi_admission", "parse_admission_log"]
