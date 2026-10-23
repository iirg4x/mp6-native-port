"""GPU regression for the MSAA sky private-mask depth tolerance.

This is an opt-in integration test, like ``test_ao_gpu.py``.  It models the
native sequence that matters for the regression: a sloping 4x depth
attachment is reduced to a single-sample, sample-zero seed, then the sky
coverage is replayed against that seed with depth writes disabled.  A nearer
foreground rectangle must continue to reject the replayed sky.

Run with the packaged QA dependency, for example::

    python tests/integration/test_sky_private_mask_gpu.py -v
"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/ao-gpu-test-deps"))
import numpy as np
import wgpu


class SkyPrivateMaskMsaa(unittest.TestCase):
    width = 256
    height = 128

    @classmethod
    def setUpClass(cls):
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if adapter is None:
            raise unittest.SkipTest("no WebGPU adapter")
        cls.device = adapter.request_device_sync()

    def _run(self, reverse):
        d = self.device
        w, h = self.width, self.height
        # The three vertices make a deliberately steep plane.  The source
        # fragment adds a small sample-zero displacement to model the 4x
        # EFB sample that the single-sample private seed actually captures.
        vertex = """
        struct VOut { @builtin(position) position: vec4f };
        @vertex fn sky_vs(@builtin(vertex_index) i:u32) -> VOut {
            let p=array<vec4f,3>(
                vec4f(-1.0,  1.0, 0.28, 1.0),
                vec4f(-1.0, -3.0, 0.64, 1.0),
                vec4f( 3.0,  1.0, 0.76, 1.0));
            var out:VOut; out.position=p[i]; return out;
        }
        @vertex fn blocker_vs(@builtin(vertex_index) i:u32) -> VOut {
            let p=array<vec4f,6>(
                vec4f(-0.24,  0.70, 0.13, 1.0),
                vec4f( 0.24,  0.70, 0.13, 1.0),
                vec4f(-0.24, -0.70, 0.13, 1.0),
                vec4f(-0.24, -0.70, 0.13, 1.0),
                vec4f( 0.24,  0.70, 0.13, 1.0),
                vec4f( 0.24, -0.70, 0.13, 1.0));
            var out:VOut; out.position=p[i]; return out;
        }
        @vertex fn seed_vs(@builtin(vertex_index) i:u32) -> VOut {
            let p=array<vec4f,3>(
                vec4f(-1.0,  1.0, 0.0, 1.0),
                vec4f(-1.0, -3.0, 0.0, 1.0),
                vec4f( 3.0,  1.0, 0.0, 1.0));
            var out:VOut; out.position=p[i]; return out;
        }
        @group(0) @binding(0) var source_msaa:texture_depth_multisampled_2d;
        @fragment fn source_fs(@builtin(position) p:vec4f,
                               @builtin(sample_index) sample:u32)
                               ->@builtin(frag_depth) f32 {
            let displacement = select(0.0, %s, sample == 0u);
            return clamp(p.z + displacement, 0.0, 1.0);
        }
        @fragment fn blocker_fs()->@builtin(frag_depth) f32 { return %s; }
        @fragment fn seed_fs(@builtin(position) p:vec4f)
                            ->@builtin(frag_depth) f32 {
            return textureLoad(source_msaa, vec2i(p.xy), 0);
        }
        struct MaskOut {
            @location(0) color: vec4f,
            @builtin(frag_depth) depth: f32,
        };
        @fragment fn mask_unbiased_fs()->@location(0) vec4f {
            return vec4f(1.0);
        }
        @fragment fn mask_compensated_fs(@builtin(position) p:vec4f)->MaskOut {
            // GX2SetPolygonOffset(-2,-0.5,...) is a viewer-facing offset.
            // Apply the equivalent conservative amount explicitly here: the
            // WebGPU test must cover both depth conventions, while GX2's
            // native sign is interpreted relative to its active Z mode.
            let slope = 0.36 / 256.0;
            let depth_ulp = 0.00000011920928955078125;
            let bias = %s * (0.5 * slope + 2.0 * depth_ulp);
            return MaskOut(vec4f(1.0), clamp(p.z + bias, 0.0, 1.0));
        }
        @group(0) @binding(0) var seed_depth:texture_depth_2d;
        @fragment fn probe_fs(@builtin(position) p:vec4f)->@location(0) f32 {
            return textureLoad(seed_depth, vec2i(p.xy), 0);
        }
        """ % (("-0.0004 + 0.0008*(p.x / 256.0)" if not reverse
                 else "0.0004 - 0.0008*(p.x / 256.0)"),
               ("0.13" if not reverse else "0.87"),
               ("-1.0" if not reverse else "1.0"))
        module = d.create_shader_module(code=vertex)

        source = d.create_texture(
            size=(w, h, 1), format="depth32float", sample_count=4,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING,
        )
        seed = d.create_texture(
            size=(w, h, 1), format="depth32float",
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.TEXTURE_BINDING,
        )
        baseline = d.create_texture(
            size=(w, h, 1), format="r8unorm",
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC,
        )
        fixed = d.create_texture(
            size=(w, h, 1), format="r8unorm",
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC,
        )
        before = d.create_texture(
            size=(w, h, 1), format="r32float",
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC,
        )
        after = d.create_texture(
            size=(w, h, 1), format="r32float",
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC,
        )

        no_color = dict(format="depth32float", depth_write_enabled=True,
                        depth_compare="always")
        source_pipeline = d.create_render_pipeline(
            layout="auto", vertex=dict(module=module, entry_point="sky_vs"),
            fragment=dict(module=module, entry_point="source_fs", targets=[]),
            depth_stencil=no_color, multisample=dict(count=4),
        )
        blocker_pipeline = d.create_render_pipeline(
            layout="auto", vertex=dict(module=module, entry_point="blocker_vs"),
            fragment=dict(module=module, entry_point="blocker_fs", targets=[]),
            depth_stencil=no_color, multisample=dict(count=4),
        )
        seed_pipeline = d.create_render_pipeline(
            layout="auto", vertex=dict(module=module, entry_point="seed_vs"),
            fragment=dict(module=module, entry_point="seed_fs", targets=[]),
            depth_stencil=no_color,
        )
        compare = "greater-equal" if reverse else "less-equal"

        def mask_pipeline(entry_point):
            return d.create_render_pipeline(
                layout="auto", vertex=dict(module=module, entry_point="sky_vs"),
                fragment=dict(module=module, entry_point=entry_point,
                              targets=[dict(format="r8unorm")]),
                # This is the WebGPU equivalent of the native private draw:
                # LEQUAL/GEQUAL and no depth writes.  The compensated entry
                # point carries GX2's viewer-facing -2/-0.5 amount in its
                # explicit frag_depth so forward and reverse-Z are identical.
                depth_stencil=dict(
                    format="depth32float", depth_write_enabled=False,
                    depth_compare=compare,
                ),
            )

        unbiased = mask_pipeline("mask_unbiased_fs")
        compensated = mask_pipeline("mask_compensated_fs")
        probe = d.create_render_pipeline(
            layout="auto", vertex=dict(module=module, entry_point="seed_vs"),
            fragment=dict(module=module, entry_point="probe_fs",
                          targets=[dict(format="r32float")]),
        )
        probe_group = probe.get_bind_group_layout(0)

        clear_depth = 0.0 if reverse else 1.0
        encoder = d.create_command_encoder()
        pass_info = dict(view=source.create_view(), depth_load_op="clear",
                         depth_store_op="store", depth_clear_value=clear_depth)
        render = encoder.begin_render_pass(color_attachments=[],
                                           depth_stencil_attachment=pass_info)
        render.set_pipeline(source_pipeline)
        render.draw(3)
        render.set_pipeline(blocker_pipeline)
        render.draw(6)
        render.end()

        source_group = seed_pipeline.get_bind_group_layout(0)
        source_bindings = d.create_bind_group(
            layout=source_group,
            entries=[dict(binding=0, resource=source.create_view())],
        )
        render = encoder.begin_render_pass(
            color_attachments=[],
            depth_stencil_attachment=dict(view=seed.create_view(),
                                          depth_load_op="clear",
                                          depth_store_op="store",
                                          depth_clear_value=clear_depth),
        )
        render.set_pipeline(seed_pipeline)
        render.set_bind_group(0, source_bindings)
        render.draw(3)
        render.end()

        def probe_pass(target):
            group = d.create_bind_group(
                layout=probe_group,
                entries=[dict(binding=0, resource=seed.create_view())],
            )
            p = encoder.begin_render_pass(
                color_attachments=[dict(view=target.create_view(), load_op="clear",
                                        store_op="store", clear_value=(0, 0, 0, 0))],
            )
            p.set_pipeline(probe)
            p.set_bind_group(0, group)
            p.draw(3)
            p.end()

        probe_pass(before)

        def mask_pass(target, pipeline):
            p = encoder.begin_render_pass(
                color_attachments=[dict(view=target.create_view(), load_op="clear",
                                        store_op="store", clear_value=(0, 0, 0, 0))],
                depth_stencil_attachment=dict(view=seed.create_view(),
                                              depth_read_only=True),
            )
            p.set_pipeline(pipeline)
            p.draw(3)
            p.end()

        mask_pass(baseline, unbiased)
        mask_pass(fixed, compensated)
        probe_pass(after)
        d.queue.submit([encoder.finish()])

        def read(texture, dtype, channels=1):
            raw = d.queue.read_texture(
                dict(texture=texture),
                dict(bytes_per_row=w * np.dtype(dtype).itemsize,
                     rows_per_image=h),
                (w, h, 1),
            )
            shape = (h, w, channels) if channels != 1 else (h, w)
            return np.frombuffer(raw, dtype=dtype).reshape(shape).copy()

        return (read(baseline, np.uint8), read(fixed, np.uint8),
                read(before, np.float32), read(after, np.float32))

    def test_sample_zero_slope_bias_covers_sky_but_not_nearer_foreground(self):
        for reverse in (False, True):
            with self.subTest(reverse_z=reverse):
                baseline, compensated, before, after = self._run(reverse)
                y, x = np.indices(baseline.shape)
                # Leave a two-pixel margin at the blocker edge; this avoids
                # making rasterization's top-left tie rule part of the test.
                blocker = ((x >= int(self.width * 0.35)) &
                           (x < int(self.width * 0.65)) &
                           (y >= int(self.height * 0.10)) &
                           (y < int(self.height * 0.90)))
                blocker_inner = ((x >= int(self.width * 0.41)) &
                                 (x < int(self.width * 0.59)) &
                                 (y >= int(self.height * 0.20)) &
                                 (y < int(self.height * 0.80)))
                sky = ~blocker
                # The unfixed sample-zero replay must expose the sloping-depth
                # regression, not merely pass because a flat plane hides it.
                self.assertGreater(int(np.count_nonzero((baseline == 0) & sky)),
                                   int(np.count_nonzero(sky)) // 16)
                self.assertTrue(
                    np.all(compensated[sky] == 255),
                    f"reverse={reverse} baseline_miss={np.count_nonzero((baseline == 0) & sky)} "
                    f"fixed_sky_min={compensated[sky].min()} fixed_sky_nonzero="
                    f"{np.count_nonzero(compensated[sky])} "
                    f"seed_range=({before.min():.6f},{before.max():.6f})",
                )
                self.assertTrue(
                    np.all(compensated[blocker_inner] == 0),
                    f"reverse={reverse} blocker_max={compensated[blocker_inner].max()} "
                    f"blocker_nonzero={np.count_nonzero(compensated[blocker_inner])}",
                )
                np.testing.assert_array_equal(before, after)


if __name__ == "__main__":
    unittest.main(verbosity=2)
