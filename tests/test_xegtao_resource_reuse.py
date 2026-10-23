"""Native allocation and per-pass access contracts for XeGTAO texture reuse.

The independent GPU harness executes both allocation strategies and compares
their output bits. These checks ensure the production bindings use that same
non-overlapping lifetime plan rather than only validating the test renderer.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/gx/xegtao/native.hpp"


def block(source, marker):
    return source.split(marker, 1)[1].split("});", 1)[0]


class XeGTAOResourceReuse(unittest.TestCase):
    def test_parent_retires_upstream_resources_on_legacy_transition(self):
        source = (ROOT / "src/gx/ambient_occlusion.cpp").read_text()
        encode = source.split("void encode(", 1)[1].split("void composite(", 1)[0]
        previous = encode.index("const bool previousUpstream = job.upstreamSignal;")
        retire = encode.index("if (!upstream && previousUpstream) job.upstream = {};")
        legacy = encode.index("if (!upstream && (!job.ao[0]")
        self.assertLess(previous, retire)
        self.assertLess(retire, legacy)
        # A normal XeGTAO frame keeps its textures and per-buffer upload cache.
        self.assertEqual(encode.count("job.upstream = {};"), 1)

    def test_four_allocations_with_combined_usage_and_exact_format_aliases(self):
        source = SOURCE.read_text()
        scratch = source.split("    std::shared_ptr<Scratch> create_scratch(", 1)[1].split(
            "    std::shared_ptr<Scratch> acquire_scratch(", 1
        )[0]
        allocations = re.findall(r"scratch->(\w+(?:\[\d+\])?)\s*=\s*image\(", scratch)
        self.assertCountEqual(allocations, ["rawDepth", "depthMips", "normals", "ao[0]"])
        self.assertEqual(scratch.count("image("), 4)
        self.assertIn("const auto computeUsage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding;", scratch)
        raw = scratch.split("scratch->rawDepth = image(", 1)[1].split(";", 1)[0]
        self.assertIn("wgpu::TextureFormat::R32Float", raw)
        self.assertIn("computeUsage | wgpu::TextureUsage::RenderAttachment", raw)
        normals = scratch.split("scratch->normals = image(", 1)[1].split(";", 1)[0]
        self.assertIn("wgpu::TextureFormat::R32Uint, computeUsage", normals)
        for alias in ("scratch->edges = scratch->rawDepth;", "scratch->edgeView = scratch->rawView;",
                      "scratch->ao[1] = scratch->normals;", "scratch->aoViews[1] = scratch->normalView;"):
            self.assertIn(alias, scratch)

    def test_per_job_uniforms_and_common_group_are_separate_from_shared_scratch(self):
        source = SOURCE.read_text()
        resources = source.split("struct Resources {", 1)[1].split("};", 1)[0]
        self.assertIn("wgpu::Buffer constants, adapter;", resources)
        self.assertIn("UniformUploadCache<XeGTAO::GTAOConstants> constantsUpload", resources)
        self.assertIn("UniformUploadCache<AdapterParams> adapterUpload", resources)
        self.assertIn("wgpu::BindGroup common;", resources)
        self.assertIn("std::shared_ptr<Scratch> scratch;", resources)
        self.assertNotIn("wgpu::Texture", resources)
        self.assertNotIn("wgpu::BindGroup prefilter", resources)
        self.assertNotIn("wgpu::BindGroup normal", resources)
        self.assertNotIn("wgpu::BindGroup main", resources)
        self.assertNotIn("wgpu::TextureView", resources)
        dispatch = source.split("    void dispatch(", 1)[1].split("    bool initialization_failed", 1)[0]
        self.assertIn("const Scratch &scratch", dispatch)
        self.assertIn("pass.SetBindGroup(0, r.common);", dispatch)
        self.assertIn("scratch.width", dispatch)
        self.assertIn("scratch.height", dispatch)
        encode = source.split("    void encode(", 1)[1]
        self.assertIn("r.scratch = acquire_scratch(device, width, height);", encode)
        self.assertIn("const Scratch &s = *r.scratch;", encode)

    def test_hilbert_lut_is_pipeline_owned_and_uploaded_once_outside_allocate(self):
        source = SOURCE.read_text()
        self.assertIn("wgpu::Texture hilbertLut;", source)
        self.assertIn("wgpu::TextureView hilbertView;", source)
        self.assertIn(
            "wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst",
            source,
        )
        initializer = source.split("    bool ensure_hilbert_lut(", 1)[1].split(
            "    void allocate(", 1
        )[0]
        self.assertEqual(initializer.count("WriteTexture("), 1)
        self.assertIn("if (hilbertUploaded) return true;", initializer)
        self.assertIn(
            "values[y * XE_HILBERT_WIDTH + x] = XeGTAO::HilbertIndex(x, y);",
            initializer,
        )
        allocate = source.split("    void allocate(", 1)[1].split("    static void raster(", 1)[0]
        self.assertNotIn("WriteTexture(", allocate)
        self.assertIn("ensure_hilbert_lut(device, device.GetQueue())", source)
        self.assertIn("{.binding = 2, .textureView = hilbertView}", source)

    def test_actual_stage_bindings_never_sample_their_storage_output(self):
        source = SOURCE.read_text()
        identities = {"rawView": ("rawDepth", None), "normalView": ("normals", None),
                      "hilbertView": ("hilbert", None), "aoViews[0]": ("ao0", None),
                      "depthView": ("mips", None)}
        # Resolve production view aliases, rather than assuming that roles with
        # different names represent distinct physical textures.
        for destination, origin in re.findall(
            r"scratch->(\w+(?:\[\d+\])?)\s*=\s*scratch->(\w+(?:\[\d+\])?);", source
        ):
            if origin in identities:
                identities[destination] = identities[origin]
        for mip in range(5):
            identities[f"mipViews[{mip}]"] = ("mips", mip)

        def overlaps(left, right):
            return left[0] == right[0] and (left[1] is None or right[1] is None or left[1] == right[1])

        for name, indices in (("prefilter", (None,)), ("normal", (None,)),
                              ("main", (None,)), ("denoise", (0, 1))):
            layout = block(source, f"{name}Layout = layout(")
            read_bindings = set(map(int, re.findall(r"texture\((\d+),", layout)))
            write_bindings = set(map(int, re.findall(r"storage\((\d+),", layout)))
            resources = block(source, f"scratch->{name}{'[i]' if name == 'denoise' else ''} = bindings(")
            bindings = re.findall(r"\.binding\s*=\s*(\d+),\s*\.textureView\s*=\s*(?:scratch->)?([^},]+)", resources)
            self.assertEqual({int(binding) for binding, _ in bindings}, read_bindings | write_bindings)
            for index in indices:
                with self.subTest(stage=name, ping_pong=index):
                    reads, writes = [], []
                    for binding, view in bindings:
                        view = re.sub(r"\s+", "", view)
                        if index is not None:
                            view = view.replace("[1-i]", f"[{1-index}]").replace("[i]", f"[{index}]")
                        physical = identities[view]
                        (reads if int(binding) in read_bindings else writes).append(physical)
                    self.assertFalse(any(overlaps(read, write) for read in reads for write in writes),
                                     f"{name} aliases a sampled texture with storage output")
                    self.assertFalse(any(overlaps(write, other) for i, write in enumerate(writes)
                                         for other in writes[i+1:]), "storage outputs overlap")

    def test_producer_consumer_order_and_bind_group_isolation(self):
        source = SOURCE.read_text()
        encode = source.split("    void encode(", 1)[1]
        operations = re.findall(r"(?:raster|dispatch)\(encoder,\s*([^,]+)", encode)
        self.assertEqual(operations, ["canonical", "0", "1", "2", "3", "4"])
        dispatch = source.split("    void dispatch(", 1)[1].split("public:", 1)[0]
        self.assertIn("auto pass = encoder.BeginComputePass(&desc);", dispatch)
        self.assertIn("for (uint32_t gap = 1; gap < groupIndex; ++gap) pass.SetBindGroup(gap, emptyGroup);", dispatch)
        self.assertIn("pass.SetBindGroup(groupIndex, group);", dispatch)
        self.assertEqual(dispatch.count("pass.End();"), 1)


if __name__ == "__main__":
    unittest.main()
