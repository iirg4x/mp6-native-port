"""Source-level proof for the post-0074 present FXAA fast path."""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "compat/aurora/base/0075-present-fxaa-work.patch"
TARGET = "lib/webgpu/gpu.cpp"
POST_0074_MARKER = "Surface capabilities: supported-present-modes=[{}]"


def replay_pre_0075() -> str:
    """Reconstruct the pinned generated source through the 0074 queue."""
    pin = step_aurora.read_aurora_pin()
    source = subprocess.run(
        ["git", "-C", build.AURORA_ROOT, "show", f"{pin}:{TARGET}"],
        capture_output=True, check=True, timeout=20,
    ).stdout.decode()
    for path in sorted((ROOT / "compat/aurora/base").glob("*.patch")):
        if path.name == PATCH.name:
            break
        for section in step_aurora.split_patch_sections(path.read_text()):
            if (section["new"] or section["old"]) == TARGET:
                source = apply_patches.apply_unified_diff(
                    source, section["body"], label=str(path))
    return source


def resample(text: str) -> str:
    match = re.search(r'resampleShaderSource = R"\((.*?)\)"sv;', text, re.S)
    if not match:
        raise AssertionError("resample shader literal is absent")
    return match.group(1)


def apply_private_patch(source: str) -> str:
    old = """@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    if (uniforms.post_aa_mode == 1u) {
        color = vec4<f32>(apply_fxaa(in.uv), color.a);
    }
    return vec4(color.rgb, 1.0);
}"""
    new = """@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    if (uniforms.post_aa_mode == 1u) {
        return vec4<f32>(apply_fxaa(in.uv), 1.0);
    }
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}"""
    if source.count(old) != 1:
        raise AssertionError("expected exactly one current fs_main")
    return source.replace(old, new, 1)


def add_zero_direction_candidate(shader: str) -> str:
    anchor = """    dir = clamp(dir * rcp_dir_min, vec2<f32>(-8.0, -8.0), vec2<f32>(8.0, 8.0)) * rcp;

    let rgb_a = 0.5 * ("""
    replacement = """    dir = clamp(dir * rcp_dir_min, vec2<f32>(-8.0, -8.0), vec2<f32>(8.0, 8.0)) * rcp;
    if (all(dir == vec2<f32>(0.0, 0.0))) {
        return rgb_m;
    }

    let rgb_a = 0.5 * ("""
    if shader.count(anchor) != 1:
        raise AssertionError("FXAA normalized-dir anchor is not unique")
    return shader.replace(anchor, replacement, 1)


@unittest.skip("before/after proof of the former Aurora patch 0075; the change is in "
               "third_party/aurora and the pre-0075 source is history")
class PresentFxaaWorkSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = replay_pre_0075()
        cls.patch = PATCH.read_text(encoding="utf-8")
        if POST_0074_MARKER not in cls.text:
            raise unittest.SkipTest("current generated source is not post-0074")

    def test_patch_applies_strictly_to_post_0074_generated_source(self):
        self.assertIn("diff --git a/lib/webgpu/gpu.cpp b/lib/webgpu/gpu.cpp", self.patch)
        self.assertEqual(self.patch.count("@@ -261,12 +261,12 @@"), 1)
        self.assertEqual(self.patch.count("apply_fxaa(in.uv), 1.0"), 1)
        added = "\n".join(line[1:] for line in self.patch.splitlines()
                             if line.startswith("+") and not line.startswith("+++"))
        self.assertNotIn("apply_fxaa(in.uv), color.a", added)

        with tempfile.TemporaryDirectory(prefix="present-fxaa-work-", dir=ROOT / "build") as folder:
            root = Path(folder)
            (root / "lib/webgpu").mkdir(parents=True)
            (root / "lib/webgpu/gpu.cpp").write_text(
                self.text, encoding="utf-8")
            check = subprocess.run(
                ["git", "apply", "--check", "--unsafe-paths",
                 "--directory=" + str(root), str(PATCH)],
                cwd=ROOT, capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
            apply = subprocess.run(
                ["git", "apply", "--unsafe-paths",
                 "--directory=" + str(root), str(PATCH)],
                cwd=ROOT, capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(apply.returncode, 0, apply.stdout + apply.stderr)
            patched = (root / "lib/webgpu/gpu.cpp").read_text(encoding="utf-8")

        old_shader = resample(self.text)
        new_shader = resample(patched)
        self.assertEqual(new_shader, apply_private_patch(old_shader))
        self.assertIn("if (uniforms.post_aa_mode == 1u) {\n        return vec4<f32>(apply_fxaa(in.uv), 1.0);", new_shader)
        self.assertEqual(new_shader.count("return vec4<f32>(apply_fxaa(in.uv), 1.0);"), 1)
        self.assertNotIn("color = vec4<f32>(apply_fxaa(in.uv), color.a);", new_shader)
        self.assertIn("return vec4(color.rgb, 1.0);", new_shader)

    def test_only_outer_fxaa_branch_changes_and_algorithm_is_identical(self):
        old_shader = resample(self.text)
        new_shader = apply_private_patch(old_shader)
        old_fxaa = old_shader[old_shader.index("fn apply_fxaa"):old_shader.index("// END FXAA REGION")]
        new_fxaa = new_shader[new_shader.index("fn apply_fxaa"):new_shader.index("// END FXAA REGION")]
        self.assertEqual(old_fxaa, new_fxaa)
        self.assertEqual(new_shader.count("sample_area(in.position)"), 1)
        self.assertEqual(new_shader.count("textureSample(t, s, in.uv)"), 1)
        self.assertEqual(new_shader.count("uniforms.post_aa_mode == 1u"), 1)

    def test_zero_direction_candidate_is_separate_and_algorithm_preserving(self):
        old_shader = resample(self.text)
        outer = apply_private_patch(old_shader)
        zero = add_zero_direction_candidate(outer)
        self.assertEqual(zero.count("if (all(dir == vec2<f32>(0.0, 0.0)))"), 1)
        self.assertIn("return rgb_m;", zero)
        insertion = (
            "    if (all(dir == vec2<f32>(0.0, 0.0))) {\n"
            "        return rgb_m;\n"
            "    }\n"
        )
        self.assertEqual(zero.replace(insertion, "", 1), outer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
