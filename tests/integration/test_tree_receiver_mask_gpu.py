"""Procedural GPU regression for the tree receiver replay.

The native b01_m240 replay is intentionally not extracted here.  This small
WebGPU scene models the observable contract of that draw:

* the tree writes RGB into the private AO receiver target, but not alpha;
* the draw uses a read-only scene depth attachment and cannot change it;
* a nearer opaque object rejects the tree receiver mask, so that object keeps
  its own AO;
* a later alpha-only layer can mark its coverage without erasing the earlier
  tree RGB, and wins in the composite;
* the MSAA case includes a deliberately displaced private depth sample.  The
  compensated path represents the native viewer-facing polygon offset; the
  unbiased path is retained as a regression witness for the failure.

This is an opt-in WebGPU test, not native-game evidence.  The native runner
still owns replay ordering, model admission and the actual b01_m240 draw.

Run with the packaged QA dependency, for example::

    python tests/integration/test_tree_receiver_mask_gpu.py -v
"""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/ao-gpu-test-deps"))

try:  # Keep this opt-in test importable by CPU-only discovery.
    import numpy as np
    import wgpu
except ImportError:  # pragma: no cover - exercised only without QA extras
    np = None
    wgpu = None


HAS_GPU_DEPS = np is not None and wgpu is not None


