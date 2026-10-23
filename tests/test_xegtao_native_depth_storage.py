"""CPU/source contracts for XeGTAO working-depth storage selection."""

from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "src" / "gx" / "xegtao" / "policy.hpp"
NATIVE = ROOT / "src" / "gx" / "xegtao" / "native.hpp"
AMBIENT = ROOT / "src" / "gx" / "ambient_occlusion.cpp"
R32_HEADER = ROOT / "src" / "gx" / "xegtao" / "generated_shaders.hpp"
R16_HEADER = ROOT / "src" / "gx" / "xegtao" / "generated_r16_shaders.hpp"

sys.path.insert(0, str(ROOT / "tests"))
from test_xegtao_parameters import run_cpp


STAGES = (
    "CSPrefilterDepths16x16",
    "CSGenerateNormals",
    "CSGTAOLow",
    "CSGTAOMedium",
    "CSGTAOHigh",
    "CSGTAOUltra",
    "CSDenoisePass",
    "CSDenoiseLastPass",
)
MAIN_STAGES = ("CSGTAOLow", "CSGTAOMedium", "CSGTAOHigh", "CSGTAOUltra")
_RAW_STAGE_RE = re.compile(
    r'inline constexpr char (?P<name>[A-Za-z_]\w*)\[\] = R"MP6_XEGTAO\('
    r'(?P<body>.*?)\)MP6_XEGTAO";',
    re.DOTALL,
)
_STORAGE_RE = re.compile(
    r"@binding\(\s*(?P<binding>\d+)\s*\)\s*"
    r"@group\(\s*(?P<group>\d+)\s*\)\s*var\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*:\s*"
    r"texture_storage_2d\s*<\s*(?P<format>[A-Za-z0-9_]+)\s*,\s*"
    r"(?P<access>[A-Za-z0-9_-]+)\s*>\s*;",
    re.MULTILINE,
)


