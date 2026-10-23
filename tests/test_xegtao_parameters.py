"""Standalone CPU checks for the future XeGTAO parameter adapter."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build


def run_cpp(source):
    with tempfile.TemporaryDirectory(prefix="xegtao-parameters-", dir=ROOT / "build") as folder:
        folder = Path(folder)
        cpp = folder / "test.cpp"
        exe = folder / "test.exe"
        cpp.write_text(source)
        result = subprocess.run(
            [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG",
             "-I", str(ROOT / "src"), str(cpp), "-o", str(exe)],
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode:
            raise AssertionError(result.stderr)
        result = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=10
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


class XeGTAOParameters(unittest.TestCase):
    def test_presets_keep_upstream_constants_and_positive_denoise_beta(self):
        run_cpp(r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
#include "gx/xegtao/parameters.hpp"

int main() {
    using namespace mp6::xegtao;
    const float viewport[] = {0.0f, 0.0f, 1.0f, 1.0f};
    XeGTAO::GTAOConstants baseline{};
    assert(fill_parameters(257, 145, 0.75f, 1.5f, 0.25f, 500.0f, viewport, baseline));
    for (auto quality : {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra}) {
        for (uint32_t passes : {1u, 2u, 3u}) {
            XeGTAO::GTAOConstants actual{};
            assert(fill_parameters(257, 145, 0.75f, 1.5f, 0.25f, 500.0f,
                                   viewport, actual, 32.0f, {quality, passes}));
            // Upstream uses beta 1.2 for every positive count. Quality selects
            // the main entry point; it does not alter this constants block.
            assert(actual.DenoiseBlurBeta == 1.2f);
            assert(actual.NoiseIndex == 0);
            assert(std::memcmp(&actual, &baseline, sizeof(actual)) == 0);
        }
    }
    for (Policy invalid : {Policy{Quality::High, 0}, Policy{Quality::High, 4},
                           Policy{static_cast<Quality>(4), 2}}) {
        XeGTAO::GTAOConstants actual = baseline;
        assert(!fill_parameters(257, 145, 0.75f, 1.5f, 0.25f, 500.0f,
                                viewport, actual, 32.0f, invalid));
        assert(std::memcmp(&actual, &baseline, sizeof(actual)) == 0);
    }
}
''')

    def test_full_viewport_matches_upstream_constants_and_abi(self):
        run_cpp(r'''
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include "gx/xegtao/parameters.hpp"

static bool close(float a, float b) {
    return std::abs(a - b) <= 1.0e-6f * (1.0f + std::max(std::abs(a), std::abs(b)));
}

int main() {
    static_assert(sizeof(XeGTAO::GTAOConstants) == 96);
    static_assert(offsetof(XeGTAO::GTAOConstants, DepthUnpackConsts) == 16);
    static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul) == 32);
    static_assert(offsetof(XeGTAO::GTAOConstants, NDCToViewMul_x_PixelSize) == 48);
    static_assert(offsetof(XeGTAO::GTAOConstants, EffectRadius) == 56);
    static_assert(offsetof(XeGTAO::GTAOConstants, NoiseIndex) == 92);

    constexpr int width = 1280;
    constexpr int height = 721;
    constexpr float tanY = 0.75f;
    constexpr float aspect = 16.0f / 9.0f;
    constexpr float nearPlane = 0.25f;
    constexpr float farPlane = 500.0f;
    const float viewport[] = {0.0f, 0.0f, 1.0f, 1.0f};

    XeGTAO::GTAOConstants actual{};
    assert(mp6::xegtao::fill_parameters(width, height, tanY, aspect, nearPlane,
                                         farPlane, viewport, actual));

    XeGTAO::GTAOSettings settings;
    settings.Radius = 32.0f;
    const float projection[] = {
        1.0f / (tanY * aspect), 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f / tanY, 0.0f, 0.0f,
        0.0f, 0.0f, farPlane / (farPlane - nearPlane), 1.0f,
        0.0f, 0.0f, -(nearPlane * farPlane) / (farPlane - nearPlane), 0.0f,
    };
    XeGTAO::GTAOConstants expected{};
    XeGTAO::GTAOUpdateConstants(expected, width, height, settings, projection,
                                 true, 0U);
    assert(std::memcmp(&actual, &expected, sizeof(actual)) == 0);
    assert(actual.NoiseIndex == 0);
}
''')

    def test_offset_viewport_and_odd_resolution(self):
        run_cpp(r'''
#include <cassert>
#include <algorithm>
#include <cmath>
#include "gx/xegtao/parameters.hpp"

static bool close(float a, float b) {
    return std::abs(a - b) <= 2.0e-6f * (1.0f + std::max(std::abs(a), std::abs(b)));
}

int main() {
    constexpr float tanY = 0.625f;
    constexpr float aspect = 1.7f;
    const float viewport[] = {0.125f, 0.2f, 0.5f, 0.625f};
    XeGTAO::GTAOConstants c{};
    assert(mp6::xegtao::fill_parameters(641, 479, tanY, aspect, 0.5f, 900.0f,
                                         viewport, c, 11.0f));
    const float tanX = c.CameraTanHalfFOV.x;
    const float expectedMulX = 2.0f * tanX / viewport[2];
    const float expectedMulY = -2.0f * tanY / viewport[3];
    assert(c.ViewportSize.x == 641 && c.ViewportSize.y == 479);
    assert(close(c.ViewportPixelSize.x, 1.0f / 641.0f));
    assert(close(c.ViewportPixelSize.y, 1.0f / 479.0f));
    assert(close(c.NDCToViewMul.x, expectedMulX));
    assert(close(c.NDCToViewMul.y, expectedMulY));
    assert(close(c.NDCToViewAdd.x, -tanX - expectedMulX * viewport[0]));
    assert(close(c.NDCToViewAdd.y, tanY - expectedMulY * viewport[1]));
    assert(close(c.NDCToViewMul_x_PixelSize.x, expectedMulX / 641.0f));
    assert(close(c.NDCToViewMul_x_PixelSize.y, expectedMulY / 479.0f));
    assert(c.EffectRadius == 11.0f);
    assert(c.NoiseIndex == 0);
}
''')

    def test_depth_round_trip_and_default_radius(self):
        run_cpp(r'''
#include <cassert>
#include <cmath>
#include "gx/xegtao/parameters.hpp"

static bool close(float a, float b) {
    return std::abs(a - b) <= 2.0e-3f * (1.0f + std::abs(b));
}

int main() {
    const float viewport[] = {0.0f, 0.0f, 1.0f, 1.0f};
    XeGTAO::GTAOConstants c{};
    assert(mp6::xegtao::fill_parameters(17, 19, 0.8f, 1.25f, 0.1f, 100.0f,
                                         viewport, c));
    const float nearDepth = c.DepthUnpackConsts.x /
        (c.DepthUnpackConsts.y - 0.0f);
    const float farDepth = c.DepthUnpackConsts.x /
        (c.DepthUnpackConsts.y - 1.0f);
    assert(close(nearDepth, 0.1f));
    assert(close(farDepth, 100.0f));
    assert(c.EffectRadius == 32.0f);
    assert(c.NoiseIndex == 0);
}
''')

    def test_invalid_inputs_are_rejected_without_output_write(self):
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include "gx/xegtao/parameters.hpp"

static bool accepted(std::int64_t width, std::int64_t height, float tanY,
                     float aspect, float nearPlane, float farPlane,
                     const float *viewport, float radius = 32.0f) {
    XeGTAO::GTAOConstants c{};
    c.NoiseIndex = 77;
    const XeGTAO::GTAOConstants before = c;
    assert(!mp6::xegtao::fill_parameters(width, height, tanY, aspect, nearPlane,
                                          farPlane, viewport, c, radius));
    return std::memcmp(&c, &before, sizeof(c)) == 0;
}

int main() {
    const float valid[] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float zeroWidth[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float zeroHeight[] = {0.0f, 0.0f, 1.0f, 0.0f};
    const float nanViewport[] = {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f};
    assert(accepted(0, 1, 0.5f, 1.0f, 0.1f, 1.0f, valid));
    assert(accepted(-1, 1, 0.5f, 1.0f, 0.1f, 1.0f, valid));
    assert(accepted(static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1,
                    1, 0.5f, 1.0f, 0.1f, 1.0f, valid));
    assert(accepted(1, 1, 0.0f, 1.0f, 0.1f, 1.0f, valid));
    assert(accepted(1, 1, 0.5f, 0.0f, 0.1f, 1.0f, valid));
    assert(accepted(1, 1, std::numeric_limits<float>::infinity(), 1.0f,
                    0.1f, 1.0f, valid));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.0f, 1.0f, valid));
    assert(accepted(1, 1, 0.5f, 1.0f, 2.0f, 1.0f, valid));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, zeroWidth));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, zeroHeight));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, nanViewport));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, nullptr));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, valid,
                    std::numeric_limits<float>::quiet_NaN()));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, valid, 0.0f));
    assert(accepted(1, 1, 0.5f, 1.0f, 0.1f, 1.0f, valid, -1.0f));
}
''')


if __name__ == "__main__":
    unittest.main()