@unittest.skipUnless(
    HAS_GPU_DEPS,
    "optional numpy/wgpu==0.32.0 GPU dependencies are unavailable",
)
class TreeReceiverMaskGPU(unittest.TestCase):
    width = 128  # 4 * RGBA8 bytes per row is 512, satisfying WebGPU alignment.
    height = 64
    # Keep module import safe when the optional GPU dependencies are absent;
    # the skip decorator is evaluated only after the class is constructed.
    visibility = 0.4

    # Values are exactly representable in the normalized 8-bit target.  That
    # makes a failed RGB-only write distinguishable from target quantization.
    scene_rgb = (64 / 255.0, 160 / 255.0, 224 / 255.0)
    trunk_rgb = (128 / 255.0, 64 / 255.0, 32 / 255.0)
    opaque_rgb = (224 / 255.0, 48 / 255.0, 32 / 255.0)
    alpha_rgb = (32 / 255.0, 192 / 255.0, 80 / 255.0)

    @classmethod
    def setUpClass(cls):
        cls.adapter = wgpu.gpu.request_adapter_sync(
            power_preference="high-performance"
        )
        if cls.adapter is None:
            raise unittest.SkipTest("wgpu could not find a high-performance adapter")
        cls.device = cls.adapter.request_device_sync()

    @staticmethod
    def _rect_mask(width, height, x0, x1, y0, y1):
        y, x = np.indices((height, width))
        ndc_x = (x.astype(np.float32) + 0.5) * (2.0 / width) - 1.0
        ndc_y = 1.0 - (y.astype(np.float32) + 0.5) * (2.0 / height)
        return (
            (ndc_x > x0)
            & (ndc_x < x1)
            & (ndc_y > y0)
            & (ndc_y < y1)
        )

    @classmethod
    def _shader(cls, reverse, samples):
        if reverse:
            background_z, trunk_z, opaque_z, alpha_z = 0.18, 0.48, 0.76, 0.62
            compare = "greater-equal"
            private_delta = -0.001 if samples > 1 else 0.0
            compensated_bias = 0.0025 if samples > 1 else 0.0
        else:
            background_z, trunk_z, opaque_z, alpha_z = 0.82, 0.52, 0.24, 0.38
            compare = "less-equal"
            private_delta = 0.001 if samples > 1 else 0.0
            compensated_bias = -0.0025 if samples > 1 else 0.0

        def f(value):
            return f"{value:.9f}"

        return f"""
struct VOut {{ @builtin(position) position: vec4f }};
struct MaskOut {{
    @location(0) color: vec4f,
    @builtin(frag_depth) depth: f32,
}};

fn rect(i:u32, x0:f32, x1:f32, y0:f32, y1:f32, z:f32) -> VOut {{
    let p = array<vec2f,6>(
        vec2f(x0, y1), vec2f(x1, y1), vec2f(x0, y0),
        vec2f(x0, y0), vec2f(x1, y1), vec2f(x1, y0));
    var out:VOut;
    out.position = vec4f(p[i], z, 1.0);
    return out;
}}

@vertex fn background_vs(@builtin(vertex_index) i:u32) -> VOut {{
    return rect(i, -1.0, 1.0, -1.0, 1.0, {f(background_z)});
}}
@vertex fn trunk_vs(@builtin(vertex_index) i:u32) -> VOut {{
    return rect(i, -0.45, 0.15, -0.50, 0.50, {f(trunk_z)});
}}
@vertex fn opaque_vs(@builtin(vertex_index) i:u32) -> VOut {{
    return rect(i, -0.05, 0.45, -0.35, 0.35, {f(opaque_z)});
}}
@vertex fn alpha_vs(@builtin(vertex_index) i:u32) -> VOut {{
    return rect(i, -0.25, 0.25, -0.20, 0.20, {f(alpha_z)});
}}

@vertex fn fullscreen_vs(@builtin(vertex_index) i:u32) -> VOut {{
    let p = array<vec2f,3>(vec2f(-1.0,1.0), vec2f(-1.0,-3.0), vec2f(3.0,1.0));
    var out:VOut;
    out.position = vec4f(p[i], 0.0, 1.0);
    return out;
}}

@fragment fn background_fs() -> @location(0) vec4f {{
    return vec4f({f(cls.scene_rgb[0])}, {f(cls.scene_rgb[1])},
                 {f(cls.scene_rgb[2])}, 1.0);
}}
@fragment fn trunk_fs() -> @location(0) vec4f {{
    return vec4f({f(cls.trunk_rgb[0])}, {f(cls.trunk_rgb[1])},
                 {f(cls.trunk_rgb[2])}, 1.0);
}}
@fragment fn opaque_fs() -> @location(0) vec4f {{
    return vec4f({f(cls.opaque_rgb[0])}, {f(cls.opaque_rgb[1])},
                 {f(cls.opaque_rgb[2])}, 1.0);
}}
@fragment fn alpha_fs() -> @location(0) vec4f {{
    return vec4f({f(cls.alpha_rgb[0])}, {f(cls.alpha_rgb[1])},
                 {f(cls.alpha_rgb[2])}, 0.5);
}}

// RGB-only tree replay.  The two generated entry points differ only in the
// explicit private-center displacement compensation used by the MSAA case.
@fragment fn trunk_mask_unbiased_fs(@builtin(position) p:vec4f) -> MaskOut {{
    return MaskOut(
        vec4f({f(cls.trunk_rgb[0])}, {f(cls.trunk_rgb[1])},
               {f(cls.trunk_rgb[2])}, 1.0),
        clamp(p.z + {f(private_delta)}, 0.0, 1.0));
}}
@fragment fn trunk_mask_fixed_fs(@builtin(position) p:vec4f) -> MaskOut {{
    return MaskOut(
        vec4f({f(cls.trunk_rgb[0])}, {f(cls.trunk_rgb[1])},
               {f(cls.trunk_rgb[2])}, 1.0),
        clamp(p.z + {f(private_delta + compensated_bias)}, 0.0, 1.0));
}}
@fragment fn alpha_mask_fs() -> @location(0) vec4f {{
    return vec4f(0.0, 0.0, 0.0, 1.0);
}}

@group(0) @binding(0) var scene_color:texture_2d<f32>;
@group(0) @binding(1) var receiver_mask:texture_2d<f32>;
@fragment fn composite_fs(@builtin(position) p:vec4f) -> @location(0) vec4f {{
    let at = vec2i(p.xy);
    let mask = textureLoad(receiver_mask, at, 0);
    // A later alpha-only layer is already represented in scene_color.  The
    // marker therefore preserves that scene pixel instead of applying AO.
    if (mask.a > 0.5) {{ return vec4f(0.0, 0.0, 0.0, 1.0); }}
    // This is the production blend payload: the destination scene is already
    // in the target, RGB is restored from the private receiver, and alpha is
    // the background visibility.  The native pipeline uses src=one,dst=srcA.
    let visibility = {f(float(cls.visibility))};
    return vec4f(mask.rgb * (1.0 - visibility), visibility);
}}
""", compare

    def _texture(self, width, height, fmt, usage, samples=1):
        return self.device.create_texture(
            size=(width, height, 1),
            format=fmt,
            usage=usage,
            sample_count=samples,
        )

    def _read(self, texture, dtype, channels=1):
        item = np.dtype(dtype).itemsize
        row_bytes = self.width * channels * item
        raw = self.device.queue.read_texture(
            {"texture": texture},
            {"bytes_per_row": row_bytes, "rows_per_image": self.height},
            (self.width, self.height, 1),
        )
        shape = (self.height, self.width, channels) if channels != 1 else (
            self.height,
            self.width,
        )
        return np.frombuffer(raw, dtype=dtype).reshape(shape).copy()

    def _run(self, reverse, samples, include_late_alpha):
        d = self.device
        w, h = self.width, self.height
        shader_code, compare = self._shader(reverse, samples)
        module = d.create_shader_module(code=shader_code)

        render_usage = wgpu.TextureUsage.RENDER_ATTACHMENT
        sampled_usage = wgpu.TextureUsage.TEXTURE_BINDING
        readback_usage = wgpu.TextureUsage.COPY_SRC

        # The resolved color is the native scene continuation.  For 4x, the
        # actual render attachment is separate so the depth/MSAA contract is
        # exercised instead of merely using a single-sample mock.
        scene_color = self._texture(
            w,
            h,
            "rgba8unorm",
            render_usage | sampled_usage | readback_usage | wgpu.TextureUsage.COPY_DST,
        )
        scene_msaa = (
            self._texture(w, h, "rgba8unorm", render_usage, samples)
            if samples > 1
            else scene_color
        )
        scene_depth = self._texture(
            w,
            h,
            "depth32float",
            render_usage | sampled_usage,
            samples,
        )

        mask_unbiased = self._texture(
            w, h, "rgba8unorm", render_usage | sampled_usage | readback_usage
        )
        mask_fixed = self._texture(
            w, h, "rgba8unorm", render_usage | sampled_usage | readback_usage
        )
        mask_unbiased_msaa = (
            self._texture(w, h, "rgba8unorm", render_usage, samples)
            if samples > 1
            else mask_unbiased
        )
        mask_fixed_msaa = (
            self._texture(w, h, "rgba8unorm", render_usage, samples)
            if samples > 1
            else mask_fixed
        )
        output = self._texture(
            w,
            h,
            "rgba8unorm",
            render_usage | readback_usage | wgpu.TextureUsage.COPY_DST,
        )
        depth_before = self._texture(w, h, "r32float", render_usage | readback_usage)
        depth_after = self._texture(w, h, "r32float", render_usage | readback_usage)

        solid_target = dict(format="rgba8unorm")
        solid_depth = dict(
            format="depth32float", depth_write_enabled=True, depth_compare=compare
        )

        def solid(entry, *, blend=None, depth_write=True):
            target = dict(solid_target)
            if blend is not None:
                target["blend"] = blend
            return d.create_render_pipeline(
                layout="auto",
                vertex=dict(module=module, entry_point=entry.replace("_fs", "_vs")),
                fragment=dict(module=module, entry_point=entry, targets=[target]),
                depth_stencil=dict(
                    format="depth32float",
                    depth_write_enabled=depth_write,
                    depth_compare=compare,
                ),
                primitive=dict(topology="triangle-list"),
                multisample=dict(count=samples),
            )

        background = solid("background_fs")
        trunk = solid("trunk_fs")
        opaque = solid("opaque_fs")
        alpha = solid(
            "alpha_fs",
            depth_write=False,
            blend=dict(
                color=dict(src_factor="src-alpha", dst_factor="one-minus-src-alpha", operation="add"),
                alpha=dict(src_factor="zero", dst_factor="one", operation="add"),
            ),
        )

        def mask_pipeline(entry, write_mask):
            return d.create_render_pipeline(
                layout="auto",
                vertex=dict(module=module, entry_point="trunk_vs" if entry.startswith("trunk") else "alpha_vs"),
                fragment=dict(
                    module=module,
                    entry_point=entry,
                    targets=[dict(format="rgba8unorm", write_mask=write_mask)],
                ),
                depth_stencil=dict(
                    format="depth32float",
                    depth_write_enabled=False,
                    depth_compare=compare,
                ),
                primitive=dict(topology="triangle-list"),
                multisample=dict(count=samples),
            )

        rgb = wgpu.ColorWrite.RED | wgpu.ColorWrite.GREEN | wgpu.ColorWrite.BLUE
        alpha_only = wgpu.ColorWrite.ALPHA
        trunk_unbiased = mask_pipeline("trunk_mask_unbiased_fs", rgb)
        trunk_fixed = mask_pipeline("trunk_mask_fixed_fs", rgb)
        alpha_mask = mask_pipeline("alpha_mask_fs", alpha_only)

        probe_type = "texture_depth_multisampled_2d" if samples > 1 else "texture_depth_2d"
        probe_code = f"""
@group(0) @binding(0) var scene_depth:{probe_type};
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {{
    let p=array<vec2f,3>(vec2f(-1.0,1.0),vec2f(-1.0,-3.0),vec2f(3.0,1.0));
    return vec4f(p[i],0.0,1.0);
}}
@fragment fn fs(@builtin(position) p:vec4f)->@location(0) f32 {{
    return textureLoad(scene_depth, vec2i(p.xy), 0);
}}
"""
        probe_module = d.create_shader_module(code=probe_code)
        probe_pipeline = d.create_render_pipeline(
            layout="auto",
            vertex=dict(module=probe_module, entry_point="vs"),
            fragment=dict(module=probe_module, entry_point="fs", targets=[dict(format="r32float")]),
        )
        probe_group = d.create_bind_group(
            layout=probe_pipeline.get_bind_group_layout(0),
            entries=[dict(binding=0, resource=scene_depth.create_view())],
        )

        def color_attachment(msaa, resolved, clear=(0, 0, 0, 0)):
            return dict(
                view=msaa.create_view(),
                resolve_target=resolved.create_view() if samples > 1 else None,
                load_op="clear",
                store_op="store",
                clear_value=clear,
            )

        clear_depth = 0.0 if reverse else 1.0
        encoder = d.create_command_encoder()
        scene_pass = encoder.begin_render_pass(
            color_attachments=[color_attachment(scene_msaa, scene_color)],
            depth_stencil_attachment=dict(
                view=scene_depth.create_view(),
                depth_load_op="clear",
                depth_store_op="store",
                depth_clear_value=clear_depth,
            ),
        )
        scene_pass.set_pipeline(background)
        scene_pass.draw(6)
        scene_pass.set_pipeline(trunk)
        scene_pass.draw(6)
        scene_pass.set_pipeline(opaque)
        scene_pass.draw(6)
        if include_late_alpha:
            # Alpha is deliberately later than the opaque pass.  It may blend
            # into scene color, but it cannot write the shared scene depth.
            scene_pass.set_pipeline(alpha)
            scene_pass.draw(6)
        scene_pass.end()

        def probe(target):
            p = encoder.begin_render_pass(
                color_attachments=[dict(
                    view=target.create_view(),
                    load_op="clear",
                    store_op="store",
                    clear_value=(0, 0, 0, 0),
                )]
            )
            p.set_pipeline(probe_pipeline)
            p.set_bind_group(0, probe_group)
            p.draw(3)
            p.end()

        probe(depth_before)

        def mask_pass(target_msaa, target, *, fixed, with_alpha):
            p = encoder.begin_render_pass(
                color_attachments=[color_attachment(target_msaa, target)],
                depth_stencil_attachment=dict(
                    view=scene_depth.create_view(), depth_read_only=True
                ),
            )
            p.set_pipeline(trunk_fixed if fixed else trunk_unbiased)
            p.draw(6)
            if with_alpha:
                p.set_pipeline(alpha_mask)
                p.draw(6)
            p.end()

        mask_pass(mask_unbiased_msaa, mask_unbiased, fixed=False, with_alpha=False)
        mask_pass(mask_fixed_msaa, mask_fixed, fixed=True, with_alpha=include_late_alpha)

        # Composite is the same destination-preserving blend contract used by
        # the native AO path.  The target starts as the already-rendered scene.
        encoder.copy_texture_to_texture(
            {"texture": scene_color}, {"texture": output}, (w, h, 1)
        )
        composite_layout = d.create_bind_group_layout(entries=[
            dict(binding=0, visibility=wgpu.ShaderStage.FRAGMENT,
                 texture=dict(sample_type="float", view_dimension="2d")),
            dict(binding=1, visibility=wgpu.ShaderStage.FRAGMENT,
                 texture=dict(sample_type="float", view_dimension="2d")),
        ])
        composite_group = d.create_bind_group(
            layout=composite_layout,
            entries=[
                dict(binding=0, resource=scene_color.create_view()),
                dict(binding=1, resource=mask_fixed.create_view()),
            ],
        )
        composite = d.create_render_pipeline(
            layout=d.create_pipeline_layout(bind_group_layouts=[composite_layout]),
            vertex=dict(module=module, entry_point="fullscreen_vs"),
            fragment=dict(
                module=module,
                entry_point="composite_fs",
                targets=[dict(
                    format="rgba8unorm",
                    write_mask=rgb,
                    blend=dict(
                        color=dict(src_factor="one", dst_factor="src-alpha", operation="add"),
                        alpha=dict(src_factor="zero", dst_factor="one", operation="add"),
                    ),
                )],
            ),
        )
        composite_pass = encoder.begin_render_pass(
            color_attachments=[dict(
                view=output.create_view(), load_op="load", store_op="store"
            )]
        )
        composite_pass.set_pipeline(composite)
        composite_pass.set_bind_group(0, composite_group)
        composite_pass.draw(3)
        composite_pass.end()
        probe(depth_after)
        d.queue.submit([encoder.finish()])

        return {
            "scene": self._read(scene_color, np.uint8, channels=4).astype(np.float32) / 255.0,
            "unbiased": self._read(mask_unbiased, np.uint8, channels=4),
            "mask": self._read(mask_fixed, np.uint8, channels=4),
            "output": self._read(output, np.uint8, channels=4).astype(np.float32) / 255.0,
            "depth_before": self._read(depth_before, np.float32),
            "depth_after": self._read(depth_after, np.float32),
        }

    def test_rgb_only_tree_replay_restores_trunk_and_preserves_scene_depth(self):
        """The tree is unshaded while ordinary scene pixels keep AO."""
        trunk_core = self._rect_mask(self.width, self.height, -0.38, -0.12, -0.32, 0.32)
        overlap_core = self._rect_mask(self.width, self.height, 0.00, 0.10, -0.24, 0.24)
        opaque_core = self._rect_mask(self.width, self.height, 0.22, 0.38, -0.24, 0.24)

        for reverse in (False, True):
            for samples in (1, 4):
                with self.subTest(reverse_z=reverse, samples=samples):
                    result = self._run(reverse, samples, include_late_alpha=False)
                    mask = result["mask"]
                    output = result["output"][..., :3]
                    scene = result["scene"][..., :3]

                    # RGB-only means alpha remains the cleared marker for the
                    # entire trunk replay, not merely at one center pixel.
                    np.testing.assert_array_equal(mask[..., 3], 0)
                    np.testing.assert_allclose(
                        mask[trunk_core, :3] / 255.0,
                        np.broadcast_to(self.trunk_rgb, (int(trunk_core.sum()), 3)),
                        atol=1 / 255,
                    )
                    np.testing.assert_array_equal(mask[overlap_core, :3], 0)

                    # Restoring the same RGB that was rendered into the scene
                    # makes trunk contact seams disappear from the composite.
                    np.testing.assert_allclose(output[trunk_core], scene[trunk_core], atol=2 / 255)
                    # The nearer opaque foreground rejected the tree mask and
                    # therefore retains the AO visibility multiplier.
                    np.testing.assert_allclose(
                        output[overlap_core],
                        scene[overlap_core] * self.visibility,
                        atol=3 / 255,
                    )
                    np.testing.assert_allclose(
                        output[opaque_core],
                        scene[opaque_core] * self.visibility,
                        atol=3 / 255,
                    )
                    np.testing.assert_array_equal(
                        result["depth_before"], result["depth_after"]
                    )

                    if samples == 4:
                        # The synthetic sample-zero/private-center displacement
                        # is intentionally large enough to fail without the
                        # compensated polygon offset.
                        np.testing.assert_array_equal(
                            result["unbiased"][trunk_core, :3], 0
                        )
                    else:
                        np.testing.assert_allclose(
                            result["unbiased"][trunk_core, :3] / 255.0,
                            np.broadcast_to(self.trunk_rgb, (int(trunk_core.sum()), 3)),
                            atol=1 / 255,
                        )

    def test_later_alpha_marker_wins_without_erasing_tree_rgb(self):
        """A later alpha coverage marker follows the RGB trunk receiver."""
        alpha_visible = self._rect_mask(self.width, self.height, -0.20, -0.10, -0.12, 0.12)
        alpha_blocked = self._rect_mask(self.width, self.height, 0.02, 0.10, -0.12, 0.12)
        opaque_core = self._rect_mask(self.width, self.height, 0.22, 0.38, -0.24, 0.24)

        for reverse in (False, True):
            for samples in (1, 4):
                with self.subTest(reverse_z=reverse, samples=samples):
                    result = self._run(reverse, samples, include_late_alpha=True)
                    mask = result["mask"]
                    output = result["output"][..., :3]
                    scene = result["scene"][..., :3]

                    # Alpha writes are restricted to the later layer's visible
                    # pixels.  Its alpha-only pipeline leaves prior trunk RGB
                    # intact underneath the marker.
                    np.testing.assert_array_equal(mask[alpha_visible, 3], 255)
                    np.testing.assert_array_equal(mask[alpha_blocked, 3], 0)
                    np.testing.assert_allclose(
                        mask[alpha_visible, :3] / 255.0,
                        np.broadcast_to(self.trunk_rgb, (int(alpha_visible.sum()), 3)),
                        atol=1 / 255,
                    )

                    # The marker makes the composite preserve the later scene
                    # color; blocked alpha does not steal the opaque object's AO.
                    np.testing.assert_allclose(output[alpha_visible], scene[alpha_visible], atol=2 / 255)
                    np.testing.assert_allclose(
                        output[alpha_blocked],
                        scene[alpha_blocked] * self.visibility,
                        atol=3 / 255,
                    )
                    np.testing.assert_allclose(
                        output[opaque_core],
                        scene[opaque_core] * self.visibility,
                        atol=3 / 255,
                    )
                    np.testing.assert_array_equal(
                        result["depth_before"], result["depth_after"]
                    )


if __name__ == "__main__":  # pragma: no cover - direct optional harness entry point
    unittest.main(verbosity=2)
