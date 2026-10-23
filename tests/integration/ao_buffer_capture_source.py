"""Private-build source transforms for the ordered XeGTAO buffer capture.

The capture contract is deliberately kept out of the production AO sources.
The builder writes transformed copies below ``build/ao-buffer-qa`` and links
those copies into a QA-only executable.  The transforms are exact-count
operations so a changed production seam fails closed instead of silently
producing a different diagnostic binary.
"""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
NATIVE_DIR = ROOT / "src" / "gx" / "xegtao"

STAGES = (
    ("clean_depth", 0),
    ("canonical_depth", 0),
    ("depth_mip", 0),
    ("depth_mip", 1),
    ("depth_mip", 2),
    ("depth_mip", 3),
    ("depth_mip", 4),
    ("normals", 0),
    ("raw_ao", 0),
    ("edges", 0),
    ("final_ao", 0),
)


def replace_once(text: str, old: str, new: str, *, label: str | None = None) -> str:
    """Replace one source seam, rejecting both drift and re-instrumentation."""

    count = text.count(old)
    if count != 1:
        raise ValueError(f"ao buffer seam expected once: {label or old[:100]!r} ({count})")
    return text.replace(old, new)


def _include_path(path: str) -> str:
    # C++ accepts forward-slash absolute paths on Windows and this keeps the
    # generated source stable in command/dependency records.
    return Path(path).resolve().as_posix()


def _resolve_native_includes(source: str, native_dir: Path) -> str:
    """Make every relative quoted include in native.hpp an absolute include."""

    include_re = re.compile(r'(^\s*#\s*include\s+")([^"\n]+)("\s*$)', re.MULTILINE)

    def resolve(match: re.Match[str]) -> str:
        include = match.group(2)
        candidate = Path(include)
        if candidate.is_absolute():
            return match.group(0)
        resolved = (native_dir / candidate).resolve()
        if not resolved.is_file():
            raise ValueError(f"native quoted include is not a real file: {include}")
        return f'{match.group(1)}{_include_path(str(resolved))}{match.group(3)}'

    return include_re.sub(resolve, source)


def _capture(device: str, encoder: str, texture: str, stage: str, mip: int | None = None) -> str:
    suffix = "" if mip is None else f", {mip}"
    return f'mp6::qa::ao_buffers::capture({device}, {encoder}, {texture}, "{stage}"{suffix});'


def instrument_native(source: str, native_dir: str | Path | None = None) -> str:
    """Instrument a private ``xegtao/native.hpp`` copy.

    The native pipeline's split View-Z path is the only path accepted by the
    buffer contract.  Texture copies are recorded immediately after each
    producer stage, before an aliased resource is overwritten by its next
    role.  The backend header itself remains external to this source copy.
    """

    marker = "QA_AO_BUFFER_CAPTURE_NATIVE"
    if marker in source:
        raise ValueError("AO buffer native transform already installed")
    base = Path(native_dir) if native_dir is not None else NATIVE_DIR
    transformed = _resolve_native_includes(source, base)

    # Keep CopySrc private to this generated native header.  This intentionally
    # covers the four physical XeGTAO images (and harmlessly the LUT image).
    transformed = replace_once(
        transformed,
        "        const wgpu::TextureDescriptor desc{\n"
        "            .label = label, .usage = usage, .size = {width, height, 1}, .format = format, .mipLevelCount = mips,\n",
        "        usage = usage | wgpu::TextureUsage::CopySrc;\n"
        "        const wgpu::TextureDescriptor desc{\n"
        "            .label = label, .usage = usage, .size = {width, height, 1}, .format = format, .mipLevelCount = mips,\n",
        label="native image CopySrc",
    )

    # Refuse the alternate implementation at the point where selected capture
    # would otherwise produce an incomplete inventory.  The runner rejects the
    # same settings before launch; this is a second fail-closed guard in the
    # private translation unit.
    canonical = (
        '        raster(encoder, canonical, canonicalGroup, s.rawView, "XeGTAO canonical depth");\n'
    )
    canonical_guard = (
        '            if (std::getenv("MP6_QA_AO_BUFFERS_TICK") &&\n'
        '                (batchedCompute || policy.resolution != Resolution::Full ||\n'
        '                 workingDepthFormat != wgpu::TextureFormat::R32Float)) {\n'
        '                std::fprintf(stderr, "[AO-BUFFERS-ERROR] capture requires split/full/r32\\n");\n'
        '                std::abort();\n'
        '            }\n'
        + canonical
        + "        "
        + _capture("device", "encoder", "s.rawDepth", "canonical_depth")
        + "\n"
    )
    transformed = replace_once(transformed, canonical, canonical_guard, label="canonical capture")

    prefilter = (
        '        dispatch(encoder, 0, r, s, input == DepthInput::CanonicalNdc ? externalPrefilter : s.prefilter,\n'
        '                 1, 16, 16, "XeGTAO depth MIPs");\n'
    )
    mip_captures = "".join(
        "        " + _capture("device", "encoder", "s.depthMips", "depth_mip", mip) + "\n"
        for _name, mip in STAGES[2:7]
    )
    transformed = replace_once(transformed, prefilter, prefilter + mip_captures, label="depth mip captures")

    normals = (
        '        dispatch(encoder, 1, r, s, input == DepthInput::CanonicalNdc ? externalNormal : s.normal,\n'
        '                 3, 8, 8, "XeGTAO normals");\n'
    )
    transformed = replace_once(
        transformed,
        normals,
        normals + "        " + _capture("device", "encoder", "s.normals", "normals") + "\n",
        label="normal capture",
    )

    main = (
        '        dispatch(encoder, 2, r, s, s.main, 2, 8, 8, "XeGTAO main");\n'
    )
    transformed = replace_once(
        transformed,
        main,
        main
        + "        " + _capture("device", "encoder", "s.ao[0]", "raw_ao") + "\n"
        + "        " + _capture("device", "encoder", "s.edges", "edges") + "\n",
        label="raw/edge captures",
    )

    final = (
        '        dispatch(encoder, 4, r, s, s.denoise[1 - r.finalAoIndex], 3, 16, 8, "XeGTAO final denoise");\n'
    )
    transformed = replace_once(
        transformed,
        final,
        final + "        " + _capture("device", "encoder", "s.ao[r.finalAoIndex]", "final_ao") + "\n",
        label="final AO capture",
    )

    return "/* " + marker + " */\n" + transformed