def _stages(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    return {match.group("name"): match.group("body") for match in _RAW_STAGE_RE.finditer(text)}


def _storage_declarations(source: str):
    return [
        (
            int(match.group("binding")),
            int(match.group("group")),
            match.group("name"),
            match.group("format"),
            match.group("access"),
        )
        for match in _STORAGE_RE.finditer(source)
    ]


def _biases(source: str) -> list[float]:
    return [
        float(value.rstrip("f"))
        for value in re.findall(r"(?<![A-Za-z0-9_])0\.999\d+(?:f)?", source)
    ]


class XeGTAONativeDepthStorage(unittest.TestCase):
    def test_policy_parser_defaults_strict_fallback_and_enabled_storage(self):
        run_cpp(
            r'''
#include <cassert>
#include <cstring>
#include "gx/xegtao/policy.hpp"

int main() {
    using namespace mp6::xegtao;
    const auto baseline = parse_policy(nullptr, nullptr, nullptr, nullptr);
    assert(baseline.policy.quality == Quality::High);
    assert(baseline.policy.denoisePasses == 2);
    assert(baseline.policy.resolution == Resolution::Full);
    assert(baseline.policy.depthStorage == DepthStorage::Float32);
    assert(!baseline.invalidQuality && !baseline.invalidDenoise &&
           !baseline.invalidResolution && !baseline.invalidDepthStorage);

    const auto selected = parse_policy("low", "3", "half", "r16float");
    assert(valid_policy(selected.policy));
    assert(selected.policy.quality == Quality::Low);
    assert(selected.policy.denoisePasses == 3);
    assert(selected.policy.resolution == Resolution::Half);
    assert(selected.policy.depthStorage == DepthStorage::Float16);

    const auto badDepth = parse_policy("low", "3", "half", "R16FLOAT");
    assert(badDepth.invalidDepthStorage);
    assert(!badDepth.invalidQuality && !badDepth.invalidDenoise && !badDepth.invalidResolution);
    assert(badDepth.policy.quality == Quality::Low);
    assert(badDepth.policy.denoisePasses == 3);
    assert(badDepth.policy.resolution == Resolution::Half);
    assert(badDepth.policy.depthStorage == DepthStorage::Float32);

    const auto badAll = parse_policy("bad", "bad", "bad", "bad");
    assert(badAll.invalidQuality && badAll.invalidDenoise &&
           badAll.invalidResolution && badAll.invalidDepthStorage);
    assert(badAll.policy.quality == Quality::High);
    assert(badAll.policy.denoisePasses == 2);
    assert(badAll.policy.resolution == Resolution::Full);
    assert(badAll.policy.depthStorage == DepthStorage::Float32);

    assert(supported_depth_storage(DepthStorage::Float32, false) == DepthStorage::Float32);
    assert(supported_depth_storage(DepthStorage::Float32, true) == DepthStorage::Float32);
    assert(supported_depth_storage(DepthStorage::Float16, false) == DepthStorage::Float32);
    assert(supported_depth_storage(DepthStorage::Float16, true) == DepthStorage::Float16);
    assert(std::strcmp(depth_storage_name(DepthStorage::Float32), "r32float") == 0);
    assert(std::strcmp(depth_storage_name(DepthStorage::Float16), "r16float") == 0);
}
'''
        )

    def test_all_quality_resolution_denoise_storage_combinations_are_valid(self):
        run_cpp(
            r'''
#include <cassert>
#include "gx/xegtao/policy.hpp"

int main() {
    using namespace mp6::xegtao;
    const Quality qualities[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
    const Resolution resolutions[] = {Resolution::Full, Resolution::Half};
    const DepthStorage storage[] = {DepthStorage::Float32, DepthStorage::Float16};
    for (Quality quality : qualities)
        for (Resolution resolution : resolutions)
            for (DepthStorage depthStorage : storage)
                for (unsigned passes = 1; passes <= 3; ++passes)
                    assert(valid_policy(Policy{quality, passes, resolution, depthStorage}));

    assert(!valid_policy(Policy{static_cast<Quality>(4), 2, Resolution::Full, DepthStorage::Float32}));
    assert(!valid_policy(Policy{Quality::High, 0, Resolution::Full, DepthStorage::Float32}));
    assert(!valid_policy(Policy{Quality::High, 4, Resolution::Full, DepthStorage::Float32}));
    assert(!valid_policy(Policy{Quality::High, 2, static_cast<Resolution>(2), DepthStorage::Float32}));
    assert(!valid_policy(Policy{Quality::High, 2, Resolution::Full, static_cast<DepthStorage>(2)}));
}
'''
        )

    def test_gtao_constants_abi_and_math_do_not_depend_on_storage_selector(self):
        run_cpp(
            r'''
#include <cassert>
#include <cstddef>
#include <cstring>
#include "gx/xegtao/parameters.hpp"

int main() {
    using namespace mp6::xegtao;
    static_assert(sizeof(XeGTAO::GTAOConstants) == 96);
    static_assert(offsetof(XeGTAO::GTAOConstants, DepthUnpackConsts) == 16);
    static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul) == 32);
    static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul_x_PixelSize) == 48);
    static_assert(offsetof(XeGTAO::GTAOConstants, EffectRadius) == 56);
    static_assert(offsetof(XeGTAO::GTAOConstants, NoiseIndex) == 92);

    const float viewport[] = {0.0f, 0.0f, 1.0f, 1.0f};
    XeGTAO::GTAOConstants baseline{};
    assert(fill_parameters(641, 479, 0.75f, 1.5f, 0.25f, 500.0f,
                           viewport, baseline));
    const Quality qualities[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
    const Resolution resolutions[] = {Resolution::Full, Resolution::Half};
    const DepthStorage storage[] = {DepthStorage::Float32, DepthStorage::Float16};
    for (Quality quality : qualities)
        for (Resolution resolution : resolutions)
            for (DepthStorage depthStorage : storage)
                for (unsigned passes = 1; passes <= 3; ++passes) {
                    XeGTAO::GTAOConstants actual{};
                    const Policy policy{quality, passes, resolution, depthStorage};
                    assert(fill_parameters(641, 479, 0.75f, 1.5f, 0.25f, 500.0f,
                                           viewport, actual, 32.0f, policy));
                    assert(std::memcmp(&actual, &baseline, sizeof(actual)) == 0);
                }
}
'''
        )

    def test_native_resource_cache_keys_actual_format_and_keeps_fp32_roles(self):
        source = NATIVE.read_text(encoding="utf-8")
        compact = re.sub(r"\s+", " ", source)
        scratch = source.split("    std::shared_ptr<Scratch> create_scratch(", 1)[1].split(
            "    std::shared_ptr<Scratch> acquire_scratch(", 1
        )[0]
        init = source.split("    bool initialize(", 1)[1].split(
            "    void reset()", 1
        )[0]

        self.assertIn("DepthStorage depthStorage = DepthStorage::Float32;", source)
        self.assertRegex(
            re.sub(r"\s+", " ", source),
            r"scratch\.width == width && scratch\.height == height && "
            r"scratch\.depthStorage == depthStorage",
        )
        self.assertIn("scratch->depthStorage = policy.depthStorage;", scratch)
        self.assertIn("depthStorage = policy.depthStorage", source)
        self.assertIn(
            "policy.depthStorage = supported_depth_storage(selectedPolicy.depthStorage, r16StorageSupported);",
            compact,
        )
        self.assertIn(
            "workingDepthFormat = r16 ? wgpu::TextureFormat::R16Float : wgpu::TextureFormat::R32Float;",
            compact,
        )
        self.assertEqual(len(re.findall(r"storage\([1-5], workingDepthFormat\)", init)), 5)
        self.assertRegex(
            compact,
            r"scratch->depthMips = image\(device, \"XeGTAO depth MIPs\", width, height, "
            r"workingDepthFormat, computeUsage, 5\)",
        )

        # R16 is limited to the five depth-MIP storage outputs. Raw canonical
        # depth, normal output, AO output, and edge arithmetic retain their
        # existing FP32/uint formats.
        self.assertIn("wgpu::TextureFormat::R32Float, computeUsage | wgpu::TextureUsage::RenderAttachment", compact)
        self.assertIn("wgpu::TextureFormat::R32Uint, computeUsage", compact)
        self.assertIn("canonical = raster_pipeline(device, adapter, canonicalLayout, \"fs_canonical_depth\", TF::R32Float)", compact)
        self.assertRegex(init, r"normalLayout\s*=\s*layout\([\s\S]*?storage\(1, TF::R32Uint\)")
        self.assertRegex(init, r"mainLayout\s*=\s*layout\([\s\S]*?storage\(4, TF::R32Float\)")
        self.assertIn("stages[1] = compute_pipeline(device, shaders::CSGenerateNormals", compact)
        self.assertIn("stages[3] = compute_pipeline(device, shaders::CSDenoisePass", compact)
        self.assertIn("stages[4] = compute_pipeline(device, shaders::CSDenoiseLastPass", compact)

    def test_native_selects_matching_r16_or_r32_stage_sources(self):
        if not R16_HEADER.is_file():
            self.skipTest("generated R16 shader header is not present yet")
        source = NATIVE.read_text(encoding="utf-8")
        compact = re.sub(r"\s+", " ", source)
        init = source.split("    bool initialize(", 1)[1].split(
            "    void reset()", 1
        )[0]
        self.assertIn('#include "generated_r16_shaders.hpp"', source)
        self.assertIn(
            "stages[0] = compute_pipeline(device, r16 ? r16_shaders::CSPrefilterDepths16x16 : shaders::CSPrefilterDepths16x16,",
            compact,
        )
        for stage in MAIN_STAGES:
            self.assertIn(f"mainCode = shaders::{stage};", compact)
            self.assertIn(f"mainCode = r16_shaders::{stage};", compact)
        self.assertIn("if (r16)", init)

    def test_r16_and_r32_generated_shader_storage_and_bias_contracts(self):
        if not R16_HEADER.is_file() or not R32_HEADER.is_file():
            self.skipTest("generated R16/R32 shader header is not present yet")
        r16 = _stages(R16_HEADER)
        r32 = _stages(R32_HEADER)
        self.assertEqual(set(STAGES), set(r16))
        self.assertEqual(set(STAGES), set(r32))

        for label, stages, storage_format, bias in (
            ("R16", r16, "r16float", 0.99920),
            ("R32", r32, "r32float", 0.99999),
        ):
            with self.subTest(storage=label):
                prefilter = _storage_declarations(stages["CSPrefilterDepths16x16"])
                depth_outputs = [item for item in prefilter if "g_outWorkingDepthMIP" in item[2]]
                self.assertEqual(
                    [(binding, group, fmt, access) for binding, group, _name, fmt, access in depth_outputs],
                    [(index, 1, storage_format, "write") for index in range(1, 6)],
                )
                self.assertEqual(len(depth_outputs), 5)
                self.assertNotIn("f16", stages["CSPrefilterDepths16x16"])

                normal_outputs = _storage_declarations(stages["CSGenerateNormals"])
                self.assertIn((1, 3, "g_outNormalmap_0", "r32uint", "write"), normal_outputs)
                self.assertNotIn("f16", stages["CSGenerateNormals"])

                for stage in MAIN_STAGES:
                    body = stages[stage]
                    self.assertNotIn("f16", body)
                    self.assertIn("texture_2d<f32>", body)
                    outputs = _storage_declarations(body)
                    self.assertIn("r32uint", [item[3] for item in outputs])
                    self.assertIn("r32float", [item[3] for item in outputs])
                    self.assertTrue(
                        any(abs(value - bias) <= 2.0e-5 for value in _biases(body)),
                        f"{stage} has no {label} depth bias",
                    )

                for stage in ("CSDenoisePass", "CSDenoiseLastPass"):
                    self.assertNotIn("f16", stages[stage])
                    self.assertIn("r32uint", [item[3] for item in _storage_declarations(stages[stage])])


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
