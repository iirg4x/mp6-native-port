"""QA-only AO ownership transforms for depth snapshot attribution.

These transforms are consumed by the isolated frame-profile renderer.  They
never edit the Aurora or game checkout; every replacement is deliberately
strict so a source-layout drift fails closed instead of producing an
ambiguous profile.
"""


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(
            f"expected one source anchor: {old[:100]!r}, got {text.count(old)}"
        )
    return text.replace(old, new)


def instrument_ao_depth_snapshot(text):
    """Bracket both AO depth-resolve call sites with a scoped request.

    The scope ends before the caller consumes the targets.  That is important:
    the request is only an admission hint for the immediately nested
    ``resolve_pass`` and cannot leak into a later unrelated resolve.
    """
    if "AoDepthSnapshotScope" in text:
        raise ValueError("AO source is already depth-snapshot profiled")

    foliage = (
        "        if (!gfx::resolve_pass({.color=false,.depth=true},targets) || "
        "!targets.depth || !targets.width || !targets.height) return 0;"
    )
    foliage_replacement = (
        "        bool profileAoDepthResolved = false;\n"
        "        {\n"
        "            mp6_frame_profile::AoDepthSnapshotScope profileAoDepthScope;\n"
        "            profileAoDepthResolved = gfx::resolve_pass({.color=false,.depth=true},targets);\n"
        "        }\n"
        "        if (!profileAoDepthResolved || !targets.depth || !targets.width || !targets.height) return 0;"
    )
    text = replace_once(text, foliage, foliage_replacement)

    decal = (
        "    if (!gfx::resolve_pass({.color = false, .depth = true}, targets) || !targets.depth) {\n"
        "        decal = {};\n"
        "        return;\n"
        "    }"
    )
    decal_replacement = (
        "    bool profileAoDepthResolved = false;\n"
        "    {\n"
        "        mp6_frame_profile::AoDepthSnapshotScope profileAoDepthScope;\n"
        "        profileAoDepthResolved = gfx::resolve_pass({.color = false, .depth = true}, targets);\n"
        "    }\n"
        "    if (!profileAoDepthResolved || !targets.depth) {\n"
        "        decal = {};\n"
        "        return;\n"
        "    }"
    )
    return replace_once(text, decal, decal_replacement)


def instrument_tex_copy_conv_header(text):
    """Expose a profile-labelled snapshot entry point in the generated header."""
    if "snapshot_depth_profiled" in text:
        raise ValueError("tex_copy_conv header is already depth-snapshot profiled")
    anchor = (
        "void snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,\n"
        "                    const wgpu::TextureView& dst);"
    )
    addition = (
        anchor
        + "\n\n"
        + "// QA-only entry point; production callers retain snapshot_depth above.\n"
        + "void snapshot_depth_profiled(const wgpu::CommandEncoder& cmd,\n"
        + "                              const wgpu::TextureView& srcDepth, uint32_t msaaSamples,\n"
        + "                              const wgpu::TextureView& dst, const char* profileLabel);"
    )
    return replace_once(text, anchor, addition)


def instrument_tex_copy_conv(text):
    """Give the actual copy pass a caller-selected profiling label.

    The four-argument public function remains a wrapper with the existing
    generic label.  Thus non-AO snapshots are neither renamed nor counted a
    second time.
    """
    if "snapshot_depth_profiled" in text:
        raise ValueError("tex_copy_conv source is already depth-snapshot profiled")
    text = '#include "mp6_frame_profile.hpp"\n' + text
    signature = (
        "void snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,\n"
        "                    const wgpu::TextureView& dst) {"
    )
    profiled_signature = (
        "void snapshot_depth_profiled(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,\n"
        "                             const wgpu::TextureView& dst, const char* profileLabel) {"
    )
    text = replace_once(text, signature, profiled_signature)
    text = replace_once(
        text,
        'timestampWrites = webgpu::gpu_prof::pass_writes("Depth snapshot"),',
        "timestampWrites = webgpu::gpu_prof::pass_writes(profileLabel),",
    )
    wrapper = (
        "\nvoid snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,\n"
        "                    const wgpu::TextureView& dst) {\n"
        '  snapshot_depth_profiled(cmd, srcDepth, msaaSamples, dst, "Depth snapshot");\n'
        "}\n"
    )
    return replace_once(
        text,
        "\n} // namespace aurora::gfx::tex_copy_conv\n",
        wrapper + "\n} // namespace aurora::gfx::tex_copy_conv\n",
    )