def instrument_ambient(
    source: str,
    private_native_path: str | Path,
    backend_header_path: str | Path,
) -> str:
    """Instrument a private ambient_occlusion.cpp translation unit."""

    marker = "QA_AO_BUFFER_CAPTURE_AMBIENT"
    if marker in source:
        raise ValueError("AO buffer ambient transform already installed")
    native_include = _include_path(str(private_native_path))
    backend_include = _include_path(str(backend_header_path))

    transformed = replace_once(
        source,
        '#include "xegtao/native.hpp"\n',
        f'#include "{backend_include}"\n#include "{native_include}"\n',
        label="private native include",
    )
    transformed = replace_once(
        transformed,
        "namespace {\n",
        'extern "C" long mp6_tick_count;\n\nnamespace {\n',
        label="tick declaration",
    )
    transformed = replace_once(
        transformed,
        "    uint64_t frame = UINT64_MAX; /* game-thread only */\n",
        "    uint64_t frame = UINT64_MAX; /* game-thread only */\n"
        "    uint64_t qaTick = 0; /* copied on the game thread */\n"
        "    int32_t qaCamera = -1; /* copied on the game thread */\n",
        label="job metadata fields",
    )
    transformed = replace_once(
        transformed,
        '                .label = "MP6 GTAO decal-free depth",\n'
        "                .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,\n",
        '                .label = "MP6 GTAO decal-free depth",\n'
        "                .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc,\n",
        label="private clean depth CopySrc",
    )
    transformed = replace_once(
        transformed,
        "    job->frame = g_frame;\n",
        "    job->frame = g_frame;\n"
        "    job->qaTick = static_cast<uint64_t>(mp6_tick_count);\n"
        "    job->qaCamera = static_cast<int32_t>(view->camera);\n",
        label="game-thread job metadata",
    )

    encode_anchor = (
        "        g_xegtao.encode(ctx.device, ctx.queue, encoder, job.upstream,\n"
        "            job.depth, *upstreamConstants, adapter, job.canonicalDepth ?\n"
        "                mp6::xegtao::DepthInput::CanonicalNdc : mp6::xegtao::DepthInput::ViewZ);\n"
    )
    encode_replacement = (
        "        if (std::getenv(\"MP6_QA_AO_BUFFERS_TICK\") &&\n"
        "            job.canonicalDepth) {\n"
        "            std::fprintf(stderr, \"[AO-BUFFERS-ERROR] canonical preparation is unsupported\\n\");\n"
        "            std::abort();\n"
        "        }\n"
        "        mp6::qa::ao_buffers::Metadata qaMetadata{};\n"
        "        qaMetadata.frame = job.frame;\n"
        "        qaMetadata.tick = static_cast<int64_t>(job.qaTick);\n"
        "        qaMetadata.camera = job.qaCamera;\n"
        "        qaMetadata.width = width;\n"
        "        qaMetadata.height = height;\n"
        "        std::memcpy(qaMetadata.params.data(), &job.params, sizeof(job.params));\n"
        "        std::memcpy(qaMetadata.constants.data(), &*upstreamConstants, sizeof(*upstreamConstants));\n"
        "        std::memcpy(qaMetadata.adapter.data(), &adapter, sizeof(adapter));\n"
        "        const bool qaBufferJob = mp6::qa::ao_buffers::begin_job(qaMetadata);\n"
        "        if (qaBufferJob)\n"
        "            mp6::qa::ao_buffers::capture(ctx.device, encoder, job.cleanDepth, \"clean_depth\");\n"
        + encode_anchor
        + "        if (qaBufferJob) mp6::qa::ao_buffers::end_job();\n"
    )
    transformed = replace_once(transformed, encode_anchor, encode_replacement, label="upstream capture boundary")

    transformed = replace_once(
        transformed,
        '    if (std::getenv("MP6_AO_DIAG"))\n',
        '    mp6::qa::ao_buffers::finish();\n'
        '    if (std::getenv("MP6_AO_DIAG"))\n',
        label="AO buffer finish after synchronize",
    )
    return "/* " + marker + " */\n" + transformed
