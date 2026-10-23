"""CPU contract and executable control-flow checks for lazy XeGTAO setup."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]
AMBIENT = ROOT / "src/gx/ambient_occlusion.cpp"
NATIVE = ROOT / "src/gx/xegtao/native.hpp"


def preparation_block() -> str:
    source = AMBIENT.read_text(encoding="utf-8")
    begin_marker = "    // AO_UPSTREAM_PREPARATION_BEGIN\n"
    end_marker = "    // AO_UPSTREAM_PREPARATION_END"
    begin = source.index(begin_marker) + len(begin_marker)
    end = source.index(end_marker, begin)
    return source[begin:end]


CPP_FACADE = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>

namespace XeGTAO {
struct GTAOConstants {
    struct { float x = 0.0f, y = 0.0f; } DepthUnpackConsts;
    inline static unsigned constructions = 0;
    GTAOConstants() { ++constructions; }
};
}

namespace mp6::xegtao {
struct Policy { int resolution = 0; };
struct WorkingSize { uint32_t width = 0, height = 0; };
inline unsigned working_calls = 0;
inline unsigned fill_calls = 0;
inline bool fill_result = true;
inline WorkingSize next_size{};

WorkingSize working_size(uint32_t, uint32_t, int) {
    ++working_calls;
    return next_size;
}

bool fill_parameters(uint32_t width, uint32_t height,
                     float, float, float, float, const float *,
                     XeGTAO::GTAOConstants &constants, float, const Policy &) {
    ++fill_calls;
    if (!fill_result) return false;
    constants.DepthUnpackConsts.x = float(width);
    constants.DepthUnpackConsts.y = float(height);
    return true;
}

struct Pipeline {
    bool checked = false;
    bool supported = false;
    bool initialize_result = true;
    unsigned initialize_calls = 0;

    bool unavailable() const noexcept { return checked && !supported; }

    bool initialize(int, const Policy &) {
        ++initialize_calls;
        if (checked) return supported;
        checked = true;
        supported = initialize_result;
        return supported;
    }

    void reset() { checked = supported = false; }
};
}

struct Params {
    float projection[4] = {1, 2, 3, 4};
    float viewport[4] = {5, 6, 7, 8};
};
struct Job { Params params; };
struct Context { int device = 1; uint32_t targetWidth = 1920, targetHeight = 1080; };

bool g_xegtaoRequested = false;
mp6::xegtao::Policy g_xegtaoPolicy;
mp6::xegtao::Pipeline g_xegtao;

bool prepare(bool requested, uint32_t target_width, uint32_t target_height,
             uint32_t expected_width, uint32_t expected_height) {
    g_xegtaoRequested = requested;
    mp6::xegtao::next_size = {expected_width, expected_height};
    Context ctx{1, target_width, target_height};
    Job job;
""" + preparation_block() + r"""
    if (upstream) {
        assert(upstreamConstants.has_value());
        assert(upstreamConstants->DepthUnpackConsts.x == float(expected_width));
        assert(upstreamConstants->DepthUnpackConsts.y == float(expected_height));
    }
    return upstream;
}

void clear_counters() {
    mp6::xegtao::working_calls = 0;
    mp6::xegtao::fill_calls = 0;
    mp6::xegtao::fill_result = true;
    XeGTAO::GTAOConstants::constructions = 0;
}

int main() {
    // Legacy frames do no upstream sizing, scratch construction, filling, or init.
    clear_counters();
    g_xegtao = {};
    assert(!prepare(false, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 0);
    assert(mp6::xegtao::fill_calls == 0);
    assert(g_xegtao.initialize_calls == 0);
    assert(XeGTAO::GTAOConstants::constructions == 0);

    // A native init failure remains the legacy fallback and suppresses all
    // subsequent upstream preparation until the pipeline is explicitly reset.
    clear_counters();
    g_xegtao = {};
    g_xegtao.initialize_result = false;
    assert(!prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 1 && mp6::xegtao::fill_calls == 1);
    assert(g_xegtao.initialize_calls == 1 && g_xegtao.unavailable());
    const unsigned constructions_after_failure = XeGTAO::GTAOConstants::constructions;
    assert(!prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 1 && mp6::xegtao::fill_calls == 1);
    assert(g_xegtao.initialize_calls == 1);
    assert(XeGTAO::GTAOConstants::constructions == constructions_after_failure);
    g_xegtao.reset();
    g_xegtao.initialize_result = true;
    assert(prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 2 && mp6::xegtao::fill_calls == 2);
    assert(g_xegtao.initialize_calls == 2);

    // Tiny views skip fill/init but remain retryable when the view grows.
    clear_counters();
    g_xegtao = {};
    assert(!prepare(true, 8, 8, 8, 8));
    assert(mp6::xegtao::working_calls == 1 && mp6::xegtao::fill_calls == 0);
    assert(g_xegtao.initialize_calls == 0);
    assert(XeGTAO::GTAOConstants::constructions == 0);
    assert(prepare(true, 32, 32, 32, 32));
    assert(mp6::xegtao::working_calls == 2 && mp6::xegtao::fill_calls == 1);
    assert(g_xegtao.initialize_calls == 1);

    // Invalid camera parameters do not poison initialization; the next valid
    // frame still fills and initializes.
    clear_counters();
    g_xegtao = {};
    mp6::xegtao::fill_result = false;
    assert(!prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 1 && mp6::xegtao::fill_calls == 1);
    assert(g_xegtao.initialize_calls == 0 && !g_xegtao.unavailable());
    mp6::xegtao::fill_result = true;
    assert(prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 2 && mp6::xegtao::fill_calls == 2);
    assert(g_xegtao.initialize_calls == 1);

    // A normal opt-in frame fills constants before the one real init attempt.
    clear_counters();
    g_xegtao = {};
    assert(prepare(true, 1920, 1080, 1920, 1080));
    assert(mp6::xegtao::working_calls == 1 && mp6::xegtao::fill_calls == 1);
    assert(g_xegtao.initialize_calls == 1);
    assert(XeGTAO::GTAOConstants::constructions == 1);
    return 0;
}
"""


class AoUpstreamPreparationTests(unittest.TestCase):
    def test_extracted_preparation_control_flow(self):
        with tempfile.TemporaryDirectory(prefix="ao-upstream-prep-", dir=ROOT / "build") as folder:
            directory = Path(folder)
            source = directory / "preparation.cpp"
            executable = directory / "preparation.exe"
            source.write_text(CPP_FACADE, encoding="utf-8")
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG",
                 str(source), "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_source_wiring_and_ordering(self):
        source = AMBIENT.read_text(encoding="utf-8")
        native = NATIVE.read_text(encoding="utf-8")
        block = preparation_block()

        self.assertIn("#include <optional>", source)
        self.assertIn("bool unavailable() const noexcept", native)
        self.assertIn("return checked && !supported;", native)
        self.assertIn("g_xegtaoRequested && !g_xegtao.unavailable()", block)
        self.assertIn("std::optional<XeGTAO::GTAOConstants> upstreamConstants", source)
        self.assertIn("upstreamConstants.emplace()", block)
        self.assertLess(block.index("working_size"), block.index("fill_parameters"))
        self.assertLess(block.index("fill_parameters"), block.index("initialize"))
        self.assertIn("*upstreamConstants", block)
        self.assertIn("upstreamConstants->DepthUnpackConsts", source)
        self.assertIn("job.depth, *upstreamConstants, adapter", source)


if __name__ == "__main__":
    unittest.main()
