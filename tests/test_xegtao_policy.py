"""CPU and native-wiring contracts for the opt-in XeGTAO preset selector."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_xegtao_parameters import run_cpp


class XeGTAOPolicy(unittest.TestCase):
    def test_pc_default_and_explicit_legacy_override(self):
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#define _WIN32 1
#include "gx/xegtao/policy.hpp"
int main() {
    using namespace mp6::xegtao;
    static_assert(default_enabled);
    assert(requested(nullptr));
    assert(requested("1"));
    assert(!requested("0"));
    assert(requested("invalid"));
    assert(requested(""));
}
''')

    def test_android_remains_opt_in(self):
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#define __ANDROID__ 1
#include "gx/xegtao/policy.hpp"
int main() {
    using namespace mp6::xegtao;
    static_assert(!default_enabled);
    assert(!requested(nullptr));
    assert(requested("1"));
    assert(!requested("0"));
    assert(!requested("invalid"));
}
''')

    def test_default_all_presets_and_strict_invalid_fallback(self):
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include "gx/xegtao/policy.hpp"

int main() {
    using namespace mp6::xegtao;
    const auto baseline = parse_policy(nullptr, nullptr);
    assert(baseline.policy.quality == Quality::High);
    assert(baseline.policy.denoisePasses == 2);
    assert(baseline.policy.resolution == Resolution::Full);
    assert(!baseline.invalidQuality && !baseline.invalidDenoise && !baseline.invalidResolution);
    const char *names[] = {"low", "medium", "high", "ultra"};
    const char *labels[] = {"Low", "Medium", "High", "Ultra"};
    const char *entries[] = {"CSGTAOLow", "CSGTAOMedium", "CSGTAOHigh", "CSGTAOUltra"};
    const Quality qualities[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
    for (unsigned q = 0; q < 4; ++q) {
        for (unsigned passes = 1; passes <= 3; ++passes) {
            const auto parsed = parse_policy(names[q], passes == 1 ? "1" : (passes == 2 ? "2" : "3"));
            assert(valid_policy(parsed.policy));
            assert(parsed.policy.quality == qualities[q]);
            assert(parsed.policy.denoisePasses == passes);
            assert(parsed.policy.resolution == Resolution::Full);
            assert(!parsed.invalidQuality && !parsed.invalidDenoise);
            assert(std::strcmp(quality_name(parsed.policy.quality), labels[q]) == 0);
            assert(std::strcmp(main_entry_point(parsed.policy.quality), entries[q]) == 0);
            // Independently simulate main -> ping-pong denoise, including the
            // mandatory final pass. The composite must consume its destination.
            unsigned written = 0;
            for (unsigned i = 0; i < passes; ++i) written = 1 - written;
            assert(final_ao_index(parsed.policy) == written);
        }
    }
    const char *badQuality[] = {"", "Low", "HIGH", " low", "high ", "Ultra", "1"};
    for (const char *value : badQuality) {
        const auto parsed = parse_policy(value, "1");
        assert(parsed.invalidQuality && !parsed.invalidDenoise);
        assert(parsed.policy.quality == Quality::High);
        assert(parsed.policy.denoisePasses == 1);
    }
    const char *badDenoise[] = {"", "0", "4", "01", "1 ", " 2", "1x", "-1"};
    for (const char *value : badDenoise) {
        const auto parsed = parse_policy("low", value);
        assert(!parsed.invalidQuality && parsed.invalidDenoise);
        assert(parsed.policy.quality == Quality::Low);
        assert(parsed.policy.denoisePasses == 2);
    }
    const auto badBoth = parse_policy("garbage", "garbage");
    assert(badBoth.invalidQuality && badBoth.invalidDenoise);
    assert(badBoth.policy.quality == baseline.policy.quality);
    assert(badBoth.policy.denoisePasses == baseline.policy.denoisePasses);
    assert(!valid_policy({Quality::High, 0}));
    assert(!valid_policy({Quality::High, 4}));
    assert(!valid_policy({static_cast<Quality>(4), 2}));
    assert(!valid_policy({Quality::High, 2, static_cast<Resolution>(2)}));
}
''')

    def test_resolution_selector_and_overflow_safe_dimensions(self):
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "gx/xegtao/policy.hpp"

int main() {
    using namespace mp6::xegtao;
    for (const char *quality : {"low", "medium", "high", "ultra"}) {
        for (const char *denoise : {"1", "2", "3"}) {
            const auto full = parse_policy(quality, denoise, "full");
            const auto half = parse_policy(quality, denoise, "half");
            assert(valid_policy(full.policy) && valid_policy(half.policy));
            assert(full.policy.resolution == Resolution::Full);
            assert(half.policy.resolution == Resolution::Half);
            assert(full.policy.quality == half.policy.quality);
            assert(full.policy.denoisePasses == half.policy.denoisePasses);
            assert(!full.invalidResolution && !half.invalidResolution);
        }
    }
    const char *invalid[] = {"", "Half", "FULL", " half", "half ", "0.5", "2", "quarter"};
    for (const char *resolution : invalid) {
        const auto parsed = parse_policy("low", "1", resolution);
        assert(parsed.invalidResolution && !parsed.invalidQuality && !parsed.invalidDenoise);
        assert(parsed.policy.resolution == Resolution::Full);
        assert(parsed.policy.quality == Quality::Low && parsed.policy.denoisePasses == 1);
    }
    const auto independent = parse_policy("invalid", "invalid", "half");
    assert(independent.invalidQuality && independent.invalidDenoise && !independent.invalidResolution);
    assert(independent.policy.quality == Quality::High && independent.policy.denoisePasses == 2);
    assert(independent.policy.resolution == Resolution::Half);
    constexpr auto full = working_size(2341, 1081, Resolution::Full);
    static_assert(full.width == 2341 && full.height == 1081);
    constexpr auto half = working_size(2341, 1081, Resolution::Half);
    static_assert(half.width == 1171 && half.height == 541);
    constexpr auto even = working_size(2340, 1080, Resolution::Half);
    static_assert(even.width == 1170 && even.height == 540);
    constexpr auto small = working_size(0, 1, Resolution::Half);
    static_assert(small.width == 1 && small.height == 1);
    constexpr auto zeroFull = working_size(0, 0, Resolution::Full);
    static_assert(zeroFull.width == 1 && zeroFull.height == 1);
    constexpr auto largest = working_size(UINT32_MAX, UINT32_MAX - 1, Resolution::Half);
    static_assert(largest.width == UINT32_C(2147483648) && largest.height == UINT32_C(2147483647));
    constexpr auto largestFull = working_size(UINT32_MAX, UINT32_MAX, Resolution::Full);
    static_assert(largestFull.width == UINT32_MAX && largestFull.height == UINT32_MAX);
}
''')

    def test_worker_captures_once_and_composite_uses_final_destination(self):
        # These source contracts cover the actual native path; the isolated GPU
        # harness uses its own resource scheduler and cannot prove this wiring.
        source = (ROOT / "src/gx/ambient_occlusion.cpp").read_text()
        init = source.split("void init_worker(", 1)[1].split("void init_depth_path(", 1)[0]
        for name in ("MP6_AO_XEGTAO_QUALITY", "MP6_AO_XEGTAO_DENOISE", "MP6_AO_XEGTAO_RESOLUTION"):
            expression = f'std::getenv("{name}")'
            self.assertEqual(source.count(expression), 1)
            self.assertIn(expression, init.split("if (g_xegtaoRequested)", 1)[1])
        self.assertIn("if (g_shader) return;", init)
        self.assertIn("if (parsed.invalidQuality)", init)
        self.assertIn("if (parsed.invalidDenoise)", init)
        self.assertIn("if (parsed.invalidResolution)", init)
        self.assertIn("mp6::xegtao::working_size(ctx.targetWidth, ctx.targetHeight, g_xegtaoPolicy.resolution)", source)
        self.assertIn("std::max(upstreamSize.width, upstreamSize.height) >= 16", source)
        self.assertIn("mp6::xegtao::fill_parameters(upstreamSize.width, upstreamSize.height,", source)
        self.assertIn("upstreamConstants, 64.0f, g_xegtaoPolicy)", source)
        self.assertIn("g_xegtao.initialize(ctx.device, g_xegtaoPolicy)", source)
        self.assertIn("job.upstream.scratch->aoViews[job.upstream.finalAoIndex]", source)
        self.assertIn(".binding = 3, .textureView = job.upstream.scratch->mipViews[0]", source)
        self.assertIn(".entryCount = half ? 5u : 4u", source)
        self.assertIn("make_layout(device, half ? 5 : 4)", source)
        self.assertIn("half ? std::string(mp6::xegtao::HalfCompositeShader)", source)
        self.assertIn("mp6::xegtao::composite_shader(canonicalDepth)", source)
        self.assertIn("assert(!canonicalDepth || !half);", source)
        shutdown = source.split('extern "C" void mp6_ao_shutdown', 1)[1]
        self.assertIn("g_xegtaoPolicy = {};", shutdown)

    def test_native_compiles_one_main_and_always_runs_final_denoise(self):
        source = (ROOT / "src/gx/xegtao/native.hpp").read_text()
        self.assertEqual(source.count("stages[2] = compute_pipeline("), 1)
        self.assertIn("mainCode = shaders::CSGTAOLow;", source)
        self.assertIn("mainCode = shaders::CSGTAOMedium;", source)
        self.assertIn("mainCode = shaders::CSGTAOUltra;", source)
        self.assertIn("const char *mainCode = shaders::CSGTAOHigh;", source)
        self.assertIn("main_entry_point(policy.quality), 2, mainLayout", source)
        encode = source.split("    void encode(", 1)[1]
        self.assertIn("r.finalAoIndex = final_ao_index(policy);", encode)
        self.assertIn("for (uint32_t i = 0; i + 1 < policy.denoisePasses; ++i)\n"
                      '            dispatch(encoder, 3, r, s, s.denoise[i % 2], 3, 16, 8, "XeGTAO denoise");', encode)
        self.assertIn('dispatch(encoder, 4, r, s, s.denoise[1 - r.finalAoIndex], 3, 16, 8, "XeGTAO final denoise");', encode)
        self.assertIn("std::array<wgpu::Texture, 2> ao;", source)
        self.assertIn("policy.resolution == Resolution::Half ? HalfAdapterShader : AdapterShader", source)
        self.assertIn("XeGTAO experimental: half-resolution bilateral uint output", source)
        self.assertIn("XeGTAO %s: full-resolution direct uint output", source)


if __name__ == "__main__":
    unittest.main()
